#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/note/worker_note_writer.hpp"

namespace sotto::note {
namespace {

const std::filesystem::path kModels = SOTTO_MODELS_DIR;

std::filesystem::path HostExe() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().parent_path() / "sotto_note_host.exe";
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
                std::system("taskkill /IM sotto_note_host.exe /F >nul 2>&1");
            }
        });

    EXPECT_TRUE(killed.load());
    ASSERT_FALSE(note.empty());
    EXPECT_EQ(note.find("doctor"), std::string::npos);
}

TEST(WorkerNoteWriter, AMissingHostFailsLoudly) {
    WorkerNoteWriter writer("C:/nowhere/sotto_note_host.exe", kModels, PromptPath());
    EXPECT_THROW(writer.Write(ElbowTranscript(), {}, nullptr), std::runtime_error);
}

TEST(WorkerNoteWriter, AnEmptyTranscriptRefusesToWrite) {
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath());
    EXPECT_THROW(writer.Write({}, {}, nullptr), std::runtime_error);
}

}  // namespace
}  // namespace sotto::note
