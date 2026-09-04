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
        asr::Turn chunk;
        chunk.first_frame = first;
        chunk.frame_count = clip.size();
        chunk.text = reply + " at " + std::to_string(first);
        return std::vector<asr::Turn>{chunk};
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
        asr::Turn chunk;
        chunk.frame_count = 30000;
        chunk.text =
            "the same five words again the same five words again "
            "the same five words again the same five words again";
        return std::vector<asr::Turn>{chunk};
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

TEST(SpeculatedTurns, StopsAtTheFirstSpanTheCacheDoesNotHold) {
    const std::vector<LabelledSlice> merged{
        {0, 30000, 0}, {30000, 60000, 1}, {60000, 90000, 0}, {90000, 120000, 1}};
    TurnTexts cache;
    cache[{0, 30000}] = "first";
    cache[{30000, 60000}] = "second";
    cache[{90000, 120000}] = "fourth";  // known, but behind an unknown turn
    std::vector<std::string> texts;
    const auto known = SpeculatedTurns(merged, 120000, cache, &texts);
    ASSERT_EQ(known.size(), 2u);
    EXPECT_EQ(known[1].cluster, 1);
    EXPECT_EQ(texts, (std::vector<std::string>{"first", "second"}));
}

TEST(SpeculatedTurns, SkipsSpansBelowTheClipFloorAsFinaliseDoes) {
    const std::vector<LabelledSlice> merged{{0, 30000, 0}, {30000, 32000, 1}, {32000, 60000, 0}};
    TurnTexts cache;
    cache[{0, 30000}] = "first";
    cache[{32000, 60000}] = "third";
    std::vector<std::string> texts;
    const auto known = SpeculatedTurns(merged, 60000, cache, &texts);
    ASSERT_EQ(known.size(), 2u);
    EXPECT_EQ(known[1].first_frame, 32000u);
    EXPECT_EQ(texts[1], "third");
}

TEST(SpeculatedTurns, NothingKnownIsEmpty) {
    std::vector<std::string> texts;
    EXPECT_TRUE(SpeculatedTurns({{0, 30000, 0}}, 30000, {}, &texts).empty());
    EXPECT_TRUE(texts.empty());
}

}  // namespace
}  // namespace ambient::diar

namespace ambient::diar {
namespace {

TEST(AssembleFromChunks, ACutPieceOnChunkEdgesIsAssembledNotRedecoded) {
    struct Flag {
        Flag() {
            _putenv_s("AMBIENT_CHUNK_ASSEMBLE", "1");
        }
        ~Flag() {
            _putenv_s("AMBIENT_CHUNK_ASSEMBLE", "");
        }
    } flag;
    TurnChunks cache;
    asr::Turn c1, c2, c3;
    c1.first_frame = 0;
    c1.frame_count = 40000;
    c1.text = "have you had clots?";
    c2.first_frame = 41000;
    c2.frame_count = 12000;
    c2.text = "No.";
    c3.first_frame = 54000;
    c3.frame_count = 30000;
    c3.text = "Anyone in your family?";
    cache[{0, 84000}] = {c1, c2, c3};
    const auto piece = AssembleFromChunks(cache, 40500, 53500);  // the cut around "No."
    ASSERT_TRUE(piece.has_value());
    EXPECT_EQ(JoinedText(*piece), "No.");
    EXPECT_FALSE(AssembleFromChunks(cache, 20000, 53500).has_value()) << "not on a chunk edge";
}

}  // namespace
}  // namespace ambient::diar
