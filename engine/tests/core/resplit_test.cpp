#include "core/resplit.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::diar {
namespace {

asr::Turn C(std::uint64_t first, std::uint64_t end, const char* text) {
    asr::Turn t;
    t.first_frame = first;
    t.frame_count = end - first;
    t.text = text;
    return t;
}

// Two unit centroids on orthogonal axes; a chunk's voice is a unit vector too
const std::vector<std::vector<float>> kCentroids{{1.0f, 0.0f}, {0.0f, 1.0f}};

// Audio before 100000 is the patient's voice (cluster 1); after it the doctor's (0)
std::vector<float> Voice(std::uint64_t first, std::uint64_t /*end*/) {
    return first < 100000 ? std::vector<float>{0.1f, 0.995f} : std::vector<float>{0.995f, 0.1f};
}

TEST(Resplit, ABorrowedHeadChunkGoesBackToItsSpeaker) {
    // The doctor's merged turn starts 3 s early with the patient's sentence
    const std::vector<LabelledSlice> turns{{50000, 200000, 0}};
    const std::vector<std::string> texts{"but I just get that from the chemist. Okay, and how often?"};
    const std::vector<std::vector<asr::Turn>> chunks{
        {C(50000, 98000, "but I just get that from the chemist."), C(100000, 200000, "Okay, and how often?")}};
    const auto out = ResplitByEmbedding(turns, texts, chunks, Voice, kCentroids);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].slice.cluster, 1);
    EXPECT_EQ(out[0].text, "but I just get that from the chemist.");
    EXPECT_EQ(out[0].slice.first_frame, 50000u);
    EXPECT_EQ(out[1].slice.cluster, 0);
    EXPECT_EQ(out[1].text, "Okay, and how often?");
    EXPECT_EQ(out[1].slice.end_frame, 200000u) << "the turn keeps its own end";
}

TEST(Resplit, OnlyEdgeChunksMoveAndAShortOneStays) {
    const std::vector<LabelledSlice> turns{{50000, 300000, 0}};
    const std::vector<std::string> texts{"a b c d"};
    const std::vector<std::vector<asr::Turn>> chunks{{C(50000, 56000, "a"),  // 0.4 s: too short to move
                                                      C(60000, 98000, "b"), C(100000, 200000, "c"),
                                                      C(200000, 300000, "d")}};
    const auto out = ResplitByEmbedding(turns, texts, chunks, Voice, kCentroids);
    ASSERT_EQ(out.size(), 1u) << "the short head chunk blocks the head run; the tail is the doctor's";
    EXPECT_EQ(out[0].text, "a b c d");
}

TEST(Resplit, AChunkStampedPastTheTurnIsJudgedOnTheTurnsAudioOnly) {
    // The tail chunk's stamp runs 5 s past the turn end into the patient's audio;
    // judged on the turn's own audio it is the doctor's and stays
    const std::vector<LabelledSlice> turns{{100000, 140000, 0}};
    const std::vector<std::string> texts{"a b"};
    const std::vector<std::vector<asr::Turn>> chunks{{C(100000, 120000, "a"), C(120000, 220000, "b")}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> asked;
    const auto embed = [&](std::uint64_t first, std::uint64_t end) {
        asked.push_back({first, end});
        return Voice(first, end);
    };
    const auto out = ResplitByEmbedding(turns, texts, chunks, embed, kCentroids);
    ASSERT_EQ(out.size(), 1u);
    for (const auto& span : asked) EXPECT_LE(span.second, 140000u);
}

TEST(Resplit, NothingMovesWithoutAMarginOrWithOneCluster) {
    const std::vector<LabelledSlice> turns{{50000, 200000, 0}};
    const std::vector<std::string> texts{"x y"};
    const std::vector<std::vector<asr::Turn>> chunks{{C(50000, 98000, "x"), C(100000, 200000, "y")}};
    const auto same = [](std::uint64_t, std::uint64_t) { return std::vector<float>{0.7f, 0.714f}; };
    EXPECT_EQ(ResplitByEmbedding(turns, texts, chunks, same, kCentroids)[0].text, "x y");
    EXPECT_EQ(ResplitByEmbedding(turns, texts, chunks, Voice, {{1.0f, 0.0f}}).size(), 1u);
}

}  // namespace
}  // namespace ambient::diar
