#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ports/transcriber.hpp"

namespace sotto::models {
class ModelStore;
class OvRuntime;
}  // namespace sotto::models

namespace sotto::metrics {
class Registry;
}  // namespace sotto::metrics

namespace sotto::asr {

using DecodeFn = std::function<std::vector<Turn>(std::span<const float>, std::uint64_t)>;

using DecodeLoader = std::function<DecodeFn()>;

// Whisper behind the transcriber port. Submit only enqueues; one worker
// thread loads the pipeline, then decodes, so neither startup nor capture
// ever waits on the GPU. Windows queued while the model loads are decoded
// once it is ready: at 30x realtime the backlog clears in seconds, and
// Finish waits for it, so a finalised transcript is always complete
class WhisperTranscriber : public ITranscriber {
   public:
    // device_override replaces the manifest device for LIVE windows. Clips
    // (the finalise re-decode) keep the manifest device: on the NPU the
    // re-decode is the longest stage at ~11x, and the GPU sits idle at stop,
    // so one burst there converges low-power mode with GPU-mode latency for
    // seconds of draw - the capture phase keeps every watt it saved
    WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                       std::string device_override = "", metrics::Registry* metrics = nullptr);
    explicit WhisperTranscriber(DecodeFn decode);  // Tests inject the decode
    explicit WhisperTranscriber(DecodeLoader loader, metrics::Registry* metrics = nullptr,
                                DecodeLoader clip_loader = {});  // Tests pace the load
    WhisperTranscriber(DecodeFn decode, DecodeFn clip_decode);   // Tests split the devices
    ~WhisperTranscriber() override;

    void Begin(ITurnSink& sink) override;
    void Submit(std::span<const float> frames, std::uint64_t first_frame,
                std::uint64_t first_new_frame = 0) override;
    void Finish() override;

    // Rides the same worker queue after any pending live windows, so the one
    // pipeline is never contended and the live transcript is never delayed.
    // Blocks until decoded; never touches the sink or the dedup backstop
    std::string DecodeClip(std::span<const float> frames, std::uint64_t first_frame) override;

    // Frees the pipeline once the queues drain; the next Submit reloads it
    void Release() override;

   private:
    struct Window {
        std::vector<float> frames;
        std::uint64_t first_frame;
        std::uint64_t first_new_frame;
    };
    struct Clip {
        std::vector<float> frames;
        std::uint64_t first_frame;
        std::promise<std::string> text;
    };

    void WorkerLoop();
    void LoadIfPending();
    void RecordDecode(std::size_t frames, std::chrono::steady_clock::time_point t0);

    DecodeLoader factory_;  // The reload recipe Release re-arms from
    DecodeLoader loader_;
    DecodeFn decode_;  // Worker-thread only once the loader has run
    DecodeLoader clip_factory_;
    DecodeLoader clip_loader_;
    DecodeFn clip_decode_;  // Empty: clips share the live pipeline
    metrics::Registry* metrics_ = nullptr;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Window> queue_;
    std::deque<Clip> clips_;
    ITurnSink* sink_ = nullptr;
    std::optional<Turn> previous_turn_;  // for the boundary dedup backstop
    bool busy_ = false;
    bool stopping_ = false;
    bool release_requested_ = false;
    std::thread worker_;
};

}  // namespace sotto::asr
