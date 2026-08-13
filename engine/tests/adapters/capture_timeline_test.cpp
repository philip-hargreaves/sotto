#include "adapters/audio/capture_timeline.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace sotto::audio {
namespace {

// The dev machine's shape: 48 kHz native, 16 kHz target, 10 ms packets
constexpr std::uint32_t kNative = 48000;
constexpr std::uint32_t kFrames = 160;              // Target frames per packet
constexpr std::uint64_t kNativeStep = kFrames * 3;  // Native frames per packet

TEST(CaptureTimeline, CleanStreamReportsNoLoss) {
    CaptureTimeline timeline(kNative);

    for (int packet = 0; packet < 100; ++packet) {
        EXPECT_EQ(timeline.OnPacket(packet * kNativeStep, kFrames, false), 0u);
    }

    EXPECT_EQ(timeline.TotalLost(), 0u);
}

TEST(CaptureTimeline, FirstPacketAnchorsAndNeverReportsLoss) {
    CaptureTimeline timeline(kNative);

    // Arbitrary origin, and the flag is undefined on the first packet
    EXPECT_EQ(timeline.OnPacket(987654, kFrames, true), 0u);

    EXPECT_EQ(timeline.TotalLost(), 0u);
    EXPECT_EQ(timeline.DiscontinuityFlags(), 0u);
}

TEST(CaptureTimeline, AGapIsReportedInTargetFrames) {
    CaptureTimeline timeline(kNative);
    timeline.OnPacket(0, kFrames, false);
    timeline.OnPacket(kNativeStep, kFrames, false);

    // 4800 native frames vanish: 100 ms, which is 1600 target frames
    const std::uint64_t lost = timeline.OnPacket(2 * kNativeStep + 4800, kFrames, true);

    EXPECT_EQ(lost, 1600u);
    EXPECT_EQ(timeline.TotalLost(), 1600u);
    EXPECT_EQ(timeline.DiscontinuityFlags(), 1u);
}

TEST(CaptureTimeline, ALossIsReportedOnceNotEveryPacket) {
    CaptureTimeline timeline(kNative);
    timeline.OnPacket(0, kFrames, false);
    timeline.OnPacket(kNativeStep + 4800, kFrames, true);
    ASSERT_EQ(timeline.TotalLost(), 1600u);

    // Positions keep the same offset forever; no new loss may be invented
    for (int packet = 2; packet < 50; ++packet) {
        EXPECT_EQ(timeline.OnPacket(packet * kNativeStep + 4800, kFrames, false), 0u);
    }

    EXPECT_EQ(timeline.TotalLost(), 1600u);
}

TEST(CaptureTimeline, NonIntegralRatioNeverDriftsIntoPhantomLoss) {
    // 44.1 kHz native against 100-frame packets: every real position is the
    // rounded value of a non-integral ideal, so sub-frame wobble is constant
    CaptureTimeline timeline(44100);
    double ideal = 0.0;

    for (int packet = 0; packet < 2000; ++packet) {
        const auto position = static_cast<std::uint64_t>(std::llround(ideal));
        EXPECT_EQ(timeline.OnPacket(position, 100, false), 0u) << "packet " << packet;
        ideal += 100.0 * 44100.0 / 16000.0;
    }

    EXPECT_EQ(timeline.TotalLost(), 0u);
}

TEST(CaptureTimeline, ABackwardPositionClampsAndRecovers) {
    CaptureTimeline timeline(kNative);
    timeline.OnPacket(0, kFrames, false);
    timeline.OnPacket(kNativeStep, kFrames, false);

    // A device hiccup reports an earlier position; that is not loss
    EXPECT_EQ(timeline.OnPacket(kNativeStep / 2, kFrames, false), 0u);

    // And when positions resume where they should be, still no phantom loss
    EXPECT_EQ(timeline.OnPacket(3 * kNativeStep, kFrames, false), 0u);
    EXPECT_EQ(timeline.TotalLost(), 0u);
}

TEST(CaptureTimeline, NativeEqualToTargetIsTheIdentityCase) {
    CaptureTimeline timeline(16000);
    timeline.OnPacket(0, kFrames, false);

    const std::uint64_t lost = timeline.OnPacket(kFrames + 320, kFrames, false);

    EXPECT_EQ(lost, 320u);
}

TEST(CaptureTimeline, FlagsAreCountedIndependentlyOfPositions) {
    CaptureTimeline timeline(kNative);
    timeline.OnPacket(0, kFrames, false);

    // A flagged packet with clean positions: corroboration signal only
    EXPECT_EQ(timeline.OnPacket(kNativeStep, kFrames, true), 0u);

    EXPECT_EQ(timeline.DiscontinuityFlags(), 1u);
    EXPECT_EQ(timeline.TotalLost(), 0u);
}

}  // namespace
}  // namespace sotto::audio
