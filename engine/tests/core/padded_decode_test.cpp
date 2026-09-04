#include "core/padded_decode.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::diar {
namespace {

// 10 s of audio; the clip is the patient's answer at 5.0-5.8 s
const std::vector<float> kAudio(160000, 0.0f);
constexpr std::uint64_t kA = 80000;
constexpr std::uint64_t kB = 92800;
constexpr std::uint64_t kPad = 16000;
const std::string kBefore = "Have you got any history of blood clots?";
const std::string kAfter = "Okay, anyone in your family?";

asr::Turn W(std::uint64_t first, std::uint64_t end, const char* text) {
    asr::Turn t;
    t.first_frame = first;
    t.frame_count = end - first;
    t.text = text;
    return t;
}

// The padded window as Whisper times it: the neighbours' boundary words
// smeared into the pauses, the clip's first word timed early
std::vector<asr::Turn> Padded() {
    return {W(kA - 16000, kA - 12000, "blood"), W(kA - 12000, kA - 6000, "clots?"),
            W(kA - 5000, kA + 1500, "Not"),     W(kA + 1500, kA + 3000, "that"),
            W(kA + 3000, kA + 4000, "I"),       W(kA + 4000, kA + 6000, "know"),
            W(kA + 6000, kA + 8000, "of,"),     W(kA + 8000, kB + 2000, "no."),
            W(kB + 500, kB + 6000, "OK,"),      W(kB + 6000, kB + 9000, "anyone")};
}

std::vector<asr::Turn> Scripted(std::span<const float> clip, std::uint64_t first) {
    const std::uint64_t end = first + clip.size();
    if (first == kA && end == kB) return {W(kA, kB, "Thanks, Tony.")};
    if (first == kA - kPad && end == kB + kPad) return Padded();
    if (first == kA - kPad && end == kB) {
        auto words = Padded();
        words.resize(8);
        return words;
    }
    return {W(first, end, "unexpected")};
}

TEST(TrimToClip, EdgeWordsThatRepeatTheNeighboursAreDroppedTheClipsOwnKept) {
    const auto kept = TrimToClip(Padded(), kA, kB, kBefore, kAfter);
    EXPECT_EQ(JoinedText(kept), "Not that I know of, no.");
}

TEST(TrimToClip, AnEdgeWordTheNeighbourDoesNotHoldStaysWithTheClip) {
    // The doctor's decode missed "healthy": it is not in `after`, so the
    // patient keeps it even though it is timed at the edge
    auto words = Padded();
    words.insert(words.begin() + 8, W(kB + 200, kB + 1400, "healthy"));
    const auto kept = TrimToClip(words, kA, kB, kBefore, kAfter);
    EXPECT_EQ(JoinedText(kept), "Not that I know of, no. healthy");
}

TEST(TrimToClip, OkAndOkayAreTheSameWord) {
    EXPECT_TRUE(detail::SameWord("ok", "okay"));
    EXPECT_TRUE(detail::SameWord("ots", "clots"));
    EXPECT_FALSE(detail::SameWord("no", "not"));  // two letters: only ok is trusted
    EXPECT_FALSE(detail::SameWord("you", "yes"));
}

TEST(PaddedDecode, OnlyTheSideWithANeighbourIsPadded) {
    EXPECT_EQ(JoinedText(PaddedDecode(kAudio, kA, kB, kPad, Scripted, kBefore, "")), "Not that I know of, no.");
}

TEST(PaddedDecode, NothingInsideFallsBackToThePlainClip) {
    const auto outside = [](std::span<const float>, std::uint64_t first) {
        if (first == kA) return std::vector<asr::Turn>{W(kA, kB, "plain words")};
        return std::vector<asr::Turn>{W(kA - kPad, kA - 12000, "all before the clip")};
    };
    EXPECT_EQ(JoinedText(PaddedDecode(kAudio, kA, kB, kPad, outside, kBefore, kAfter)), "plain words");
}

TEST(PaddedDecode, NoPadOrALongClipDecodesPlainly) {
    EXPECT_EQ(JoinedText(PaddedDecode(kAudio, kA, kB, 0, Scripted, kBefore, kAfter)), "Thanks, Tony.");
    int calls = 0;
    const auto count = [&](std::span<const float> clip, std::uint64_t first) {
        ++calls;
        EXPECT_EQ(first, 0u);
        EXPECT_EQ(clip.size(), 100000u);
        return std::vector<asr::Turn>{W(0, 100000, "long")};
    };
    EXPECT_EQ(JoinedText(PaddedDecode(kAudio, 0, 100000, kPad, count, kBefore, kAfter)), "long");
    EXPECT_EQ(calls, 1);
}

}  // namespace
}  // namespace ambient::diar
