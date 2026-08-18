#include "core/diar_regions.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace sotto::diar {
namespace {

constexpr std::uint64_t kHop = audio::kVadHopFrames;

void Add(std::vector<float>& probs, std::size_t hops, float p) {
    probs.insert(probs.end(), hops, p);
}

TEST(DiarRegions, ASpokenStretchBecomesOnePaddedRegion) {
    std::vector<float> probs;
    Add(probs, 10, 0.05f);
    Add(probs, 20, 0.90f);
    Add(probs, 10, 0.05f);
    const auto regions = SpeechRegions(probs, 40 * kHop);

    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].first_frame, 10 * kHop - kPadFrames);
    EXPECT_EQ(regions[0].end_frame, 30 * kHop + kPadFrames);
}

TEST(DiarRegions, ABlipShorterThanMinSpeechIsDropped) {
    std::vector<float> probs;
    Add(probs, 10, 0.05f);
    Add(probs, 3, 0.90f);  // 1536 frames, under the 1600 minimum
    Add(probs, 20, 0.05f);
    EXPECT_TRUE(SpeechRegions(probs, 33 * kHop).empty());
}

TEST(DiarRegions, AGapShorterThanMinSilenceDoesNotSplit) {
    std::vector<float> probs;
    Add(probs, 20, 0.90f);
    Add(probs, 4, 0.05f);  // 2048 frames, under the 2400 minimum
    Add(probs, 20, 0.90f);
    Add(probs, 10, 0.05f);
    const auto regions = SpeechRegions(probs, 54 * kHop);

    ASSERT_EQ(regions.size(), 1u) << "a sub-150 ms gap stays inside the region";
    EXPECT_EQ(regions[0].end_frame, 44 * kHop + kPadFrames);
}

TEST(DiarRegions, AGapPastMinSilenceSplitsAndBothEdgesPad) {
    std::vector<float> probs;
    Add(probs, 20, 0.90f);
    Add(probs, 10, 0.05f);  // 5120 frames of silence
    Add(probs, 20, 0.90f);
    Add(probs, 10, 0.05f);
    const auto regions = SpeechRegions(probs, 60 * kHop);

    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].first_frame, 0u);
    EXPECT_EQ(regions[0].end_frame, 20 * kHop + kPadFrames);
    EXPECT_EQ(regions[1].first_frame, 30 * kHop - kPadFrames);
    EXPECT_EQ(regions[1].end_frame, 50 * kHop + kPadFrames);
}

TEST(DiarRegions, OpenSpeechAtTheEndClosesAtTheTotal) {
    std::vector<float> probs;
    Add(probs, 5, 0.05f);
    Add(probs, 20, 0.90f);
    const auto regions = SpeechRegions(probs, 25 * kHop);

    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].end_frame, 25 * kHop);
}

}  // namespace
}  // namespace sotto::diar
