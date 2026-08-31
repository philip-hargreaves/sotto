#include "core/metrics.hpp"

#include <gtest/gtest.h>

namespace ambient::metrics {
namespace {

TEST(Metrics, RecordsAndSnapshots) {
    Registry registry;
    registry.RecordDevice("asr", "GPU.0");
    registry.RecordLoad("asr", 2.1);
    registry.RecordStage("sealed", 2.0);
    registry.RecordDecode(30.0, 1.0);
    registry.RecordDecode(30.0, 1.0);
    registry.RecordSession(541.5, 3, 108);
    registry.RecordTranscript(73, 2);

    const auto s = registry.Take();
    EXPECT_EQ(s.devices.at("asr"), "GPU.0");
    EXPECT_EQ(s.load_seconds.at("asr"), 2.1);
    EXPECT_EQ(s.stage_seconds.at("sealed"), 2.0);
    EXPECT_EQ(s.decoded_audio_seconds, 60.0);
    EXPECT_EQ(s.decode_busy_seconds, 2.0);
    EXPECT_EQ(s.session_audio_seconds, 541.5);
    EXPECT_EQ(s.lost_frames, 3u);
    EXPECT_EQ(s.diar_ticks, 108);
    EXPECT_EQ(s.turns, 73);
    EXPECT_EQ(s.clusters, 2);
}

TEST(Metrics, ANewSessionKeepsDevicesAndLoadsOnly) {
    Registry registry;
    registry.RecordDevice("asr", "NPU");
    registry.RecordLoad("asr", 13.4);
    registry.RecordDecode(30.0, 3.0);
    registry.RecordStage("sealed", 19.6);
    registry.RecordTranscript(73, 2);

    registry.BeginSession(true, 1.0);

    const auto s = registry.Take();
    EXPECT_EQ(s.devices.at("asr"), "NPU");
    EXPECT_EQ(s.load_seconds.at("asr"), 13.4);
    EXPECT_TRUE(s.stage_seconds.empty());
    EXPECT_EQ(s.decoded_audio_seconds, 0);
    EXPECT_EQ(s.turns, -1);
    EXPECT_TRUE(s.replay);
    EXPECT_EQ(s.replay_speed, 1.0);
}

}  // namespace
}  // namespace ambient::metrics
