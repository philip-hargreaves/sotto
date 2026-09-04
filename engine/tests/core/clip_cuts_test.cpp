#include "core/clip_cuts.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ambient::diar {
namespace {

// 32 ms hops; speech everywhere except a pause at hops 40-42 (1.28-1.34 s)
std::vector<float> Probabilities() {
    std::vector<float> p(200, 0.9f);
    p[40] = 0.3f;
    p[41] = 0.05f;
    p[42] = 0.3f;
    return p;
}

TEST(SnapClipCuts, AnEdgeNearAPauseMovesOntoThePause) {
    const std::vector<std::uint64_t> cuts{41 * 512 + 3000};  // 190 ms after the pause
    const auto kept = SnapClipCuts(cuts, Probabilities(), {});
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0], 41u * 512u);
}

TEST(SnapClipCuts, AnEdgeWithNoPauseNearbyIsDropped) {
    const std::vector<std::uint64_t> cuts{120 * 512};
    EXPECT_TRUE(SnapClipCuts(cuts, Probabilities(), {}).empty());
}

TEST(SnapClipCuts, AnEdgeTooCloseToAnotherCutIsDropped) {
    const std::vector<std::uint64_t> cuts{41 * 512};
    const std::vector<std::uint64_t> seg{41 * 512 + 4000};  // 250 ms away
    EXPECT_TRUE(SnapClipCuts(cuts, Probabilities(), seg).empty());
}

TEST(SnapClipCuts, ExactModeKeepsAnEdgeWithoutAPauseButHonoursClearance) {
    const std::vector<std::uint64_t> cuts{120 * 512, 120 * 512 + 3000};
    const auto kept = SnapClipCuts(cuts, Probabilities(), {}, 0, false);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0], 120u * 512u);
}

TEST(SnapClipCuts, TwoEdgesOnOnePauseKeepOne) {
    const std::vector<std::uint64_t> cuts{41 * 512 - 2000, 41 * 512 + 2000};
    EXPECT_EQ(SnapClipCuts(cuts, Probabilities(), {}).size(), 1u);
}

}  // namespace
}  // namespace ambient::diar
