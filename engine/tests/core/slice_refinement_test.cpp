#include "core/slice_refinement.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace sotto::diar {
namespace {

TEST(RefineRegions, AChangePointInsideARegionSplitsIt) {
    const std::vector<Region> regions{{10000, 50000}};
    const auto slices = RefineRegions(regions, {30000});
    ASSERT_EQ(slices.size(), 2u);
    EXPECT_EQ(slices[0].first_frame, 10000u);
    EXPECT_EQ(slices[0].end_frame, 30000u);
    EXPECT_EQ(slices[1].first_frame, 30000u);
    EXPECT_EQ(slices[1].end_frame, 50000u);
}

TEST(RefineRegions, ACutWithinTheEdgeMarginIsTheEdge) {
    const std::vector<Region> regions{{10000, 50000}};
    EXPECT_EQ(RefineRegions(regions, {10500})[0].first_frame, 10000u);
    EXPECT_EQ(RefineRegions(regions, {49500})[0].end_frame, 50000u);
    // Just past the margin the cut registers; its sub-minimum left fragment drops
    EXPECT_EQ(RefineRegions(regions, {10801})[0].first_frame, 10801u);
}

TEST(RefineRegions, ASubMinimumFragmentDropsAndTheRestSurvives) {
    const std::vector<Region> regions{{10000, 50000}};
    // The cut leaves a 2000-frame fragment on the left, under the 3200 floor
    const auto slices = RefineRegions(regions, {12000});
    ASSERT_EQ(slices.size(), 1u);
    EXPECT_EQ(slices[0].first_frame, 12000u);
    EXPECT_EQ(slices[0].end_frame, 50000u);
}

TEST(RefineRegions, CutsOutsideARegionAreIgnored) {
    const std::vector<Region> regions{{10000, 50000}, {60000, 90000}};
    const auto slices = RefineRegions(regions, {55000, 70000});
    ASSERT_EQ(slices.size(), 3u) << "only the second region splits";
}

TEST(EmbeddingRanges, OverlapIsExcludedWhenEnoughCleanRemains) {
    const Region slice{0, 20000};
    const auto ranges = EmbeddingRanges(slice, {{8000, 10000}});
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].end_frame, 8000u);
    EXPECT_EQ(ranges[1].first_frame, 10000u);
}

TEST(EmbeddingRanges, AMostlyOverlappedSliceEmbedsWhole) {
    const Region slice{0, 10000};
    // 7000 of 10000 frames overlapped: 3000 clean, under the 8000 minimum
    const auto ranges = EmbeddingRanges(slice, {{3000, 10000}});
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].first_frame, 0u);
    EXPECT_EQ(ranges[0].end_frame, 10000u);
}

TEST(EmbeddingRanges, ASliceUnderTheFloorDoesNotEmbed) {
    EXPECT_TRUE(EmbeddingRanges({0, 3000}, {{0, 2000}}).empty());
}

TEST(EmbeddingRanges, NoOverlapMeansTheWholeSliceOneRange) {
    const auto ranges = EmbeddingRanges({5000, 25000}, {});
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].first_frame, 5000u);
    EXPECT_EQ(ranges[0].end_frame, 25000u);
}

}  // namespace
}  // namespace sotto::diar
