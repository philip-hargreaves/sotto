#include "adapters/audio/wasapi_capture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace ambient::audio {
namespace {

// The engine drops a few milliseconds while the stream settles after Start
// (measured here: 31 to 191 frames, run to run), so losses are split into a
// startup window and steady state. Only steady state must be lossless.
constexpr std::uint64_t kWarmupPackets = 50;  // ~500 ms of 10 ms packets

// Fields are written on the capture thread and read only after the join
struct RecordingSink : IAudioSink {
    std::uint64_t total_frames = 0;
    std::uint64_t packets = 0;
    std::uint64_t lost_in_warmup = 0;
    std::uint64_t lost_in_steady_state = 0;
    std::vector<SourceEnd> ends;

    void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override {
        total_frames += frames.size();
        ++packets;
        (packets <= kWarmupPackets ? lost_in_warmup : lost_in_steady_state) += lost_frames;
    }

    void OnEnd(const SourceEnd& end) override {
        ends.push_back(end);
    }
};

TEST(WasapiCapture, CapturesRealAudioThenStopsCleanly) {
    WasapiCapture source;
    RecordingSink sink;

    std::thread runner([&source, &sink] { source.Run(sink); });
    std::this_thread::sleep_for(std::chrono::seconds(2));
    source.RequestStop();
    runner.join();

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kStopped) << sink.ends[0].detail;
    // Roughly two seconds of 16 kHz audio, bounded loosely for scheduling
    EXPECT_GT(sink.total_frames, 16000u);
    EXPECT_LT(sink.total_frames, 48000u);
    EXPECT_GT(sink.packets, 20u);
    // The startup transient is real and must stay visible, but bounded
    EXPECT_LT(sink.lost_in_warmup, 1600u) << "more than 100 ms lost at startup";
    EXPECT_EQ(sink.lost_in_steady_state, 0u) << "steady state must be lossless";
}

TEST(WasapiCapture, StopBeforeRunEndsWithoutCapturing) {
    WasapiCapture source;
    RecordingSink sink;
    source.RequestStop();

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kStopped);
    EXPECT_EQ(sink.total_frames, 0u);
}

TEST(WasapiCapture, AnUnknownEndpointFailsWithTheCall) {
    WasapiCapture source(L"{not-a-real-endpoint-id}");
    RecordingSink sink;

    source.Run(sink);

    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed);
    EXPECT_NE(sink.ends[0].detail.find("GetDevice"), std::string::npos) << sink.ends[0].detail;
    EXPECT_EQ(sink.total_frames, 0u);
}

}  // namespace
}  // namespace ambient::audio
