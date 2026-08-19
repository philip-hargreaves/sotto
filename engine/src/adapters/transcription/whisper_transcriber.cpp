#include "adapters/transcription/whisper_transcriber.hpp"

#include <cstdio>
#include <memory>
#include <openvino/genai/whisper_pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "core/turn_assembly.hpp"
#include "ports/audio_source.hpp"

namespace sotto::asr {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

DecodeFn MakeWhisperDecode(const models::ModelStore& store, models::OvRuntime& runtime,
                           const std::string& device_override) {
    const models::ModelInfo& info = store.Resolve("asr", "default");
    store.Verify(info);
    const std::string device =
        runtime.ResolveDevice(device_override.empty() ? info.device : device_override);
    std::fprintf(stderr, "sotto-engine: asr on %s\n", device.c_str());

    auto pipeline = std::make_shared<ov::genai::WhisperPipeline>(
        info.dir, device, ov::AnyMap{{"CACHE_DIR", (info.dir / ".cache").string()}});
    auto config = pipeline->get_generation_config();
    config.language = "<|en|>";
    config.task = "transcribe";
    config.return_timestamps = true;

    // Transcript-tail conditioning (initial_prompt) was measured here and
    // rejected: it worsened WER even with register effects folded out
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

WhisperTranscriber::WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                                       std::string device_override)
    : WhisperTranscriber(DecodeLoader([&store, &runtime, device = std::move(device_override)] {
          return MakeWhisperDecode(store, runtime, device);
      })) {}

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
    // The dedup backstop never reaches across sessions
    previous_turn_.reset();
}

void WhisperTranscriber::Submit(std::span<const float> frames, std::uint64_t first_frame,
                                std::uint64_t first_new_frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({{frames.begin(), frames.end()}, first_frame, first_new_frame});
    }
    cv_.notify_all();
}

void WhisperTranscriber::Finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return (queue_.empty() && !busy_) || stopping_; });
}

std::string WhisperTranscriber::DecodeClip(std::span<const float> frames,
                                           std::uint64_t first_frame) {
    std::future<std::string> text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {};  // the worker no longer serves clips
        clips_.push_back({{frames.begin(), frames.end()}, first_frame, {}});
        text = clips_.back().text.get_future();
    }
    cv_.notify_all();
    return text.get();
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
    // Clips yield to live windows but are never starved: one clip per two
    // windows keeps speculation alive under an accelerated-replay backlog
    int windows_since_clip = 0;
    while (!stopping_) {
        cv_.wait(lock, [this] { return !queue_.empty() || !clips_.empty() || stopping_; });
        if (stopping_) break;

        if (!clips_.empty() && (queue_.empty() || windows_since_clip >= 2)) {
            windows_since_clip = 0;
            Clip clip = std::move(clips_.front());
            clips_.pop_front();
            busy_ = true;
            lock.unlock();
            std::string text;
            try {
                if (decode_) {
                    for (const Turn& turn : decode_(clip.frames, clip.first_frame)) {
                        if (turn.text.empty()) continue;
                        if (!text.empty()) text += ' ';
                        text += turn.text;
                    }
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            clip.text.set_value(std::move(text));
            lock.lock();
            busy_ = false;
            cv_.notify_all();
            continue;
        }

        if (queue_.empty()) continue;  // spurious wake
        Window window = std::move(queue_.front());
        queue_.pop_front();
        ++windows_since_clip;
        busy_ = true;
        ITurnSink* sink = sink_;
        std::optional<Turn> previous = previous_turn_;
        lock.unlock();

        // A failed decode loses this window's turns, never the session;
        // the audio is already stored
        try {
            if (decode_) {
                auto turns = decode_(window.frames, window.first_frame);
                AnchorFirstTurn(turns, window.first_frame);
                DropReheardTurns(turns, window.first_new_frame);
                // The dedup backstop acts at the window boundary only: within
                // a window the decoder never duplicates its own segments, and
                // stripping there would eat genuine repetition
                if (!turns.empty() && previous.has_value()) {
                    StripBoundaryDuplicates(*previous, turns.front());
                }
                for (Turn& turn : turns) {
                    if (turn.text.empty()) continue;
                    if (sink != nullptr) sink->OnTurn(turn);
                    previous = turn;
                }
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }

        lock.lock();
        previous_turn_ = std::move(previous);
        busy_ = false;
        cv_.notify_all();
    }
    // A caller may still be blocked on a pending clip at shutdown
    for (auto& clip : clips_) clip.text.set_value({});
}

}  // namespace sotto::asr
