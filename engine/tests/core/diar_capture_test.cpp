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
              160000u - kSegFrontierMarginFrames);  // vad behind
    EXPECT_EQ(ambient::diar::SegSettledFrontier(100000, 320000), 100000u);  // seg behind
}

TEST(SegSettledFrontier, NothingSettlesInsideTheFirstMargin) {
    EXPECT_EQ(ambient::diar::SegSettledFrontier(320000, ambient::diar::kSegFrontierMarginFrames),
              0u);
    EXPECT_EQ(ambient::diar::SegSettledFrontier(320000, 0), 0u);
}

}  // namespace
