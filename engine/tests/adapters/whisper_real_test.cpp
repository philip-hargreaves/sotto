#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "ports/audio_source.hpp"

namespace sotto::asr {
namespace {

constexpr const char* kWav =
    "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav";

std::vector<float> First30Seconds(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error(std::string("missing dev wav: ") + kWav);
    }
    in.seekg(44);  // PCM16 mono 16 kHz, header skipped
    std::vector<std::int16_t> pcm(30 * audio::kSampleRate);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

struct RecordingSink : ITurnSink {
    std::mutex mutex;
    std::vector<Turn> turns;

    void OnTurn(const Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }
};

TEST(WhisperReal, TranscribesRealSpeechWithTimingsInsideTheWindow) {
    const auto frames = First30Seconds(kWav);
    const models::ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));
    models::OvRuntime runtime;

    WhisperTranscriber transcriber(store, runtime);
    RecordingSink sink;
    transcriber.Begin(sink);

    const auto start = std::chrono::steady_clock::now();
    transcriber.Submit(frames, 0);
    transcriber.Finish();
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

    ASSERT_FALSE(sink.turns.empty());
    for (const auto& turn : sink.turns) {
        EXPECT_FALSE(turn.text.empty());
        EXPECT_LE(turn.first_frame + turn.frame_count, frames.size() + audio::kSampleRate);
    }
    std::printf("turns: %zu, %.1fx realtime, first: \"%s\"\n", sink.turns.size(),
                30.0 / took.count(), sink.turns[0].text.c_str());
}

}  // namespace
}  // namespace sotto::asr
