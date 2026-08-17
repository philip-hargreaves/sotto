#include "core/endpointer.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace sotto::audio {
namespace {

// Emits an authored probability per hop; holds the last value past the end
struct ScriptedVad : IStreamingVad {
    std::vector<float> probabilities;
    std::size_t next = 0;

    float SpeechProbability(std::span<const float>) override {
        const float p = next < probabilities.size() ? probabilities[next] : probabilities.back();
        ++next;
        return p;
    }

    void Reset() override {
        next = 0;
    }
};

constexpr std::size_t kHop = kVadHopFrames;

std::size_t Hops(double seconds) {
    return static_cast<std::size_t>(seconds * kSampleRate / kHop);
}

// Audio is a per-frame ramp so window contents identify their absolute range
std::vector<float> Ramp(std::size_t frames, std::uint64_t first = 0) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        audio[i] = static_cast<float>((first + i) % 100000);
    }
    return audio;
}

void Add(std::vector<float>& probs, std::size_t hops, float p) {
    probs.insert(probs.end(), hops, p);
}

TEST(Endpointer, AWindowStartsAtSpeechOnsetMinusThePad) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(1.0), 0.05f);  // idle second
    Add(vad.probabilities, Hops(3.0), 0.90f);  // speech
    Endpointer endpointer(vad);

    const auto total = (Hops(1.0) + Hops(3.0)) * kHop;
    auto windows = endpointer.Push(Ramp(total));
    EXPECT_TRUE(windows.empty());

    const auto tail = endpointer.Flush();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->first_frame, Hops(1.0) * kHop - Endpointer::kEdgePadFrames);
    EXPECT_EQ(tail->frames.front(),
              static_cast<float>(Hops(1.0) * kHop - Endpointer::kEdgePadFrames));
}

TEST(Endpointer, APauseDoesNotCloseBeforeTwentySecondsAccumulate) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(5.0), 0.90f);
    Add(vad.probabilities, Hops(1.0), 0.05f);  // 1 s pause, above 0.7 s
    Add(vad.probabilities, Hops(2.0), 0.90f);
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(8.0) * kHop));
    EXPECT_TRUE(windows.empty()) << "a pause before 20 s accumulated must not close";

    const auto tail = endpointer.Flush();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->first_frame, 0u) << "speech from the first hop has no pad to retain";
}

TEST(Endpointer, APauseClosesOnceTwentySecondsHaveAccumulated) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(21.0), 0.90f);
    Add(vad.probabilities, Hops(0.9), 0.05f);
    Add(vad.probabilities, Hops(4.0), 0.05f);  // stays silent
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(25.0) * kHop));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_EQ(windows[0].first_frame, 0u);
    // Cut lands at last speech plus the edge pad
    EXPECT_NEAR(static_cast<double>(windows[0].frames.size()),
                static_cast<double>(Hops(21.0) * kHop + Endpointer::kEdgePadFrames), kHop);
}

TEST(Endpointer, ALongBreakClosesRegardlessOfAccumulation) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(3.0), 0.90f);
    Add(vad.probabilities, Hops(2.5), 0.05f);
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(5.5) * kHop));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_NEAR(static_cast<double>(windows[0].frames.size()),
                static_cast<double>(Hops(3.0) * kHop + Endpointer::kEdgePadFrames), kHop);
}

TEST(Endpointer, ABlipShorterThanTheMinimumUtteranceIsDropped) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(0.25), 0.90f);  // a cough
    Add(vad.probabilities, Hops(2.5), 0.05f);
    Add(vad.probabilities, Hops(3.0), 0.90f);  // real speech
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(5.75) * kHop));
    EXPECT_TRUE(windows.empty()) << "the blip must not become a window";

    const auto tail = endpointer.Flush();
    ASSERT_TRUE(tail.has_value());
    EXPECT_GE(tail->first_frame, Hops(2.5) * kHop) << "the tail is the real speech, not the blip";
}

TEST(Endpointer, HysteresisHoldsTheStateBetweenThresholds) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(3.0), 0.90f);
    Add(vad.probabilities, Hops(3.0), 0.32f);  // between exit and enter: still speech
    Add(vad.probabilities, Hops(2.5), 0.05f);
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(8.5) * kHop));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_NEAR(static_cast<double>(windows[0].frames.size()),
                static_cast<double>(Hops(6.0) * kHop + Endpointer::kEdgePadFrames), kHop)
        << "the in-between hops count as speech";
}

TEST(Endpointer, TheCapBacktracksToTheQuietestHop) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(26.0), 0.90f);
    Add(vad.probabilities, 1, 0.28f);  // the quietest hop, still inside hysteresis
    Add(vad.probabilities, Hops(4.0), 0.90f);
    Endpointer endpointer(vad);

    const auto windows = endpointer.Push(Ramp(Hops(30.0) * kHop));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_EQ(windows[0].frames.size(), Hops(26.0) * kHop) << "cut at the quiet hop's start";

    const auto tail = endpointer.Flush();
    ASSERT_TRUE(tail.has_value());
    EXPECT_EQ(tail->first_frame, Hops(26.0) * kHop) << "the remainder continues seamlessly";
    EXPECT_EQ(tail->frames.front(), static_cast<float>(Hops(26.0) * kHop % 100000));
}

TEST(Endpointer, WindowsAndTailCoverTheSpeechExactly) {
    ScriptedVad vad;
    Add(vad.probabilities, Hops(3.0), 0.90f);
    Add(vad.probabilities, Hops(2.5), 0.05f);  // break
    Add(vad.probabilities, Hops(2.0), 0.90f);
    Endpointer endpointer(vad);

    const auto total = Hops(7.5) * kHop;
    auto windows = endpointer.Push(Ramp(total));
    const auto tail = endpointer.Flush();
    ASSERT_EQ(windows.size(), 1u);
    ASSERT_TRUE(tail.has_value());

    // Every window's content matches its claimed absolute position
    for (const auto& window : {windows[0], *tail}) {
        for (std::size_t i = 0; i < window.frames.size(); i += 997) {
            EXPECT_EQ(window.frames[i], static_cast<float>((window.first_frame + i) % 100000));
        }
    }
}

TEST(Endpointer, AlwaysSpeechDegeneratesToCappedCuts) {
    ScriptedVad vad;
    Add(vad.probabilities, 1, 0.90f);
    Endpointer endpointer(vad);  // holds 0.90 forever

    auto windows = endpointer.Push(Ramp(Hops(60.0) * kHop));
    const auto tail = endpointer.Flush();
    ASSERT_TRUE(tail.has_value());

    ASSERT_EQ(windows.size(), 2u);
    std::size_t covered = 0;
    for (const auto& window : windows) {
        EXPECT_EQ(window.first_frame, covered);
        EXPECT_LE(window.frames.size(), Endpointer::kSoftCapFrames);
        covered += window.frames.size();
    }
    EXPECT_EQ(tail->first_frame, covered);
    EXPECT_EQ(covered + tail->frames.size(), Hops(60.0) * kHop);
}

TEST(Endpointer, FlushWithoutSpeechIsEmpty) {
    ScriptedVad vad;
    Add(vad.probabilities, 1, 0.05f);
    Endpointer endpointer(vad);

    endpointer.Push(Ramp(Hops(3.0) * kHop));
    EXPECT_FALSE(endpointer.Flush().has_value());
}

}  // namespace
}  // namespace sotto::audio
