#include "core/turn_assembly.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::asr {
namespace {

Turn Make(std::uint64_t first_frame, std::uint64_t count, std::string text) {
    Turn turn;
    turn.first_frame = first_frame;
    turn.frame_count = count;
    turn.text = std::move(text);
    return turn;
}

TEST(AnchorFirstTurn, ADriftingFirstStampSnapsToTheWindowStart) {
    std::vector<Turn> turns{Make(30000, 1000, "hello"), Make(40000, 1000, "there")};
    AnchorFirstTurn(turns, 10000);
    EXPECT_EQ(turns[0].first_frame, 10000u);
    EXPECT_EQ(turns[0].frame_count, 21000u) << "the snap stretches the turn back to onset";
    EXPECT_EQ(turns[1].first_frame, 40000u) << "only the first turn anchors";
}

TEST(AnchorFirstTurn, DriftBeyondTheLimitIsLeftAlone) {
    std::vector<Turn> turns{Make(10000 + kAnchorSnapFrames + 1, 1000, "late")};
    AnchorFirstTurn(turns, 10000);
    EXPECT_EQ(turns[0].first_frame, 10000 + kAnchorSnapFrames + 1);
}

TEST(AnchorFirstTurn, AnOnTimeStampAndAnEmptyWindowAreNoOps) {
    std::vector<Turn> turns{Make(10000, 1000, "on time")};
    AnchorFirstTurn(turns, 10000);
    EXPECT_EQ(turns[0].frame_count, 1000u);
    std::vector<Turn> none;
    AnchorFirstTurn(none, 10000);
}

TEST(DropReheardTurns, TheMidpointDecides) {
    std::vector<Turn> turns{Make(1000, 500, "reheard"), Make(1800, 600, "straddler"),
                            Make(2500, 400, "new")};
    DropReheardTurns(turns, 2000);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].text, "straddler");
    EXPECT_EQ(turns[1].text, "new");
}

TEST(StripBoundaryDuplicates, AContiguousRepeatIsStrippedCaseAndPunctuationBlind) {
    const Turn prev = Make(0, 448000, "I'll probably ask for it.");
    Turn next = Make(448000, 16000, "Ask for it, we can book one.");
    StripBoundaryDuplicates(prev, next);
    EXPECT_EQ(next.text, "we can book one.");
}

TEST(StripBoundaryDuplicates, RepetitionAcrossRealSilenceIsKept) {
    const Turn prev = Make(0, 16000, "yes");
    Turn next = Make(16000 + kAdjacentGapFrames + 1, 16000, "yes");
    StripBoundaryDuplicates(prev, next);
    EXPECT_EQ(next.text, "yes") << "a genuine repeat after a gap is real speech";
}

TEST(StripBoundaryDuplicates, TheRunIsCappedAtFourWords) {
    const Turn prev = Make(0, 16000, "one two three four five");
    Turn next = Make(16000, 16000, "two three four five six");
    StripBoundaryDuplicates(prev, next);
    EXPECT_EQ(next.text, "six");
}

TEST(StripBoundaryDuplicates, AWhollyDuplicatedTurnStripsToEmpty) {
    const Turn prev = Make(0, 16000, "so");
    Turn next = Make(16000, 8000, "So.");
    StripBoundaryDuplicates(prev, next);
    EXPECT_EQ(next.text, "") << "the caller drops an emptied turn";
}

}  // namespace
}  // namespace ambient::asr
