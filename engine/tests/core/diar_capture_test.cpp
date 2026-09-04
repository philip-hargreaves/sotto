#include "core/diar_capture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ambient::diar::SettledFrontier;

ambient::asr::Turn MakeTurn(std::uint64_t first, std::uint64_t count) {
    ambient::asr::Turn turn;
    turn.first_frame = first;
    turn.frame_count = count;
    return turn;
}

TEST(SettledFrontier, NothingSettlesBeforeTheFirstCompletedTurn) {
    EXPECT_EQ(SettledFrontier(160000, {}, 320000), 0u);
}

TEST(SettledFrontier, BoundBySegmentationOrTurnsWhicheverIsBehind) {
    const std::vector<ambient::asr::Turn> turns{MakeTurn(0, 100000), MakeTurn(120000, 100000)};
    EXPECT_EQ(SettledFrontier(160000, turns, 320000), 160000u);  // seg behind
    EXPECT_EQ(SettledFrontier(320000, turns, 320000), 220000u);  // turns behind
}

TEST(SettledFrontier, TurnsPastTheAudioDoNotCount) {
    const std::vector<ambient::asr::Turn> turns{MakeTurn(0, 100000)};
    EXPECT_EQ(SettledFrontier(320000, turns, 80000), 0u);
}

TEST(SegSettledFrontier, TrailsVadByTheRegionMargin) {
    using ambient::diar::kSegFrontierMarginFrames;
    EXPECT_EQ(ambient::diar::SegSettledFrontier(320000, 160000),
              160000u - kSegFrontierMarginFrames);                          // vad behind
    EXPECT_EQ(ambient::diar::SegSettledFrontier(100000, 320000), 100000u);  // seg behind
}

TEST(SegSettledFrontier, NothingSettlesInsideTheFirstMargin) {
    EXPECT_EQ(ambient::diar::SegSettledFrontier(320000, ambient::diar::kSegFrontierMarginFrames),
              0u);
    EXPECT_EQ(ambient::diar::SegSettledFrontier(320000, 0), 0u);
}

}  // namespace

namespace ambient::diar {
namespace {

// 32 ms hops: speech to hop 50, silence after
std::vector<float> SpeechThenSilence(std::size_t hops) {
    std::vector<float> p(hops, 0.05f);
    for (std::size_t i = 0; i <= 50 && i < hops; ++i) p[i] = 0.9f;
    return p;
}

TEST(TurnClosed, SilenceAfterTheEndClosesTheTurn) {
    const auto end = 51u * audio::kVadHopFrames;
    EXPECT_TRUE(TurnClosed(SpeechThenSilence(120), end));
}

TEST(TurnClosed, NotClosedWhileTheAudioIsShorterThanTheSilenceWindow) {
    const auto end = 51u * audio::kVadHopFrames;
    EXPECT_FALSE(TurnClosed(SpeechThenSilence(60), end));
}

TEST(TurnClosed, SpeechInsideTheWindowKeepsItOpen) {
    auto p = SpeechThenSilence(120);
    p[65] = 0.8f;
    const auto end = 51u * audio::kVadHopFrames;
    EXPECT_FALSE(TurnClosed(p, end));
}

}  // namespace
}  // namespace ambient::diar
