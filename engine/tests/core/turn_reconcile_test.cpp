#include "core/turn_reconcile.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::diar {
namespace {

asr::Turn Spoken(std::uint64_t first, std::uint64_t count, std::string text) {
    asr::Turn turn;
    turn.first_frame = first;
    turn.frame_count = count;
    turn.text = std::move(text);
    return turn;
}

TEST(ReconcileTurns, ARepeatedBoundaryPhraseIsDroppedFromTheEarlierTurn) {
    // The measured case: whisper heard the window boundary twice
    std::vector<asr::Turn> turns{
        Spoken(0, 80000, "Left elbow, okay. And have you banged that elbow?"),
        Spoken(76000, 60000, "and have you banged that elbow did you have any kind of injury"),
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].text, "Left elbow, okay.");
    EXPECT_EQ(turns[1].text, "and have you banged that elbow did you have any kind of injury")
        << "the later window heard it with full context and keeps it";
}

TEST(ReconcileTurns, DifferentRenderingsStillMatch) {
    // One word rendered differently across the boundary still matches;
    // a short fuzzy tail would not (the 0.66 floor guards false dedups)
    std::vector<asr::Turn> turns{
        Spoken(0, 80000, "I can refer you to the physio department"),
        Spoken(78000, 60000, "refer you to a physio department yes that would be great"),
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].text, "I can") << "edit distance matches across renderings";
}

TEST(ReconcileTurns, OverlappingSpansAreClampedApart) {
    std::vector<asr::Turn> turns{
        Spoken(0, 100000, "are you in a private place where you're okay to speak freely"),
        Spoken(90000, 50000, "okay to speak freely yes certainly"),
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].first_frame + turns[0].frame_count, turns[1].first_frame)
        << "no frame is owned by two turns after reconcile";
    EXPECT_EQ(turns[0].text, "are you in a private place where you're");
}

TEST(ReconcileTurns, DistantRepetitionIsRealSpeech) {
    std::vector<asr::Turn> turns{
        Spoken(0, 32000, "yes that's right"),
        Spoken(160000, 32000, "yes that's right"),  // 8 s later: an answer, not an echo
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].text, "yes that's right");
    EXPECT_EQ(turns[1].text, "yes that's right");
}

TEST(ReconcileTurns, AWeakMatchIsLeftAlone) {
    std::vector<asr::Turn> turns{
        Spoken(0, 80000, "how long has the swelling been there"),
        Spoken(78000, 60000, "about three weeks now I think"),
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].text, "how long has the swelling been there");
}

TEST(ReconcileTurns, SingleWordEchoesAreBelowTheFloor) {
    // One-word repeats are genuine conversation ("okay" / "okay")
    std::vector<asr::Turn> turns{
        Spoken(0, 32000, "okay"),
        Spoken(33000, 32000, "okay so tell me what happened"),
    };
    ReconcileTurns(turns);
    EXPECT_EQ(turns[0].text, "okay");
}

}  // namespace
}  // namespace ambient::diar
