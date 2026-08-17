#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/transcription/whisper_transcriber.hpp"

namespace sotto::asr {
namespace {

struct RecordingSink : ITurnSink {
    std::mutex mutex;
    std::vector<Turn> turns;

    void OnTurn(const Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }
};

Turn Labelled(std::uint64_t first_frame, std::size_t count) {
    Turn turn;
    turn.first_frame = first_frame;
    turn.frame_count = count;
    turn.text = "w" + std::to_string(first_frame);
    return turn;
}

TEST(WhisperWorker, SubmitDoesNotBlockWhileADecodeIsInFlight) {
    std::promise<void> release;
    auto released = release.get_future().share();
    RecordingSink sink;
    WhisperTranscriber transcriber([released](std::span<const float> f, std::uint64_t first) {
        released.wait();
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);

    const std::vector<float> window(100);
    transcriber.Submit(window, 0);

    const auto start = std::chrono::steady_clock::now();
    transcriber.Submit(window, 100);
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_LT(took, std::chrono::milliseconds(50)) << "Submit must only enqueue";

    release.set_value();
    transcriber.Finish();
    EXPECT_EQ(sink.turns.size(), 2u);
}

TEST(WhisperWorker, TurnsArriveInSubmitOrder) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    for (std::uint64_t i = 0; i < 5; ++i) transcriber.Submit(window, i * 10);
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 5u);
    for (std::uint64_t i = 0; i < 5; ++i) EXPECT_EQ(sink.turns[i].first_frame, i * 10);
}

TEST(WhisperWorker, FinishWaitsForTheLastDecode) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    for (std::uint64_t i = 0; i < 3; ++i) transcriber.Submit(window, i * 10);
    transcriber.Finish();

    EXPECT_EQ(sink.turns.size(), 3u);
}

TEST(WhisperWorker, AThrowingDecodeLosesOnlyItsWindow) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        if (first == 10) throw std::runtime_error("decode failed");
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    for (std::uint64_t i = 0; i < 3; ++i) transcriber.Submit(window, i * 10);
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 2u);
    EXPECT_EQ(sink.turns[0].first_frame, 0u);
    EXPECT_EQ(sink.turns[1].first_frame, 20u);
}

TEST(WhisperWorker, WindowsQueuedDuringTheLoadDecodeOnceItCompletes) {
    std::promise<void> release;
    auto released = release.get_future().share();
    RecordingSink sink;
    // The loader stands in for pipeline compilation; the session records
    // and submits throughout
    WhisperTranscriber transcriber(DecodeLoader([released]() -> DecodeFn {
        released.wait();
        return [](std::span<const float> f, std::uint64_t first) {
            return std::vector<Turn>{Labelled(first, f.size())};
        };
    }));
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    for (std::uint64_t i = 0; i < 4; ++i) transcriber.Submit(window, i * 10);
    EXPECT_TRUE(sink.turns.empty()) << "nothing decodes before the model is ready";

    release.set_value();
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 4u);
    for (std::uint64_t i = 0; i < 4; ++i) EXPECT_EQ(sink.turns[i].first_frame, i * 10);
}

TEST(WhisperWorker, AFailedLoadDrainsWindowsWithoutTurnsOrHangs) {
    RecordingSink sink;
    WhisperTranscriber transcriber(
        DecodeLoader([]() -> DecodeFn { throw std::runtime_error("no GPU"); }));
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    for (std::uint64_t i = 0; i < 3; ++i) transcriber.Submit(window, i * 10);
    transcriber.Finish();

    EXPECT_TRUE(sink.turns.empty());
}

TEST(WhisperWorker, BeginPointsTurnsAtTheNewSink) {
    RecordingSink first_sink;
    RecordingSink second_sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        return std::vector<Turn>{Labelled(first, f.size())};
    });

    const std::vector<float> window(10);
    transcriber.Begin(first_sink);
    transcriber.Submit(window, 0);
    transcriber.Finish();

    transcriber.Begin(second_sink);
    transcriber.Submit(window, 0);
    transcriber.Finish();

    EXPECT_EQ(first_sink.turns.size(), 1u);
    EXPECT_EQ(second_sink.turns.size(), 1u);
}

}  // namespace
}  // namespace sotto::asr
