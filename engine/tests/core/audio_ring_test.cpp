#include "core/audio_ring.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <thread>
#include <vector>

#include "ports/audio_source.hpp"

namespace sotto::audio {
namespace {

std::vector<float> Sequence(std::size_t count, float first = 0.0F) {
    std::vector<float> frames(count);
    for (std::size_t i = 0; i < count; ++i) {
        frames[i] = first + static_cast<float>(i);
    }
    return frames;
}

TEST(AudioRing, RoundsCapacityUpToAPowerOfTwo) {
    EXPECT_EQ(AudioRing(1000).Capacity(), 1024u);
    EXPECT_EQ(AudioRing(8).Capacity(), 8u);
}

TEST(AudioRing, PushThenPopReturnsTheSameFrames) {
    AudioRing ring(8);
    const auto frames = Sequence(5);

    EXPECT_EQ(ring.TryPush(frames), 5u);
    std::vector<float> out(5);
    EXPECT_EQ(ring.TryPop(out), 5u);

    EXPECT_EQ(out, frames);
}

TEST(AudioRing, PopFromAnEmptyRingReturnsNothing) {
    AudioRing ring(8);
    std::vector<float> out(4);

    EXPECT_EQ(ring.TryPop(out), 0u);
}

TEST(AudioRing, PushBeyondCapacityWritesOnlyWhatFits) {
    AudioRing ring(8);
    const auto frames = Sequence(10);

    EXPECT_EQ(ring.TryPush(frames), 8u);

    std::vector<float> out(10);
    EXPECT_EQ(ring.TryPop(out), 8u);
    EXPECT_EQ(std::vector<float>(out.begin(), out.begin() + 8), Sequence(8));
}

TEST(AudioRing, FreesSpaceAsFramesArePopped) {
    AudioRing ring(8);
    std::vector<float> out(8);
    EXPECT_EQ(ring.TryPush(Sequence(8)), 8u);
    EXPECT_EQ(ring.TryPop(out), 8u);

    EXPECT_EQ(ring.TryPush(Sequence(8, 100.0F)), 8u);
    EXPECT_EQ(ring.TryPop(out), 8u);
    EXPECT_EQ(out, Sequence(8, 100.0F));
}

TEST(AudioRing, WrapsAroundTheBufferEnd) {
    AudioRing ring(8);
    std::vector<float> out(6);
    ring.TryPush(Sequence(6));
    ring.TryPop(out);

    // This push crosses the physical end of the buffer
    ring.TryPush(Sequence(6, 50.0F));
    ring.TryPop(out);

    EXPECT_EQ(out, Sequence(6, 50.0F));
}

constexpr std::size_t kTotalFrames = 1 << 20;  // Exact in a float: < 2^24
constexpr std::size_t kChunk = 480;

// gtest assertions are not thread-safe on Windows, so the threads only count
// and every assertion happens after the join.
TEST(AudioRing, TwoThreadsMoveEveryFrameInOrder) {
    AudioRing ring(1024);

    std::thread producer([&ring] {
        std::vector<float> chunk(kChunk);
        std::size_t sent = 0;
        while (sent < kTotalFrames) {
            const std::size_t count = std::min(kChunk, kTotalFrames - sent);
            for (std::size_t i = 0; i < count; ++i) {
                chunk[i] = static_cast<float>(sent + i);
            }
            std::span<const float> remaining(chunk.data(), count);
            while (!remaining.empty()) {
                remaining = remaining.subspan(ring.TryPush(remaining));
            }
            sent += count;
        }
    });

    std::size_t received = 0;
    std::size_t out_of_sequence = 0;
    std::vector<float> out(512);
    while (received < kTotalFrames) {
        const std::size_t count = ring.TryPop(out);
        for (std::size_t i = 0; i < count; ++i) {
            if (out[i] != static_cast<float>(received + i)) {
                ++out_of_sequence;
            }
        }
        received += count;
    }
    producer.join();

    EXPECT_EQ(received, kTotalFrames);
    EXPECT_EQ(out_of_sequence, 0u);
    EXPECT_EQ(ring.TryPop(out), 0u);
}

// Pins the pipeline's one format and shows the port is implementable in a
// handful of lines; the wav source's tests are the real contract tests.
TEST(AudioSourcePort, ASinkReceivesAudioThenTheEnd) {
    struct RecordingSink : IAudioSink {
        std::vector<float> frames;
        std::uint64_t lost = 0;
        SourceEnd end{};
        void OnAudio(std::span<const float> packet, std::uint64_t lost_frames) override {
            frames.insert(frames.end(), packet.begin(), packet.end());
            lost += lost_frames;
        }
        void OnEnd(const SourceEnd& source_end) override {
            end = source_end;
        }
    };

    RecordingSink sink;
    const auto frames = Sequence(4);
    static_cast<IAudioSink&>(sink).OnAudio(frames, 3);
    static_cast<IAudioSink&>(sink).OnEnd({SourceEndReason::kCompleted, ""});

    EXPECT_EQ(kSampleRate, 16000);
    EXPECT_EQ(sink.frames, frames);
    EXPECT_EQ(sink.lost, 3u);
    EXPECT_EQ(sink.end.reason, SourceEndReason::kCompleted);
}

}  // namespace
}  // namespace sotto::audio
