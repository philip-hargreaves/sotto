#include "core/per_turn.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace ambient::diar {
namespace {

const std::vector<float> kAudio(400000, 0.1f);

DecodeClipFn Decoder(std::vector<std::pair<std::uint64_t, std::uint64_t>>* calls = nullptr,
                     std::string reply = "spoken") {
    return [calls, reply](std::span<const float> clip, std::uint64_t first) {
        if (calls != nullptr) calls->push_back({first, clip.size()});
        return reply + " at " + std::to_string(first);
    };
}

TEST(MergeByCluster, ConsecutiveSameClusterSlicesBecomeOneTurn) {
    const std::vector<LabelledSlice> slices{
        {0, 10000, 0}, {12000, 30000, 0}, {31000, 50000, 1}, {52000, 60000, 0}};
    const auto turns = MergeByCluster(slices);
    ASSERT_EQ(turns.size(), 3u);
    EXPECT_EQ(turns[0].first_frame, 0u);
    EXPECT_EQ(turns[0].end_frame, 30000u);
    EXPECT_EQ(turns[1].cluster, 1);
    EXPECT_EQ(turns[2].first_frame, 52000u);
}

TEST(DecodeTurnTexts, EachTurnDecodesItsOwnAudioExactly) {
    const std::vector<LabelledSlice> turns{{0, 30000, 0}, {30000, 60000, 1}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls));
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[0], "spoken at 0");
    EXPECT_EQ(texts[1], "spoken at 30000");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0], (std::pair<std::uint64_t, std::uint64_t>{0, 30000}));
    EXPECT_EQ(calls[1], (std::pair<std::uint64_t, std::uint64_t>{30000, 30000}));
}

TEST(DecodeTurnTexts, AnOverlappingHeadIsClampedNotDecodedTwice) {
    // The second turn starts inside the first; only its unheard tail decodes
    const std::vector<LabelledSlice> turns{{0, 40000, 0}, {30000, 60000, 1}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    (void)DecodeTurnTexts(turns, kAudio, Decoder(&calls));
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1], (std::pair<std::uint64_t, std::uint64_t>{40000, 20000}));
}

TEST(DecodeTurnTexts, ANestedOverlapTurnGetsNoText) {
    // The audio belongs to whoever talked through it; decoding it would put
    // the louder speaker's words under the quieter speaker's name
    const std::vector<LabelledSlice> turns{{0, 60000, 0}, {20000, 40000, 1}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls));
    EXPECT_TRUE(texts[1].empty());
    EXPECT_EQ(calls.size(), 1u);
}

TEST(DecodeTurnTexts, SubMinimumSpansAreSkippedNotDecoded) {
    const std::vector<LabelledSlice> turns{{0, 4000, 0}};  // 0.25 s
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls));
    EXPECT_TRUE(texts[0].empty());
    EXPECT_TRUE(calls.empty());
}

TEST(DecodeTurnTexts, ADegenerateDecodeYieldsEmpty) {
    const std::vector<LabelledSlice> turns{{0, 30000, 0}};
    const auto texts = DecodeTurnTexts(turns, kAudio, [](std::span<const float>, std::uint64_t) {
        return std::string(
            "the same five words again the same five words again "
            "the same five words again the same five words again");
    });
    EXPECT_TRUE(texts[0].empty()) << "a repetition loop has no safe fallback";
}

TEST(DecodeTurnTexts, ACachedSpanIsUsedWithoutDecoding) {
    const std::vector<LabelledSlice> turns{{0, 30000, 0}, {30000, 60000, 1}};
    TurnTexts cache;
    cache[{0, 30000}] = "speculated words";
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls), &cache);
    EXPECT_EQ(texts[0], "speculated words");
    EXPECT_EQ(texts[1], "spoken at 30000");
    ASSERT_EQ(calls.size(), 1u) << "only the miss decodes";
}

TEST(DecodeTurnTexts, ACacheKeyMustMatchTheSpanExactly) {
    // A stale speculation whose boundaries did not survive clustering is
    // never found; the turn decodes fresh
    const std::vector<LabelledSlice> turns{{0, 30000, 0}};
    TurnTexts cache;
    cache[{0, 29999}] = "stale speculation";
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls), &cache);
    EXPECT_EQ(texts[0], "spoken at 0");
    EXPECT_EQ(calls.size(), 1u);
}

TEST(DecodeTurnTexts, ATurnPastTheAudioEndIsBounded) {
    const std::vector<LabelledSlice> turns{{390000, 500000, 0}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    (void)DecodeTurnTexts(turns, kAudio, Decoder(&calls));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].second, 10000u) << "clamped to the audio that exists";
}

}  // namespace
}  // namespace ambient::diar
