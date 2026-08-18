#include "adapters/vad/silero_vad.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "ports/audio_source.hpp"

namespace sotto::audio {
namespace {

std::vector<float> FirstSeconds(int seconds) {
    constexpr const char* kWav =
        "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav";
    std::ifstream in(kWav, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing dev wav: ") + kWav);
    in.seekg(44);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(seconds) * kSampleRate);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

// Thresholds under test are the validated hysteresis: enter 0.40, exit 0.25
TEST(SileroVad, SeparatesSpeechFromSilenceAtTheShippedThresholds) {
    const models::ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));
    models::OvRuntime runtime;
    SileroVad vad(store, runtime);

    const auto speech = FirstSeconds(10);
    float max_speech = 0;
    std::size_t hops = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t at = 0; at + kVadHopFrames <= speech.size(); at += kVadHopFrames) {
        max_speech = std::max(max_speech,
                              vad.SpeechProbability(std::span(speech).subspan(at, kVadHopFrames)));
        ++hops;
    }
    const auto per_hop =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        static_cast<double>(hops);

    vad.Reset();
    const std::vector<float> silence(kVadHopFrames, 0.0f);
    float max_silence = 0;
    for (int i = 0; i < 30; ++i) {
        max_silence = std::max(max_silence, vad.SpeechProbability(silence));
    }

    std::printf("speech max %.2f, silence max %.2f, %.3f ms per hop\n", max_speech, max_silence,
                per_hop);
    EXPECT_GT(max_speech, 0.40f) << "real speech must clear the enter threshold";
    EXPECT_LT(max_silence, 0.25f) << "digital silence must stay under the exit threshold";
    // Release measures 0.097 ms, matching the spec's ~0.1; the slack is Debug
    // harness overhead, and a 32 ms hop budget keeps inline capture safe
    EXPECT_LT(per_hop, 2.0) << "inline on the capture path requires margin";
}

TEST(SileroVad, ResetClearsTheRecurrentState) {
    const models::ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));
    models::OvRuntime runtime;
    SileroVad vad(store, runtime);

    const auto speech = FirstSeconds(2);
    for (std::size_t at = 0; at + kVadHopFrames <= speech.size(); at += kVadHopFrames) {
        vad.SpeechProbability(std::span(speech).subspan(at, kVadHopFrames));
    }

    vad.Reset();
    const std::vector<float> silence(kVadHopFrames, 0.0f);
    EXPECT_LT(vad.SpeechProbability(silence), 0.25f)
        << "state from the previous session must not leak";
}

TEST(SileroVad, AWrongHopSizeIsRefused) {
    const models::ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));
    models::OvRuntime runtime;
    SileroVad vad(store, runtime);
    const std::vector<float> wrong(100, 0.0f);
    EXPECT_THROW(vad.SpeechProbability(wrong), std::runtime_error);
}

}  // namespace
}  // namespace sotto::audio
