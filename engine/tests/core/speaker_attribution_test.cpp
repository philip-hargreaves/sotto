#include "core/speaker_attribution.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sotto::diar {
namespace {

asr::Turn Spoken(std::uint64_t first, std::uint64_t count, std::string text) {
    asr::Turn turn;
    turn.first_frame = first;
    turn.frame_count = count;
    turn.text = std::move(text);
    return turn;
}

TEST(SpeakerAttribution, ATurnGoesWholeToItsMajorityOwner) {
    const std::vector<LabelledSlice> slices{{{0, 40000}, 0}, {{40000, 80000}, 1}};
    const auto turns = AttributeSpeakers({Spoken(10000, 20000, "hello there")}, slices);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "speaker 1");
    EXPECT_EQ(turns[0].text, "hello there");
}

TEST(SpeakerAttribution, ATurnOverlappingNothingGoesToTheNearestSlice) {
    const std::vector<LabelledSlice> slices{{{0, 16000}, 0}, {{100000, 120000}, 1}};
    const auto turns = AttributeSpeakers({Spoken(90000, 8000, "late words")}, slices);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "speaker 2") << "words are never dropped";
}

TEST(SpeakerAttribution, AShortSharedTurnGoesToTheMostSpecificClaimant) {
    // A 0.5 s backchannel slice inside a long monologue slice; the 1 s turn
    // overlaps the monologue more in absolute terms but the backchannel
    // covers 80% of its own length
    const std::vector<LabelledSlice> slices{{{0, 160000}, 0}, {{100000, 108000}, 1}};
    const auto turns = AttributeSpeakers({Spoken(98000, 16000, "Okay.")}, slices);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "speaker 2")
        << "the backchannel claims its word from inside the monologue";
}

TEST(SpeakerAttribution, AGenuinelySharedTurnSplitsAtThePunctuation) {
    const std::vector<LabelledSlice> slices{{{0, 40000}, 0}, {{40000, 80000}, 1}};
    // 2.5 s turn straddling the handover, sentence mark near the middle
    const auto turns =
        AttributeSpeakers({Spoken(20000, 40000, "That sounds fine. When did it start?")}, slices);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].text, "That sounds fine.");
    EXPECT_EQ(turns[1].text, "When did it start?");
}

TEST(SpeakerAttribution, AShortOrTextlessStraddlerDoesNotSplit) {
    const std::vector<LabelledSlice> slices{{{0, 40000}, 0}, {{40000, 80000}, 1}};
    // Shared but under the 1.2 s split floor: one owner takes it whole
    const auto turns = AttributeSpeakers({Spoken(35000, 10000, "yes exactly right")}, slices);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].text, "yes exactly right");
}

TEST(SpeakerAttribution, ConsecutiveSameSpeakerSlicesMerge) {
    const std::vector<LabelledSlice> slices{
        {{0, 20000}, 0}, {{20000, 40000}, 0}, {{40000, 60000}, 1}};
    const auto turns = AttributeSpeakers(
        {Spoken(0, 20000, "first"), Spoken(20000, 20000, "second"), Spoken(40000, 20000, "other")},
        slices);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].text, "first second");
    EXPECT_EQ(turns[0].frame_count, 40000u);
    EXPECT_EQ(turns[1].speaker, "speaker 2");
}

TEST(SpeakerAttribution, WordlessSlicesDisappear) {
    const std::vector<LabelledSlice> slices{{{0, 20000}, 0}, {{30000, 50000}, 1}};
    const auto turns = AttributeSpeakers({Spoken(1000, 10000, "only here")}, slices);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "speaker 1");
}

TEST(SplitPoint, PrefersSentenceThenCommaThenSpace) {
    EXPECT_EQ(detail::SplitPoint("one two. three four", 0.5), 9u) << "after the sentence mark";
    EXPECT_EQ(detail::SplitPoint("one two, three four", 0.5), 9u) << "comma when no sentence";
    EXPECT_EQ(detail::SplitPoint("one twoX three four", 0.5), 9u) << "word gap as last resort";
}

}  // namespace
}  // namespace sotto::diar
