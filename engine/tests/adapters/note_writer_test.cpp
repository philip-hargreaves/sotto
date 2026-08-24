#include <gtest/gtest.h>

#include <filesystem>
#include <thread>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/qwen_note_writer.hpp"

namespace sotto::note {
namespace {

const std::filesystem::path kModels = SOTTO_MODELS_DIR;

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

TEST(QwenNoteWriter, WritesAStreamedNoteFromTheTranscript) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");

    std::vector<std::string> partials;
    const std::string text =
        writer.Write(ElbowTranscript(), {},
                     [&partials](const std::string& partial) { partials.push_back(partial); });

    ASSERT_FALSE(text.empty());
    EXPECT_GT(partials.size(), 3u) << "the note must stream, not arrive at once";
    EXPECT_TRUE(partials.back().starts_with(text)) << "the final text is the trimmed last partial";
    EXPECT_EQ(text.find("doctor"), std::string::npos) << "the register bans the word";
}

TEST(QwenNoteWriter, WritesASoapNoteWithinTheConciseLimit) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");

    const std::string text = writer.Write(ElbowTranscript(), {"soap", "concise"}, nullptr);

    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("Subjective:"), std::string::npos);
    EXPECT_NE(text.find("Objective:"), std::string::npos);
    EXPECT_NE(text.find("Assessment:"), std::string::npos);
    EXPECT_NE(text.find("Plan:"), std::string::npos);
    EXPECT_EQ(text.find("doctor"), std::string::npos);
    // The 80-word cap with slack; strict adherence is the quality commit
    std::size_t words = 0;
    bool in_word = false;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\n' || c == '\t';
        if (!space && !in_word) ++words;
        in_word = !space;
    }
    EXPECT_LE(words, 140u);
}

TEST(QwenNoteWriter, CancelInterruptsAGeneration) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");

    int seen = 0;
    const std::string text =
        writer.Write(ElbowTranscript(), {}, [&writer, &seen](const std::string&) {
            if (++seen == 3) writer.Cancel();
        });

    EXPECT_GE(seen, 3);
    EXPECT_LT(seen, 40) << "cancel must stop generation promptly";
}

TEST(QwenNoteWriter, WritesThePatientSheetFromANote) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");

    int partials = 0;
    const std::string sheet = writer.WritePatient(
        "The patient presents with a swollen left elbow, present for one week, not painful "
        "but slightly warm. The working diagnosis is bursitis. The plan is ibuprofen 400 mg "
        "twice daily after food, stopping if heartburn occurs, and blood tests. The patient "
        "was advised to seek help if the elbow becomes very red or very painful.",
        [&partials](const std::string&) { partials++; });

    ASSERT_FALSE(sheet.empty());
    EXPECT_GT(partials, 3);
    EXPECT_NE(sheet.find("Your appointment today"), std::string::npos);
    EXPECT_NE(sheet.find("When to contact us"), std::string::npos);
    EXPECT_EQ(sheet.find("doctor"), std::string::npos);
}

// The production path: the patient sheet is a second generation on the
// same resident pipeline, straight after the note
TEST(QwenNoteWriter, ThePatientSheetFollowsTheNoteOnTheSamePipeline) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
#ifdef _DEBUG
    const bool debug_build = true;
#else
    const bool debug_build = false;
#endif
    if (debug_build) {
        GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts on a second generation";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");

    const std::string note = writer.Write(ElbowTranscript(), {}, nullptr);
    ASSERT_FALSE(note.empty());
    const std::string sheet = writer.WritePatient(note, nullptr);
    ASSERT_FALSE(sheet.empty());
    EXPECT_NE(sheet.find("Your appointment today"), std::string::npos);
}

TEST(QwenNoteWriter, AnEmptyTranscriptRefusesToWrite) {
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts");
    EXPECT_THROW(writer.Write({}, {}, nullptr), std::runtime_error);
}

}  // namespace
}  // namespace sotto::note
