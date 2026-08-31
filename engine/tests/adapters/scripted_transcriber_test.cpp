#include "adapters/transcription/scripted_transcriber.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ambient::asr {
namespace {

struct RecordingSink : ITurnSink {
    std::vector<Turn> turns;

    void OnTurn(const Turn& turn) override {
        turns.push_back(turn);
    }
};

TEST(ScriptedTranscriber, EmitsOneTurnPerWindowWithItsTiming) {
    RecordingSink sink;
    ScriptedTranscriber transcriber;
    transcriber.Begin(sink);

    const std::vector<float> window(160);
    transcriber.Submit(window, 0);
    transcriber.Submit(window, 160);
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 2u);
    EXPECT_EQ(sink.turns[0].first_frame, 0u);
    EXPECT_EQ(sink.turns[0].frame_count, 160u);
    EXPECT_EQ(sink.turns[1].first_frame, 160u);
    EXPECT_TRUE(sink.turns[0].speaker.empty());
    EXPECT_FALSE(sink.turns[0].text.empty());
}

TEST(ScriptedTranscriber, TurnsAreDeterministic) {
    RecordingSink first_sink;
    RecordingSink second_sink;
    ScriptedTranscriber first;
    ScriptedTranscriber second;
    first.Begin(first_sink);
    second.Begin(second_sink);

    const std::vector<float> window(320);
    first.Submit(window, 0);
    second.Submit(window, 0);

    ASSERT_EQ(first_sink.turns.size(), 1u);
    EXPECT_EQ(first_sink.turns[0].text, second_sink.turns[0].text);
}

TEST(ScriptedTranscriber, BeginResetsTheScript) {
    RecordingSink sink;
    ScriptedTranscriber transcriber;
    const std::vector<float> window(160);

    transcriber.Begin(sink);
    transcriber.Submit(window, 0);
    const std::string first_text = sink.turns[0].text;

    transcriber.Begin(sink);
    transcriber.Submit(window, 0);
    EXPECT_EQ(sink.turns[1].text, first_text) << "a new session starts the script over";
}

}  // namespace
}  // namespace ambient::asr
