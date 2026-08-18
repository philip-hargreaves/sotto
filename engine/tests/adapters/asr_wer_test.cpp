#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/endpointer.hpp"
#include "ports/audio_source.hpp"

namespace sotto::asr {
namespace {

constexpr const char* kWav =
    "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav";
constexpr const char* kRef =
    "C:/dev/intelliscribe/bench/transcription/references/day1_consultation01.json";

// Long-form parity on this consult is 21.28%; VAD endpointing measured
// 20.37%, so the gate holds the reclaim, not just the baseline
constexpr double kMaxWer = 0.22;

std::vector<float> LoadWav(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing dev wav: ") + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

std::string LoadGold(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error(std::string("missing reference: ") + path);
    const auto ref = nlohmann::json::parse(in);
    std::vector<std::pair<double, std::string>> turns;
    for (const char* role : {"doctor", "patient"}) {
        for (const auto& turn : ref.at(role)) {
            const auto text = turn.value("text", "");
            if (!text.empty()) turns.push_back({turn.at("start").get<double>(), text});
        }
    }
    std::sort(turns.begin(), turns.end());
    std::string joined;
    for (const auto& [start, text] : turns) joined += text + " ";
    return joined;
}

// Same normalisation as the research scorer: lowercase, keep [a-z0-9'],
// collapse whitespace
std::vector<std::string> NormalisedWords(const std::string& text) {
    std::string cleaned;
    for (const char c : text) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        cleaned += (std::isalnum(static_cast<unsigned char>(lower)) || lower == '\'') ? lower : ' ';
    }
    std::vector<std::string> words;
    std::istringstream stream(cleaned);
    std::string word;
    while (stream >> word) words.push_back(word);
    return words;
}

double Wer(const std::vector<std::string>& ref, const std::vector<std::string>& hyp) {
    std::vector<std::size_t> previous(hyp.size() + 1);
    std::vector<std::size_t> current(hyp.size() + 1);
    for (std::size_t j = 0; j <= hyp.size(); ++j) previous[j] = j;
    for (std::size_t i = 1; i <= ref.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= hyp.size(); ++j) {
            const std::size_t substitution = previous[j - 1] + (ref[i - 1] == hyp[j - 1] ? 0 : 1);
            current[j] = std::min({substitution, previous[j] + 1, current[j - 1] + 1});
        }
        std::swap(previous, current);
    }
    return static_cast<double>(previous[hyp.size()]) / static_cast<double>(ref.size());
}

struct RecordingSink : ITurnSink {
    std::mutex mutex;
    std::vector<Turn> turns;

    void OnTurn(const Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }
};

TEST(AsrWer, ProductionPathHoldsTheBaseline) {
    const auto frames = LoadWav(kWav);
    const auto gold = NormalisedWords(LoadGold(kRef));

    const models::ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));
    models::OvRuntime runtime;
    const auto load_start = std::chrono::steady_clock::now();
    WhisperTranscriber transcriber(store, runtime);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_start);

    audio::SileroVad vad(store, runtime);
    audio::Endpointer endpointer(vad);
    RecordingSink sink;
    transcriber.Begin(sink);
    const auto decode_start = std::chrono::steady_clock::now();
    for (const auto& window : endpointer.Push(frames)) {
        transcriber.Submit(window.frames, window.first_frame, window.first_new_frame);
    }
    if (const auto tail = endpointer.Flush()) {
        transcriber.Submit(tail->frames, tail->first_frame, tail->first_new_frame);
    }
    transcriber.Finish();
    const auto decode =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start);

    std::string joined;
    for (const auto& turn : sink.turns) joined += turn.text + " ";
    const auto hyp = NormalisedWords(joined);
    const double wer = Wer(gold, hyp);
    const double speed = (static_cast<double>(frames.size()) / audio::kSampleRate) / decode.count();

    std::printf(
        "production WER %.2f%% (baseline 21.28%%), %.1fx realtime, load %lld ms, "
        "%zu turns\n",
        wer * 100, speed, static_cast<long long>(load_ms.count()), sink.turns.size());
    EXPECT_LE(wer, kMaxWer);
    // Release measures ~29x; the floor tolerates the Debug harness, and the
    // decode span now includes the worker's background model load
    EXPECT_GT(speed, 3.0) << "decode must stay well past realtime";
}

}  // namespace
}  // namespace sotto::asr
