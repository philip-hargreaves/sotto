#include "adapters/transcription/whisper_transcriber.hpp"

#include <cstdio>
#include <memory>
#include <openvino/genai/whisper_pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "ports/audio_source.hpp"

namespace sotto::asr {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

DecodeFn MakeWhisperDecode(const models::ModelStore& store, models::OvRuntime& runtime) {
    const models::ModelInfo& info = store.Resolve("asr", "default");
    store.Verify(info);
    const std::string device = runtime.ResolveDevice(info.device);

    auto pipeline = std::make_shared<ov::genai::WhisperPipeline>(
        info.dir, device, ov::AnyMap{{"CACHE_DIR", (info.dir / ".cache").string()}});
    auto config = pipeline->get_generation_config();
    config.language = "<|en|>";
    config.task = "transcribe";
    config.return_timestamps = true;

    return [pipeline, config](std::span<const float> frames, std::uint64_t first_frame) {
        const ov::genai::RawSpeechInput audio(frames.begin(), frames.end());
        auto result = pipeline->generate(audio, config);

        std::vector<Turn> turns;
        if (result.chunks.has_value()) {
            for (const auto& chunk : *result.chunks) {
                Turn turn;
                turn.first_frame =
                    first_frame + static_cast<std::uint64_t>(chunk.start_ts * audio::kSampleRate);
                // An open-ended last chunk reports end_ts -1
                const float end = chunk.end_ts > chunk.start_ts
                                      ? chunk.end_ts
                                      : static_cast<float>(frames.size()) / audio::kSampleRate;
                turn.frame_count =
                    static_cast<std::uint64_t>((end - chunk.start_ts) * audio::kSampleRate);
                turn.text = Trimmed(chunk.text);
                if (!turn.text.empty()) turns.push_back(std::move(turn));
            }
        } else {
            Turn turn;
            turn.first_frame = first_frame;
            turn.frame_count = frames.size();
            turn.text = Trimmed(result);
            if (!turn.text.empty()) turns.push_back(std::move(turn));
        }
        return turns;
    };
}

}  // namespace

WhisperTranscriber::WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime)
    : WhisperTranscriber(
          DecodeLoader([&store, &runtime] { return MakeWhisperDecode(store, runtime); })) {}

WhisperTranscriber::WhisperTranscriber(DecodeFn decode) : decode_(std::move(decode)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

WhisperTranscriber::WhisperTranscriber(DecodeLoader loader) : loader_(std::move(loader)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

WhisperTranscriber::~WhisperTranscriber() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void WhisperTranscriber::Begin(ITurnSink& sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = &sink;
}

void WhisperTranscriber::Submit(std::span<const float> frames, std::uint64_t first_frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({{frames.begin(), frames.end()}, first_frame});
    }
    cv_.notify_all();
}

void WhisperTranscriber::Finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return (queue_.empty() && !busy_) || stopping_; });
}

void WhisperTranscriber::WorkerLoop() {
    // Load here, not in the constructor: the engine serves and sessions
    // record while the pipeline compiles; queued windows decode afterwards.
    // A failed load drains windows without turns, so nothing hangs
    if (loader_) {
        try {
            decode_ = loader_();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: transcription unavailable (%s)\n", e.what());
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
        if (stopping_) break;

        Window window = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        ITurnSink* sink = sink_;
        lock.unlock();

        // A failed decode loses this window's turns, never the session;
        // the audio is already stored
        try {
            if (decode_) {
                for (const auto& turn : decode_(window.frames, window.first_frame)) {
                    if (sink != nullptr) sink->OnTurn(turn);
                }
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }

        lock.lock();
        busy_ = false;
        cv_.notify_all();
    }
}

}  // namespace sotto::asr
