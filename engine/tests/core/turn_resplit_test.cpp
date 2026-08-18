#include "core/turn_resplit.hpp"

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

const std::vector<float> kAudio(400000, 0.1f);

DecodeClipFn Decoder(std::vector<std::pair<std::uint64_t, std::uint64_t>>* calls = nullptr,
                     std::string reply = "piece") {
    return [calls, reply](std::span<const float> clip, std::uint64_t first) {
        if (calls != nullptr) calls->push_back({first, clip.size()});
        return reply + " at " + std::to_string(first);
    };
}

TEST(ResplitStraddles, AStraddlerBecomesOnePiecePerSpeaker) {
    const std::vector<LabelledSlice> slices{{0, 50000, 0}, {50000, 100000, 1}};
    const std::vector<asr::Turn> turns{Spoken(20000, 60000, "both speakers in here")};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;

    const auto out = ResplitStraddles(turns, slices, kAudio, Decoder(&calls));

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first_frame, 20000u);
    EXPECT_EQ(out[0].first_frame + out[0].frame_count, 50000u) << "cut at the slice boundary";
    EXPECT_EQ(out[1].first_frame, 50000u);
    EXPECT_EQ(out[1].first_frame + out[1].frame_count, 80000u);
    EXPECT_EQ(calls.size(), 2u);
    EXPECT_EQ(out[0].text, "piece at 20000");
}

TEST(ResplitStraddles, ANonStraddlerPassesThroughUndecoded) {
    const std::vector<LabelledSlice> slices{{0, 50000, 0}, {50000, 100000, 1}};
    const std::vector<asr::Turn> turns{Spoken(5000, 30000, "one speaker")};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;

    const auto out = ResplitStraddles(turns, slices, kAudio, Decoder(&calls));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].text, "one speaker");
    EXPECT_TRUE(calls.empty());
}

TEST(ResplitStraddles, ASubMinimumPieceAbortsTheWholeTurn) {
    // The second piece would be 5000 frames, under the 0.4 s floor
    const std::vector<LabelledSlice> slices{{0, 55000, 0}, {55000, 100000, 1}};
    const std::vector<asr::Turn> turns{Spoken(20000, 40000, "kept whole")};

    const auto out = ResplitStraddles(turns, slices, kAudio, Decoder());

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].text, "kept whole") << "short clips hallucinate; the original survives";
}

TEST(ResplitStraddles, AnEmptyOrDegenerateDecodeAborts) {
    const std::vector<LabelledSlice> slices{{0, 50000, 0}, {50000, 100000, 1}};
    const std::vector<asr::Turn> turns{Spoken(20000, 60000, "original text")};

    const auto empty = ResplitStraddles(
        turns, slices, kAudio, [](std::span<const float>, std::uint64_t) { return std::string(); });
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0].text, "original text");

    const auto degenerate =
        ResplitStraddles(turns, slices, kAudio, [](std::span<const float>, std::uint64_t) {
            return std::string(
                "the same five words again the same five words again "
                "the same five words again the same five words again");
        });
    ASSERT_EQ(degenerate.size(), 1u);
    EXPECT_EQ(degenerate[0].text, "original text");
}

TEST(ResplitStraddles, SameClusterNeighboursAreOneSpeakersPause) {
    const std::vector<LabelledSlice> slices{{0, 40000, 0}, {45000, 100000, 0}};
    const std::vector<asr::Turn> turns{Spoken(20000, 60000, "monologue with a pause")};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;

    const auto out = ResplitStraddles(turns, slices, kAudio, Decoder(&calls));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(calls.empty());
}

TEST(ResplitStraddles, ANestedOverlapSliceIsNotAHandover) {
    // A backchannel slice inside the monologue is simultaneous speech
    const std::vector<LabelledSlice> slices{{0, 90000, 0}, {30000, 40000, 1}};
    const std::vector<asr::Turn> turns{Spoken(10000, 70000, "monologue with a backchannel")};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;

    const auto out = ResplitStraddles(turns, slices, kAudio, Decoder(&calls));

    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(calls.empty());
}

}  // namespace
}  // namespace sotto::diar
