#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/transcription/whisper_transcriber.hpp"
#include "core/metrics.hpp"

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

TEST(WhisperWorker, DecodesAccumulateIntoTheMetrics) {
    metrics::Registry registry;
    {
        RecordingSink sink;
        WhisperTranscriber transcriber(
            DecodeLoader([] {
                return DecodeFn(
                    [](std::span<const float>, std::uint64_t) { return std::vector<Turn>{}; });
            }),
            &registry);
        transcriber.Begin(sink);
        const std::vector<float> window(16000);
        transcriber.Submit(window, 0);
        transcriber.Submit(window, 16000);
        transcriber.Finish();
    }

    const auto s = registry.Take();
    EXPECT_EQ(s.decoded_audio_seconds, 2.0);
    EXPECT_GE(s.decode_busy_seconds, 0.0);
}

TEST(WhisperWorker, ReleaseFreesAndTheNextSubmitReloads) {
    std::atomic<int> loads{0};
    RecordingSink sink;
    WhisperTranscriber transcriber(DecodeLoader([&loads] {
        ++loads;
        return DecodeFn([](std::span<const float> f, std::uint64_t first) {
            return std::vector<Turn>{Labelled(first, f.size())};
        });
    }));
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    transcriber.Submit(window, 0);
    transcriber.Finish();
    EXPECT_EQ(loads.load(), 1);

    transcriber.Release();
    transcriber.Submit(window, 10);
    transcriber.Finish();
    EXPECT_EQ(loads.load(), 2) << "a released pipeline reloads on demand";
    EXPECT_EQ(sink.turns.size(), 2u);
}

TEST(WhisperWorker, ReleaseIsANoOpForAnInjectedDecode) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);
    transcriber.Release();

    const std::vector<float> window(10);
    transcriber.Submit(window, 0);
    transcriber.Finish();
    EXPECT_EQ(sink.turns.size(), 1u);
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

TEST(WhisperWorker, TurnsInTheReheardOverlapAreNotEmittedAgain) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float>, std::uint64_t first) {
        // Three turns: wholly re-heard, straddling the boundary, wholly new
        std::vector<Turn> turns;
        turns.push_back(Labelled(first, 500));         // midpoint 1250
        turns.push_back(Labelled(first + 800, 600));   // midpoint 2100
        turns.push_back(Labelled(first + 1500, 400));  // midpoint 2700
        return turns;
    });
    transcriber.Begin(sink);

    const std::vector<float> window(2000);
    transcriber.Submit(window, 1000, 2000);
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 2u) << "the re-heard turn must not duplicate";
    EXPECT_EQ(sink.turns[0].first_frame, 1800u) << "a straddler survives";
    EXPECT_EQ(sink.turns[1].first_frame, 2500u);
}

TEST(WhisperWorker, DecodeClipReturnsTheJoinedTurnTexts) {
    WhisperTranscriber transcriber([](std::span<const float>, std::uint64_t first) {
        std::vector<Turn> turns{Labelled(first, 100), Labelled(first + 100, 100)};
        turns.push_back(Labelled(first + 200, 100));
        turns.back().text.clear();  // empty texts are skipped, not joined
        return turns;
    });

    const std::vector<float> clip(300);
    EXPECT_EQ(transcriber.DecodeClip(clip, 7000), "w7000 w7100");
}

TEST(WhisperWorker, DecodeClipNeverReachesTheSinkOrTheDedupBackstop) {
    RecordingSink sink;
    WhisperTranscriber transcriber([](std::span<const float> f, std::uint64_t first) {
        return std::vector<Turn>{Labelled(first, f.size())};
    });
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    transcriber.Submit(window, 0);
    (void)transcriber.DecodeClip(window, 500);
    transcriber.Submit(window, 10);
    transcriber.Finish();

    ASSERT_EQ(sink.turns.size(), 2u) << "clips emit no turns";
    EXPECT_EQ(sink.turns[0].first_frame, 0u);
    EXPECT_EQ(sink.turns[1].first_frame, 10u);
}

TEST(WhisperWorker, PendingLiveWindowsDecodeBeforeAClip) {
    std::promise<void> release;
    auto released = release.get_future().share();
    std::mutex order_mutex;
    std::vector<std::uint64_t> order;
    WhisperTranscriber transcriber(
        [released, &order_mutex, &order](std::span<const float> f, std::uint64_t first) {
            released.wait();
            const std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(first);
            return std::vector<Turn>{Labelled(first, f.size())};
        });
    RecordingSink sink;
    transcriber.Begin(sink);

    const std::vector<float> window(10);
    transcriber.Submit(window, 0);  // decoding blocks on `release`
    transcriber.Submit(window, 10);
    auto clip = std::async(std::launch::async, [&] { return transcriber.DecodeClip(window, 99); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the clip enqueue
    release.set_value();
    EXPECT_EQ(clip.get(), "w99");

    const std::lock_guard<std::mutex> lock(order_mutex);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[2], 99u) << "the live transcript is never delayed by a clip";
}

TEST(WhisperWorker, AFailedLoadResolvesClipsEmpty) {
    WhisperTranscriber transcriber(
        DecodeLoader([]() -> DecodeFn { throw std::runtime_error("no GPU"); }));

    const std::vector<float> clip(10);
    EXPECT_EQ(transcriber.DecodeClip(clip, 0), "") << "an aborted re-split keeps the original";
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
