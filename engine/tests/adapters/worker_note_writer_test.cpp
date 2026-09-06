#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format off
#include <tlhelp32.h>
// clang-format on

#include "adapters/models/model_store.hpp"
#include "adapters/note/worker_note_writer.hpp"

namespace ambient::note {
namespace {

const std::filesystem::path kModels = AMBIENT_MODELS_DIR;

std::filesystem::path HostExe() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().parent_path() / "ambient_note_host.exe";
}

std::filesystem::path PromptPath() {
    return kModels.parent_path() / "prompts";
}

std::vector<asr::Turn> ElbowTranscript() {
    return {{0, 16000, "doctor", "What seems to be the problem today?"},
            {16000, 32000, "patient",
             "I noticed a swelling on my left elbow about a week ago. It is not painful, "
             "just slightly warm, and it feels like there is fluid inside."},
            {48000, 16000, "doctor", "Have you injured that elbow at all?"},
            {64000, 16000, "patient", "No, not that I know of."},
            {80000, 32000, "doctor",
             "This looks like bursitis. I would take ibuprofen, four hundred milligrams "
             "twice a day after food, and we will arrange blood tests."}};
}

TEST(WorkerNoteWriter, WritesNoteAndSheetThroughTheWorkerProcess) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4") ||
        !std::filesystem::exists(HostExe())) {
        GTEST_SKIP() << "note model or host not staged";
    }
#ifdef _DEBUG
    const bool debug_build = true;
#else
    const bool debug_build = false;
#endif
    if (debug_build) {
        GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts on a second generation";
    }
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath());
    writer.Prepare();

    int partials = 0;
    const std::string note =
        writer.Write(ElbowTranscript(), {}, [&partials](const std::string&) { partials++; });
    ASSERT_FALSE(note.empty());
    EXPECT_GT(partials, 3) << "the note must stream through the pipe";
    EXPECT_EQ(note.find("doctor"), std::string::npos);

    const std::string sheet = writer.WritePatient(note, nullptr);
    ASSERT_FALSE(sheet.empty());
    EXPECT_NE(sheet.find("Your appointment today"), std::string::npos);
    EXPECT_NE(sheet.find("When to contact us"), std::string::npos);
}

TEST(WorkerNoteWriter, AKilledWorkerRespawnsAndTheNoteStillArrives) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4") ||
        !std::filesystem::exists(HostExe())) {
        GTEST_SKIP() << "note model or host not staged";
    }
#ifdef _DEBUG
    const bool debug_build = true;
#else
    const bool debug_build = false;
#endif
    if (debug_build) {
        GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts on a second generation";
    }
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath());
    writer.Prepare();

    std::atomic<bool> killed{false};
    const std::string note =
        writer.Write(ElbowTranscript(), {}, [&killed](const std::string& partial) {
            // The first streamed words prove generation is mid-flight; then
            // the worker dies under it
            if (partial.size() > 20 && !killed.exchange(true)) {
                std::system("taskkill /IM ambient_note_host.exe /F >nul 2>&1");
            }
        });

    EXPECT_TRUE(killed.load());
    ASSERT_FALSE(note.empty());
    EXPECT_EQ(note.find("doctor"), std::string::npos);
}

// The accuracy tier through the manifest's pipeline (the 35B's multimodal
// export, text-only): the lane switches, the host loads it, a note arrives
TEST(WorkerNoteWriter, TheAccuracyTierWritesANoteThroughItsOwnPipeline) {
    if (!std::filesystem::exists(kModels / "qwen3.6-35b-a3b-int4") ||
        !std::filesystem::exists(HostExe())) {
        GTEST_SKIP() << "accuracy note model or host not staged";
    }
#ifdef _DEBUG
    const bool debug_build = true;
#else
    const bool debug_build = false;
#endif
    if (debug_build) {
        GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts on a second generation";
    }
    const models::ModelStore store(kModels);
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath(), &store);
    const auto switched = writer.Configure("accuracy");
    ASSERT_EQ(switched.tier, "accuracy");
    ASSERT_EQ(switched.id, "qwen3.6-35b-a3b-int4");

    int partials = 0;
    const std::string note =
        writer.Write(ElbowTranscript(), {}, [&partials](const std::string&) { partials++; });
    ASSERT_FALSE(note.empty());
    EXPECT_GT(partials, 3);
    EXPECT_EQ(note.find("doctor"), std::string::npos);
    const auto state = writer.State();
    EXPECT_EQ(state.phase, NoteModelState::Phase::kReady);
    EXPECT_GT(state.seconds, 0.0) << "the host reported its load";
    std::fprintf(stderr, "accuracy tier: loaded in %.1f s, note %zu chars\n", state.seconds,
                 note.size());
}

// One tier through the real host, for the per-tier sweep: AMBIENT_SWEEP_TIER
// names it; the host's own log carries verify, load and decode figures
TEST(WorkerNoteWriter, NoteTierSweep) {
    char* wanted = nullptr;
    const std::string tier =
        _dupenv_s(&wanted, nullptr, "AMBIENT_SWEEP_TIER") == 0 && wanted != nullptr ? wanted : "";
    std::free(wanted);
    if (tier.empty()) GTEST_SKIP() << "set AMBIENT_SWEEP_TIER to a tier";
    if (!std::filesystem::exists(HostExe())) GTEST_SKIP() << "host not staged";
    const models::ModelStore store(kModels);
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath(), &store);
    const auto t0 = std::chrono::steady_clock::now();
    const auto switched = writer.Configure(tier);
    ASSERT_EQ(switched.tier, tier);
    const std::string note = writer.Write(ElbowTranscript(), {}, nullptr);
    const double to_note =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_FALSE(note.empty());
    const std::string sheet = writer.WritePatient(note, nullptr);
    const auto state = writer.State();
    std::fprintf(stderr,
                 "sweep %s: %s, host load %.1f s, configure-to-note %.1f s, note %zu chars, sheet "
                 "%zu chars\n",
                 tier.c_str(), state.id.c_str(), state.seconds, to_note, note.size(), sheet.size());
}

// Every live note host process by pid
std::vector<DWORD> NoteHostPids() {
    std::vector<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"ambient_note_host.exe") == 0) {
            pids.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return pids;
}

// The zombie probe: a host terminated part-way through loading the 35B, the
// way a tier switch, a respawn or an engine exit terminates it. Either the
// process is gone within 30 s or it is a process wedged inside a GPU driver
// call, which nothing but a reboot removes. Opt-in (AMBIENT_ZOMBIE_PROBE=1):
// a positive result costs the machine a hard reset
void TerminateLoadingHostAfter(int seconds) {
    if (!std::filesystem::exists(kModels / "qwen3.6-35b-a3b-int4") ||
        !std::filesystem::exists(HostExe())) {
        GTEST_SKIP() << "accuracy note model or host not staged";
    }
    char* armed = nullptr;
    const bool run = _dupenv_s(&armed, nullptr, "AMBIENT_ZOMBIE_PROBE") == 0 && armed != nullptr &&
                     std::string(armed) == "1";
    std::free(armed);
    if (!run) GTEST_SKIP() << "set AMBIENT_ZOMBIE_PROBE=1 to run (may need a reboot after)";

    const models::ModelStore store(kModels);
    const auto before = NoteHostPids();
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath(), &store);
    writer.Configure("accuracy");
    DWORD victim = 0;
    for (const DWORD pid : NoteHostPids()) {
        if (std::find(before.begin(), before.end(), pid) == before.end()) victim = pid;
    }
    ASSERT_NE(victim, 0u) << "the loading host was not found";
    std::fprintf(stderr, "zombie probe: host %lu loading; terminating after %d s\n", victim,
                 seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    EXPECT_EQ(writer.State().phase, NoteModelState::Phase::kLoading)
        << "the host finished before the kill; choose a shorter delay";

    // The engine's own path: pipe closed, 2 s grace, TerminateProcess
    writer.Configure("default");

    bool gone = false;
    for (int i = 0; i < 60 && !gone; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = NoteHostPids();
        gone = std::find(now.begin(), now.end(), victim) == now.end();
    }
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, victim);
    DWORD exit_code = 0;
    const bool signalled =
        handle != nullptr && GetExitCodeProcess(handle, &exit_code) && exit_code != STILL_ACTIVE;
    if (handle != nullptr) CloseHandle(handle);
    std::fprintf(stderr, "zombie probe: host %lu after 30 s: %s (exit code %lu)\n", victim,
                 gone ? "gone" : "STILL LISTED", exit_code);
    EXPECT_TRUE(gone || signalled)
        << "host " << victim << " survived TerminateProcess: wedged in a driver call";
}

TEST(WorkerNoteWriter, ZombieProbeTerminatedWhileHashing) {
    TerminateLoadingHostAfter(10);  // inside the SHA-256 pass: CPU only, no GPU call
}

TEST(WorkerNoteWriter, ZombieProbeTerminatedWhileCompiling) {
    TerminateLoadingHostAfter(70);  // inside the GPU build of the language model
}

TEST(WorkerNoteWriter, AMissingHostFailsLoudly) {
    WorkerNoteWriter writer("C:/nowhere/ambient_note_host.exe", kModels, PromptPath());
    EXPECT_THROW(writer.Write(ElbowTranscript(), {}, nullptr), std::runtime_error);
}

TEST(WorkerNoteWriter, AnEmptyTranscriptRefusesToWrite) {
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath());
    EXPECT_THROW(writer.Write({}, {}, nullptr), std::runtime_error);
}

}  // namespace
}  // namespace ambient::note
