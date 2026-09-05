#include "adapters/note/worker_note_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nlohmann/json.hpp>

#include "adapters/host/gpu_lease.hpp"
#include "adapters/host/power_throttling.hpp"
#include "adapters/ipc/framing.hpp"
#include "adapters/models/model_store.hpp"

namespace ambient::note {

using nlohmann::json;

namespace {

// The GPU lease gave up waiting on the host: it is wedged in a driver
// call; the lane reports it rather than retrying
constexpr const char* kWedged =
    "the note process is stuck in the graphics driver; restart the computer";

}  // namespace

struct WorkerNoteWriter::Impl {
    std::filesystem::path host_exe;
    std::filesystem::path models_root;
    std::filesystem::path prompt_path;
    const models::ModelStore* store;

    std::mutex state_mutex;  // guards spawn and the handles
    std::mutex write_mutex;  // frames interleave whole, never torn
    std::mutex read_mutex;   // one reader of the pipe at a time: the attempt or the watcher
    HANDLE process = nullptr;
    HANDLE job = nullptr;  // kill-on-close: the engine's death is the worker's
    HANDLE pipe = INVALID_HANDLE_VALUE;
    ipc::FrameDecoder decoder;
    std::int64_t next_id = 1;
    // Process-wide: a host winding down keeps its pipe name briefly; a
    // second writer must not reuse it
    static inline std::atomic<int> spawn_count{0};
    bool closing = false;
    bool respawning = false;                  // state_mutex; Run is between its two attempts
    std::atomic<bool> attempt_active{false};  // the note thread owns the pipe's read side

    // The lane: which tier, whether resident. lane_mutex is never held
    // across a call into the host or the listener
    mutable std::mutex lane_mutex;
    NoteModelState state;
    Listener listener;
    std::thread watcher;  // reads the host's load outcome while nothing else reads
    std::atomic<bool> watch_stop{false};

    // A generation streams partials constantly; this much silence means the
    // worker is wedged inside a driver call and only a respawn recovers it.
    // A request queued behind a load is silent for as long as the load
    // takes (hash + compile of a 19 GB model: minutes), so that wait has
    // its own, longer bound
    static constexpr DWORD kInactivityTimeoutMs = 120'000;
    static constexpr DWORD kLoadTimeoutMs = 20 * 60'000;

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

    std::string Tier() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.tier;
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
        const std::string tier = Tier();
        std::wstring command = L"\"" + host_exe.wstring() + L"\" \"" + pipe_path + L"\" \"" +
                               models_root.wstring() + L"\" \"" + prompt_path.wstring() +
                               L"\" \"" + std::wstring(tier.begin(), tier.end()) + L"\"";
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

    // ---- the lane -------------------------------------------------------

    void Transition(const std::function<void(NoteModelState&)>& mutate) {
        NoteModelState snapshot;
        Listener notify;
        {
            std::lock_guard<std::mutex> lock(lane_mutex);
            mutate(state);
            snapshot = state;
            notify = listener;
        }
        if (notify) notify(snapshot);
    }

    NoteModelState::Phase Phase() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase;
    }

    // Names the model a tier resolves to, and whether its compile cache
    // exists; throws the store's own message when nothing claims the tier
    void Describe(const std::string& tier, NoteModelState& into) const {
        if (store == nullptr) {
            into.id.clear();
            into.name.clear();
            into.first_use = false;
            return;
        }
        const models::ModelInfo& info = store->Resolve("note", tier);
        into.id = info.id;
        into.name = info.name;
        into.first_use = !std::filesystem::exists(info.dir / ".cache");
    }

    // The host's two load outcomes, from whichever reader saw them
    void OnHostEvent(const std::string& event, const json& params) {
        if (event == "loaded") {
            Transition([&params](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kReady;
                s.detail.clear();
                s.seconds = params.value("seconds", 0.0);
                if (params.contains("id")) s.id = params.value("id", s.id);
                if (params.contains("name")) s.name = params.value("name", s.name);
                s.first_use = params.value("firstUse", s.first_use);
            });
        } else if (event == "loadFailed") {
            Transition([&params](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kFailed;
                s.detail = params.value("detail", "note model failed to load");
            });
        }
    }

    // Reads whatever frames are waiting and dispatches load events; the
    // caller holds read_mutex. False when the pipe is gone
    bool PumpFrames() {
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                return true;
            }
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(pipe, buffer,
                          static_cast<DWORD>(std::min<DWORD>(available, sizeof(buffer))), &read,
                          nullptr) ||
                read == 0) {
                return false;
            }
            decoder.Push({buffer, read});
            while (auto payload = decoder.Next()) {
                const json message = json::parse(*payload, nullptr, false);
                if (message.is_object() && message.contains("method")) {
                    OnHostEvent(message["method"].get<std::string>(),
                                message.value("params", json::object()));
                }
            }
        }
    }

    // Reads the host's load outcome while no attempt owns the pipe
    void StartWatcher() {
        StopWatcher();
        watch_stop = false;
        watcher = std::thread([this] {
            while (!watch_stop.load() && Phase() == NoteModelState::Phase::kLoading) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (attempt_active.load()) continue;
                std::lock_guard<std::mutex> reading(read_mutex);
                if (attempt_active.load()) continue;
                bool alive;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (closing || respawning) return;
                    alive = WorkerAlive();
                }
                if (!alive || !PumpFrames()) {
                    // Only a host that died on its own is a failure; a
                    // deliberate close stopped this thread first
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (closing || respawning) return;
                    Transition([](NoteModelState& s) {
                        if (s.phase != NoteModelState::Phase::kLoading) return;
                        s.phase = NoteModelState::Phase::kFailed;
                        s.detail = "note worker exited while loading";
                    });
                    return;
                }
            }
        });
    }

    void StopWatcher() {
        watch_stop = true;
        if (watcher.joinable()) {
            if (watcher.get_id() == std::this_thread::get_id()) {
                watcher.detach();
            } else {
                watcher.join();
            }
        }
    }

    // ---- attempts -------------------------------------------------------

    // Bounded: a worker wedged inside a driver call must not wedge the
    // note thread with it
    json ReadMessage() {
        const DWORD bound = Phase() == NoteModelState::Phase::kLoading ? kLoadTimeoutMs
                                                                       : kInactivityTimeoutMs;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bound);
        for (;;) {
            {
                std::lock_guard<std::mutex> reading(read_mutex);
                if (auto payload = decoder.Next()) {
                    return json::parse(*payload, nullptr, false);
                }
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                    throw std::runtime_error("note worker died");
                }
                if (available > 0) {
                    char buffer[64 * 1024];
                    DWORD read = 0;
                    if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                        throw std::runtime_error("note worker died");
                    }
                    decoder.Push({buffer, read});
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("note worker stopped responding");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Prefill acks pile up unread between attempts; drained here so the
    // host's pipe writes never block. Only when no attempt owns the reads
    void DrainAcks() {
        std::lock_guard<std::mutex> reading(read_mutex);
        PumpFrames();
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
            } else {
                OnHostEvent(event, p);  // a load settling under the request
            }
        }
    }

    // True, and the lane marked failed, once Whisper's bounded lease wait
    // has found the host wedged
    bool Wedged() {
        if (!host::GpuLease::Global().Broken()) return false;
        Transition([](NoteModelState& s) {
            if (s.phase == NoteModelState::Phase::kFailed && s.detail == kWedged) return;
            s.phase = NoteModelState::Phase::kFailed;
            s.detail = kWedged;
        });
        return true;
    }

    // One fresh process before failing: the fresh-context retry is the
    // configuration measured to work
    std::string Run(const std::string& method, json params, const Progress& progress) {
        if (Wedged()) throw std::runtime_error(kWedged);
        try {
            return Attempt(method, params, progress);
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (closing) throw;
                std::fprintf(stderr, "ambient-engine: note worker failed (%.100s); respawning\n",
                             e.what());
                respawning = true;
                CloseWorker();
            }
            try {
                const std::string text = Attempt(method, params, progress);
                std::lock_guard<std::mutex> lock(state_mutex);
                respawning = false;
                return text;
            } catch (const std::exception& again) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    respawning = false;
                }
                Transition([&again](NoteModelState& s) {
                    s.phase = NoteModelState::Phase::kFailed;
                    s.detail = again.what();
                });
                throw;
            }
        }
    }
};

WorkerNoteWriter::WorkerNoteWriter(std::filesystem::path host_exe,
                                   std::filesystem::path models_root,
                                   std::filesystem::path prompt_path,
                                   const models::ModelStore* store, std::string tier)
    : impl_(new Impl{std::move(host_exe), std::move(models_root), std::move(prompt_path), store}) {
    impl_->state.tier = std::move(tier);
    try {
        impl_->Describe(impl_->state.tier, impl_->state);
    } catch (const std::exception&) {
        // Nothing staged for the tier: the state says so with empty names
    }
}

WorkerNoteWriter::~WorkerNoteWriter() {
    impl_->StopWatcher();
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
    if (impl_->Wedged()) return;
    try {
        impl_->EnsureWorker();
        if (!load_at_stop) {
            bool starting = false;
            impl_->Transition([&starting](NoteModelState& s) {
                if (s.phase == NoteModelState::Phase::kReady) return;
                starting = s.phase != NoteModelState::Phase::kLoading;
                s.phase = NoteModelState::Phase::kLoading;
                s.detail.clear();
            });
            impl_->Send("prepare", json::object());
            if (starting) impl_->StartWatcher();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: note worker prepare failed (%s)\n", e.what());
        impl_->Transition([&e](NoteModelState& s) {
            s.phase = NoteModelState::Phase::kFailed;
            s.detail = e.what();
        });
    }
}

// A different tier is a new host; the same tier is a no-op unless its last
// load failed. Loads immediately so a failure surfaces at the setting
NoteModelState WorkerNoteWriter::Configure(const std::string& tier) {
    NoteModelState described;
    described.tier = tier;
    try {
        impl_->Describe(tier, described);
    } catch (const std::exception& e) {
        throw std::invalid_argument(e.what());
    }
    bool same;
    {
        std::lock_guard<std::mutex> lock(impl_->lane_mutex);
        same = impl_->state.tier == tier && impl_->state.phase != NoteModelState::Phase::kFailed;
    }
    if (same) {
        return State();
    }
    impl_->StopWatcher();
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->CloseWorker();
    }
    impl_->Transition([&described](NoteModelState& s) {
        s = described;
        s.phase = NoteModelState::Phase::kIdle;
    });
    Prepare();
    return State();
}

NoteModelState WorkerNoteWriter::State() const {
    std::lock_guard<std::mutex> lock(impl_->lane_mutex);
    return impl_->state;
}

void WorkerNoteWriter::SetListener(Listener listener) {
    std::lock_guard<std::mutex> lock(impl_->lane_mutex);
    impl_->listener = std::move(listener);
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
    if (transcript.empty() || impl_->attempt_active.load() || impl_->Wedged()) return;
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
    return impl_->Run("write",
                      {{"turns", TurnsJson(transcript)},
                       {"style", options.style},
                       {"detail", options.detail},
                       {"confirmed", options.confirmed}},
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
