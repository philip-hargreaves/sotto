#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"

namespace sotto::diar {
namespace {

// 589 frames like the real model; helpers author class runs
std::vector<std::int8_t> Frames(std::initializer_list<std::pair<int, int>> runs) {
    std::vector<std::int8_t> classes;
    for (const auto& [cls, count] : runs) {
        classes.insert(classes.end(), static_cast<std::size_t>(count),
                       static_cast<std::int8_t>(cls));
    }
    return classes;
}

TEST(SegDecode, ASpeakerFlipAfterTheHoldIsAChangePoint) {
    SegResult result;
    DecodeSegWindow(Frames({{1, 100}, {2, 100}}), 0, result);
    ASSERT_EQ(result.change_points.size(), 1u);
    const double step = static_cast<double>(kSegWindowFrames) / 200.0;
    EXPECT_EQ(result.change_points[0], static_cast<std::uint64_t>(100 * step));
}

TEST(SegDecode, AFlickerShorterThanTheHoldIsNoChange) {
    SegResult result;
    // Speaker 1 for 4 frames only (under the 6-frame hold), then back
    DecodeSegWindow(Frames({{1, 100}, {2, 4}, {1, 100}}), 0, result);
    ASSERT_EQ(result.change_points.size(), 1u)
        << "the flip TO the flicker is a change only if the prior speaker held";
    SegResult clean;
    DecodeSegWindow(Frames({{1, 4}, {2, 100}}), 0, clean);
    EXPECT_TRUE(clean.change_points.empty()) << "a 4-frame holder cannot produce a change";
}

TEST(SegDecode, SilenceAndOverlapCarryNoChange) {
    SegResult result;
    // Speaker 1, silence, same speaker again: no change point
    DecodeSegWindow(Frames({{1, 100}, {0, 50}, {1, 100}}), 0, result);
    EXPECT_TRUE(result.change_points.empty());
    // Speaker 1, overlap, speaker 2: the change lands at speaker 2's onset
    SegResult handover;
    DecodeSegWindow(Frames({{1, 100}, {4, 20}, {2, 100}}), 0, handover);
    ASSERT_EQ(handover.change_points.size(), 1u);
}

TEST(SegDecode, OverlapFramesBecomeMergedSpans) {
    SegResult result;
    DecodeSegWindow(Frames({{1, 100}, {4, 10}, {5, 10}, {1, 80}}), 0, result);
    ASSERT_EQ(result.overlap_spans.size(), 1u) << "adjacent overlap classes merge into one span";
    const double step = static_cast<double>(kSegWindowFrames) / 200.0;
    EXPECT_EQ(result.overlap_spans[0].first_frame, static_cast<std::uint64_t>(100 * step));
    EXPECT_EQ(result.overlap_spans[0].end_frame, static_cast<std::uint64_t>(120 * step));
}

TEST(SegDecode, SpansMergeAcrossWindows) {
    SegResult result;
    DecodeSegWindow(Frames({{1, 195}, {4, 5}}), 0, result);
    DecodeSegWindow(Frames({{4, 5}, {1, 195}}), kSegWindowFrames, result);
    ASSERT_EQ(result.overlap_spans.size(), 1u);
    EXPECT_EQ(result.overlap_spans[0].end_frame,
              kSegWindowFrames +
                  static_cast<std::uint64_t>(5 * (static_cast<double>(kSegWindowFrames) / 200.0)));
}

}  // namespace
}  // namespace sotto::diar
