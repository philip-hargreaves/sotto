#include "core/diar_capture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using sotto::diar::ChainCutPoints;
using sotto::diar::ChainSlices;
using sotto::diar::Region;
using sotto::diar::SettledFrontier;

sotto::asr::Turn MakeTurn(std::uint64_t first, std::uint64_t count) {
    sotto::asr::Turn turn;
    turn.first_frame = first;
    turn.frame_count = count;
    return turn;
}

TEST(SettledFrontier, NothingSettlesBeforeTheFirstCompletedTurn) {
    EXPECT_EQ(SettledFrontier(160000, {}, 320000), 0u);
}

TEST(SettledFrontier, BoundBySegmentationOrTurnsWhicheverIsBehind) {
    const std::vector<sotto::asr::Turn> turns{MakeTurn(0, 100000), MakeTurn(120000, 100000)};
    EXPECT_EQ(SettledFrontier(160000, turns, 320000), 160000u);  // seg behind
    EXPECT_EQ(SettledFrontier(320000, turns, 320000), 220000u);  // turns behind
}

TEST(SettledFrontier, TurnsPastTheAudioDoNotCount) {
    const std::vector<sotto::asr::Turn> turns{MakeTurn(0, 100000)};
    EXPECT_EQ(SettledFrontier(320000, turns, 80000), 0u);
}

// Unit embeddings: identical vectors are distance 0, orthogonal are 1
const std::vector<float> kVoiceA{1.0f, 0.0f};
const std::vector<float> kVoiceB{0.0f, 1.0f};

TEST(ChainSlices, SameVoiceMergesDifferentVoiceBreaks) {
    const std::vector<Region> slices{{0, 16000}, {16000, 32000}, {32000, 48000}};
    const std::vector<std::vector<float>> embeddings{kVoiceA, kVoiceA, kVoiceB};
    const auto chains = ChainSlices(slices, embeddings);
    ASSERT_EQ(chains.size(), 2u);
    EXPECT_EQ(chains[0].first_frame, 0u);
    EXPECT_EQ(chains[0].end_frame, 32000u);
    EXPECT_EQ(chains[1].first_frame, 32000u);
}

TEST(ChainSlices, AShortSliceCannotBreakAChain) {
    // The trailing slice sounds different but is under 0.5 s, so it merges
    const std::vector<Region> slices{{0, 16000}, {16000, 20000}};
    const std::vector<std::vector<float>> embeddings{kVoiceA, kVoiceB};
    const auto chains = ChainSlices(slices, embeddings);
    ASSERT_EQ(chains.size(), 1u);
    EXPECT_EQ(chains[0].end_frame, 20000u);
}

TEST(ChainSlices, UnembeddableSlicesJoinNoChain) {
    const std::vector<Region> slices{{0, 16000}, {16000, 32000}, {32000, 48000}};
    const std::vector<std::vector<float>> embeddings{kVoiceA, {}, kVoiceB};
    const auto chains = ChainSlices(slices, embeddings);
    ASSERT_EQ(chains.size(), 2u);
    EXPECT_EQ(chains[0].end_frame, 16000u);
    EXPECT_EQ(chains[1].first_frame, 32000u);
}

TEST(ChainCutPoints, CutsAtBoundaryMidpointsInsideTheTurn) {
    const std::vector<Region> chains{{0, 30000}, {34000, 64000}};
    const auto cuts = ChainCutPoints(10000, 60000, chains);
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_EQ(cuts[0], 32000u);
}

TEST(ChainCutPoints, ACutTooNearTheTurnEdgeIsDropped) {
    // Midpoint 32000 leaves under 0.3 s of the turn on one side
    const std::vector<Region> chains{{0, 30000}, {34000, 64000}};
    EXPECT_TRUE(ChainCutPoints(30000, 60000, chains).empty());
    EXPECT_TRUE(ChainCutPoints(10000, 35000, chains).empty());
}

}  // namespace
