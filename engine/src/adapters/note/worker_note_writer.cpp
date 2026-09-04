#include "adapters/note/worker_note_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nlohmann/json.hpp>

#include "adapters/host/power_throttling.hpp"
#include "adapters/ipc/framing.hpp"

namespace ambient::note {

using nlohmann::json;

struct WorkerNoteWriter::Impl {
    std::filesystem::path host_exe;
    std::filesystem::path models_root;
    std::filesystem::path prompt_path;

    std::mutex state_mutex;  // guards spawn and the handles
    std::mutex write_mutex;  // frames interleave whole, never torn
    HANDLE process = nullptr;
    HANDLE job = nullptr;  // kill-on-close: the engine's death is the worker's
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ipc::FrameDecoder decoder;
    std::int64_t next_id = 1;
    int spawn_count = 0;
    bool closing = false;
    std::atomic<bool> attempt_active{false};  // the note thread owns the pipe's read side

    // A generation streams partials constantly; this much silence means the
    // worker is wedged inside a driver call and only a respawn recovers it
    static constexpr DWORD kInactivityTimeoutMs = 120'000;

    bool WorkerAlive() const {
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT &&
               pipe != INVALID_HANDLE_VALUE;
    }

    void CloseWorker() {
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (process != nullptr) {
            // Losing the pipe ends the host's serve loop; give it that exit
            if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
            }
            CloseHandle(process);
            process = nullptr;
        }
        if (job != nullptr) {
            CloseHandle(job);
            job = nullptr;
        }
        decoder = ipc::FrameDecoder{};
    }

    // Spawns the host and connects its private pipe; throws when the host
    // cannot start, which surfaces as a failed note
    void EnsureWorker() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (WorkerAlive()) {
            return;
        }
        CloseWorker();

        const std::wstring pipe_path = L"\\\\.\\pipe\\LOCAL\\ambient-note-" +
                                       std::to_wstring(GetCurrentProcessId()) + L"-" +
                                       std::to_wstring(++spawn_count);
        std::wstring command = L"\"" + host_exe.wstring() + L"\" \"" + pipe_path + L"\" \"" +
                               models_root.wstring() + L"\" \"" + prompt_path.wstring() + L"\"";
        // The worker shares the engine's stderr so its diagnostics land in
        // the same log
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        // Suspended until it is in the job, so it can never outlive the engine
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(host_exe.wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                            &info)) {
            throw std::runtime_error("note worker failed to start");
        }
        job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));
            AssignProcessToJobObject(job, info.hProcess);
        }
        host::DisableThrottling(info.hProcess);  // the host repeats this on itself
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process = info.hProcess;

        // The host claims the pipe before any model work, so this is quick
        for (int attempt = 0; attempt < 150; ++attempt) {
            pipe = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                               nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                break;
            }
            if (WaitForSingleObject(process, 100) != WAIT_TIMEOUT) {
                break;  // died before serving
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            CloseWorker();
            throw std::runtime_error("note worker pipe did not open");
        }
        ULONG server_pid = 0;
        if (!GetNamedPipeServerProcessId(pipe, &server_pid) ||
            server_pid != GetProcessId(process)) {
            CloseWorker();
            throw std::runtime_error("note worker pipe is not the spawned process");
        }
    }

    void Send(const std::string& method, json params) {
        json request{{"jsonrpc", "2.0"},
                     {"id", next_id++},
                     {"method", method},
                     {"params", std::move(params)}};
        const std::string frame =
            ipc::EncodeFrame(request.dump(-1, ' ', false, json::error_handler_t::replace));
        std::lock_guard<std::mutex> lock(write_mutex);
        DWORD written = 0;
        if (!WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) ||
            written != frame.size()) {
            throw std::runtime_error("note worker went away");
        }
    }

    // Bounded: a worker wedged inside a driver call must not wedge the
    // note thread with it
    json ReadMessage() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kInactivityTimeoutMs);
        for (;;) {
            if (auto payload = decoder.Next()) {
                return json::parse(*payload, nullptr, false);
            }
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                throw std::runtime_error("note worker died");
            }
            if (available == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("note worker stopped responding");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            char buffer[64 * 1024];
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                throw std::runtime_error("note worker died");
            }
            decoder.Push({buffer, read});
        }
    }

    // Prefill acks pile up unread between attempts; drained here so the
    // host's pipe writes never block. Only when no attempt owns the reads
    void DrainAcks() {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(pipe, buffer,
                          static_cast<DWORD>(std::min<DWORD>(available, sizeof(buffer))), &read,
                          nullptr) ||
                read == 0) {
                return;
            }
            decoder.Push({buffer, read});
            while (decoder.Next()) {
            }
        }
    }

    // One streamed attempt: request, then read until the worker settles it
    std::string Attempt(const std::string& method, const json& params, const Progress& progress) {
        struct ActiveFlag {
            std::atomic<bool>& flag;
            explicit ActiveFlag(std::atomic<bool>& f) : flag(f) {
                flag = true;
            }
            ~ActiveFlag() {
                flag = false;
            }
        } active{attempt_active};
        EnsureWorker();
        Send(method, params);
        for (;;) {
            const json message = ReadMessage();
            if (message.is_discarded() || !message.is_object()) {
                throw std::runtime_error("note worker spoke garbage");
            }
            if (message.contains("error")) {
                throw std::runtime_error(
                    message["error"].value("data", message["error"].value("message", "failed")));
            }
            if (!message.contains("method")) {
                continue;  // an ack, ours or an earlier prepare's
            }
            const auto& event = message["method"].get_ref<const std::string&>();
            const json& p = message.value("params", json::object());
            if (event == "partial" && progress) {
                progress(p.value("text", ""));
            } else if (event == "ready") {
                return p.value("text", "");
            } else if (event == "failed") {
                throw std::runtime_error(p.value("detail", "note generation failed"));
            }
        }
    }

    // One fresh process before failing: the fresh-context retry is the
    // configuration measured to work
    std::string Run(const std::string& method, json params, const Progress& progress) {
        try {
            return Attempt(method, params, progress);
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (closing) throw;
                std::fprintf(stderr, "ambient-engine: note worker failed (%.100s); respawning\n",
                             e.what());
                CloseWorker();
            }
            return Attempt(method, params, progress);
        }
    }
};

WorkerNoteWriter::WorkerNoteWriter(std::filesystem::path host_exe,
                                   std::filesystem::path models_root,
                                   std::filesystem::path prompt_path)
    : impl_(new Impl{std::move(host_exe), std::move(models_root), std::move(prompt_path)}) {}

WorkerNoteWriter::~WorkerNoteWriter() {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->closing = true;
    impl_->CloseWorker();
}

// Spawn and load hide inside capture; AMBIENT_NOTE_LOAD=stop defers the load
// (co-residency experiment knob). Failure surfaces on Write
void WorkerNoteWriter::Prepare() {
    static const bool load_at_stop = [] {
        char* value = nullptr;
        const bool at_stop = _dupenv_s(&value, nullptr, "AMBIENT_NOTE_LOAD") == 0 &&
                             value != nullptr && std::string(value) == "stop";
        std::free(value);
        return at_stop;
    }();
    try {
        impl_->EnsureWorker();
        if (!load_at_stop) {
            impl_->Send("prepare", json::object());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: note worker prepare failed (%s)\n", e.what());
    }
}

namespace {

json TurnsJson(const std::vector<asr::Turn>& transcript) {
    json turns = json::array();
    for (const auto& turn : transcript) {
        turns.push_back({{"firstFrame", turn.first_frame},
                         {"frameCount", turn.frame_count},
                         {"speaker", turn.speaker},
                         {"text", turn.text}});
    }
    return turns;
}

}  // namespace

void WorkerNoteWriter::Prefill(const std::vector<asr::Turn>& transcript,
                               const NoteOptions& options) {
    if (transcript.empty() || impl_->attempt_active.load()) return;
    try {
        impl_->EnsureWorker();
        impl_->DrainAcks();
        impl_->Send("prefill", {{"turns", TurnsJson(transcript)}, {"style", options.style}});
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: note prefill not sent (%.100s)\n", e.what());
    }
}

std::string WorkerNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                    const NoteOptions& options, const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    return impl_->Run(
        "write",
        {{"turns", TurnsJson(transcript)}, {"style", options.style}, {"detail", options.detail}},
        progress);
}

std::string WorkerNoteWriter::WritePatient(const std::string& note, const Progress& progress) {
    if (note.empty()) {
        throw std::runtime_error("nothing to write: the note is empty");
    }
    return impl_->Run("writePatient", {{"note", note}}, progress);
}

// A failed title is no title, never a failed note: no respawn, no throw
std::string WorkerNoteWriter::WriteLabel(const std::string& note) {
    if (note.empty()) {
        return {};
    }
    try {
        return impl_->Attempt("label", {{"note", note}}, nullptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: no label (%.100s)\n", e.what());
        return {};
    }
}

void WorkerNoteWriter::Cancel() {
    try {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->WorkerAlive()) {
            impl_->Send("cancel", json::object());
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
}

}  // namespace ambient::note
