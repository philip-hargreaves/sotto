#include "core/voice_enrolment.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace ambient::audio {
namespace {

// Speech wherever the hop has energy
struct EnergyVad : IStreamingVad {
    float SpeechProbability(std::span<const float> hop) override {
        for (const float x : hop) {
            if (x != 0.0f) return 0.9f;
        }
        return 0.0f;
    }
    void Reset() override {}
};

TEST(EnrolmentSink, KeepsOnlyTheSpeechAndStopsAtTheWindow) {
    EnergyVad vad;
    int stops = 0;
    std::vector<EnrolProgress> progress;
    EnrolmentSink sink(
        vad, 16000, [&] { ++stops; }, [&](const EnrolProgress& p) { progress.push_back(p); });
    const std::vector<float> loud(8000, 0.3f);
    const std::vector<float> silent(8000, 0.0f);
    sink.OnAudio(loud, 0);
    EXPECT_EQ(stops, 0);
    sink.OnAudio(silent, 0);
    EXPECT_EQ(stops, 1) << "one second elapsed: the source is asked to stop";
    sink.OnAudio(loud, 0);
    EXPECT_EQ(stops, 1) << "asked once";
    sink.OnEnd({SourceEndReason::kStopped, ""});

    const auto capture = sink.Take();
    EXPECT_NEAR(capture.elapsed_s, 1.5, 1e-6);
    // 8000 frames is 15 full hops of 512 (7680) plus a remainder that joins the next chunk
    EXPECT_GT(capture.speech.size(), 14000u);
    EXPECT_LT(capture.speech.size(), 16500u) << "the silent second is not speech";
    ASSERT_FALSE(progress.empty());
    EXPECT_GT(progress.back().level.level, 0.0f);
    EXPECT_TRUE(EnrolRejection(capture, false, 0.5).empty());
}

TEST(EnrolRejection, NamesCancellationDeviceLossAndTooLittleSpeech) {
    EnrolCapture capture;
    capture.speech.assign(16000, 0.1f);
    capture.end = {SourceEndReason::kStopped, ""};
    EXPECT_EQ(EnrolRejection(capture, true), "cancelled");
    EXPECT_NE(EnrolRejection(capture, false).find("not enough clear speech"), std::string::npos);
    capture.end = {SourceEndReason::kDeviceLost, "unplugged"};
    EXPECT_EQ(EnrolRejection(capture, false, 0.5), "microphone unplugged");
    capture.end = {SourceEndReason::kStopped, ""};
    EXPECT_TRUE(EnrolRejection(capture, false, 0.5).empty());
}

}  // namespace
}  // namespace ambient::audio
