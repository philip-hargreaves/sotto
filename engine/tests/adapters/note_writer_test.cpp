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
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts" / "note-narrative.md");

    std::vector<std::string> partials;
    const std::string text =
        writer.Write(ElbowTranscript(),
                     [&partials](const std::string& partial) { partials.push_back(partial); });

    ASSERT_FALSE(text.empty());
    EXPECT_GT(partials.size(), 3u) << "the note must stream, not arrive at once";
    EXPECT_TRUE(partials.back().starts_with(text)) << "the final text is the trimmed last partial";
    EXPECT_EQ(text.find("doctor"), std::string::npos) << "the register bans the word";
}

TEST(QwenNoteWriter, CancelInterruptsAGeneration) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4")) {
        GTEST_SKIP() << "note model not staged";
    }
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts" / "note-narrative.md");

    int seen = 0;
    const std::string text = writer.Write(ElbowTranscript(), [&writer, &seen](const std::string&) {
        if (++seen == 3) writer.Cancel();
    });

    EXPECT_GE(seen, 3);
    EXPECT_LT(seen, 40) << "cancel must stop generation promptly";
}

TEST(QwenNoteWriter, AnEmptyTranscriptRefusesToWrite) {
    models::ModelStore store(kModels);
    models::OvRuntime runtime;
    QwenNoteWriter writer(store, runtime, kModels.parent_path() / "prompts" / "note-narrative.md");
    EXPECT_THROW(writer.Write({}, nullptr), std::runtime_error);
}

}  // namespace
}  // namespace sotto::note
