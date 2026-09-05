#include "adapters/transcription/whisper_transcriber.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <openvino/genai/whisper_pipeline.hpp>
#include <utility>

#include "adapters/host/gpu_lease.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "core/env_flag.hpp"
#include "core/metrics.hpp"
#include "core/turn_assembly.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"

namespace ambient::asr {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(' ') - begin + 1);
}

DecodeFn MakeWhisperDecode(const models::ModelStore& store, models::OvRuntime& runtime,
                           const std::string& device_override, metrics::Registry* metrics,
                           const char* role = "asr") {
    const models::ModelInfo& info = store.Resolve("asr", "default");
    store.Verify(info);
    const std::string device =
        runtime.ResolveDevice(device_override.empty() ? info.device : device_override);
    std::fprintf(stderr, "ambient-engine: %s on %s\n", role, device.c_str());
    if (metrics != nullptr) metrics->RecordDevice(role, device);

    ov::AnyMap properties{{"CACHE_DIR", (info.dir / ".cache").string()}};
    auto pipeline = std::make_shared<ov::genai::WhisperPipeline>(info.dir, device, properties);
    auto config = pipeline->get_generation_config();
    config.language = "<|en|>";
    config.task = "transcribe";
    config.return_timestamps = true;

    // Transcript-tail conditioning (initial_prompt) was measured here and
    // rejected: it worsened WER even with register effects folded out
    return [pipeline, config](std::span<const float> frames, std::uint64_t first_frame) {
        const ov::genai::RawSpeechInput audio(frames.begin(), frames.end());
        const auto lease = host::GpuLease::Global().Acquire();
        if (lease.waited() > 0.25) {
            std::fprintf(stderr, "ambient-engine: asr waited %.2f s for the GPU lease\n",
                         lease.waited());
        }
        auto result = pipeline->generate(audio, config);

        std::vector<Turn> turns;
        if (result.chunks.has_value()) {
            const float clip_end = static_cast<float>(frames.size()) / audio::kSampleRate;
            for (const auto& chunk : *result.chunks) {
                Turn turn;
                // Stamps can overrun the clip (the window is padded to 30 s): clamp
                const float start = std::min(std::max(0.0f, chunk.start_ts), clip_end);
                turn.first_frame =
                    first_frame + static_cast<std::uint64_t>(start * audio::kSampleRate);
                // An open-ended last chunk reports end_ts -1
                const float end =
                    chunk.end_ts > chunk.start_ts ? std::min(chunk.end_ts, clip_end) : clip_end;
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
                                       std::string device_override, metrics::Registry* metrics)
    : WhisperTranscriber(DecodeLoader([&store, &runtime, device = device_override, metrics] {
                             return MakeWhisperDecode(store, runtime, device, metrics);
                         }),
                         metrics,
                         // One Whisper on the chosen device; the low-power mode pays
                         // the finalise burst at NPU speed rather than load a GPU copy
                         DecodeLoader{}) {}

WhisperTranscriber::WhisperTranscriber(DecodeFn decode) : decode_(std::move(decode)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

WhisperTranscriber::WhisperTranscriber(DecodeFn decode, DecodeFn clip_decode)
    : decode_(std::move(decode)), clip_decode_(std::move(clip_decode)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

WhisperTranscriber::WhisperTranscriber(DecodeLoader loader, metrics::Registry* metrics,
                                       DecodeLoader clip_loader)
    : factory_(loader),
      loader_(std::move(loader)),
      clip_factory_(clip_loader),
      clip_loader_(std::move(clip_loader)),
      metrics_(metrics) {
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

void WhisperTranscriber::Release() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!factory_) {
        return;  // an injected decode has nothing to reload from
    }
    release_requested_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return !release_requested_ || stopping_; });
}

std::vector<std::uint64_t> WhisperTranscriber::TakeClipCuts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(clip_cuts_, {});
}

std::string WhisperTranscriber::DecodeClip(std::span<const float> frames,
                                           std::uint64_t first_frame) {
    return diar::JoinedText(DecodeClipChunks(frames, first_frame));
}

std::vector<Turn> WhisperTranscriber::DecodeClipChunks(std::span<const float> frames,
                                                       std::uint64_t first_frame) {
    std::future<std::vector<Turn>> chunks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {};  // the worker no longer serves clips
        clips_.push_back({{frames.begin(), frames.end()}, first_frame, {}});
        chunks = clips_.back().chunks.get_future();
    }
    cv_.notify_all();
    return chunks.get();
}

// Load off the hot path; a failed load drains windows without turns, so
// nothing hangs
void WhisperTranscriber::RecordDecode(std::size_t frames,
                                      std::chrono::steady_clock::time_point t0) {
    if (metrics_ != nullptr) {
        metrics_->RecordDecode(
            static_cast<double>(frames) / 16000.0,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
}

void WhisperTranscriber::LoadIfPending() {
    if (!loader_) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
        decode_ = loader_();
        if (metrics_ != nullptr) {
            metrics_->RecordLoad(
                "asr",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient-engine: transcription unavailable (%s)\n", e.what());
    }
    loader_ = {};

    if (clip_loader_) {
        try {
            clip_decode_ = clip_loader_();
        } catch (const std::exception& e) {
            // Clips then share the live pipeline: slower, never wrong
            std::fprintf(stderr, "ambient-engine: finalise stays on the live device (%s)\n",
                         e.what());
        }
        clip_loader_ = {};
    }
}

void WhisperTranscriber::WorkerLoop() {
    LoadIfPending();

    std::unique_lock<std::mutex> lock(mutex_);
    // Clips yield to live windows but are never starved: one clip per two
    // windows keeps speculation alive under an accelerated-replay backlog
    int windows_since_clip = 0;
    while (!stopping_) {
        cv_.wait(lock, [this] {
            return !queue_.empty() || !clips_.empty() || release_requested_ || stopping_;
        });
        if (stopping_) break;

        // Release only once drained; the next work item reloads
        if (release_requested_ && queue_.empty() && clips_.empty()) {
            decode_ = {};
            clip_decode_ = {};
            loader_ = factory_;
            clip_loader_ = clip_factory_;
            release_requested_ = false;
            cv_.notify_all();
            continue;
        }
        if (loader_ && (!queue_.empty() || !clips_.empty())) {
            lock.unlock();
            LoadIfPending();
            lock.lock();
        }

        if (!clips_.empty() && (queue_.empty() || windows_since_clip >= 2)) {
            windows_since_clip = 0;
            Clip clip = std::move(clips_.front());
            clips_.pop_front();
            busy_ = true;
            lock.unlock();
            std::vector<Turn> chunks;
            std::vector<std::uint64_t> cuts;
            try {
                // The burst pipeline when the live device is the slow one
                const DecodeFn& decode = clip_decode_ ? clip_decode_ : decode_;
                if (decode) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const std::uint64_t clip_end = clip.first_frame + clip.frames.size();
                    for (const Turn& turn : decode(clip.frames, clip.first_frame)) {
                        if (turn.text.empty()) continue;
                        chunks.push_back(turn);
                        // Chunk edges: where a short answer inside a long clip
                        // begins and ends
                        for (const std::uint64_t edge :
                             {turn.first_frame, turn.first_frame + turn.frame_count}) {
                            if (edge > clip.first_frame && edge < clip_end) cuts.push_back(edge);
                        }
                    }
                    RecordDecode(clip.frames.size(), t0);
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            // The cuts land before the caller is released, so a TakeClipCuts
            // right after the decode sees them
            lock.lock();
            clip_cuts_.insert(clip_cuts_.end(), cuts.begin(), cuts.end());
            lock.unlock();
            clip.chunks.set_value(std::move(chunks));
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
                const auto t0 = std::chrono::steady_clock::now();
                auto turns = decode_(window.frames, window.first_frame);
                RecordDecode(window.frames.size(), t0);
                AnchorFirstTurn(turns, window.first_frame);
                DropReheardTurns(turns, window.first_new_frame);
                // Boundary-only dedup: within a window, stripping would eat genuine repetition
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
    for (auto& clip : clips_) clip.chunks.set_value({});
}

}  // namespace ambient::asr
