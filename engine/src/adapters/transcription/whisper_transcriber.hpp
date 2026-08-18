#pragma once

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
    WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime);
    explicit WhisperTranscriber(DecodeFn decode);      // Tests inject the decode
    explicit WhisperTranscriber(DecodeLoader loader);  // Tests pace the load
    ~WhisperTranscriber() override;

    void Begin(ITurnSink& sink) override;
    void Submit(std::span<const float> frames, std::uint64_t first_frame,
                std::uint64_t first_new_frame = 0) override;
    void Finish() override;

    // Rides the same worker queue after any pending live windows, so the one
    // pipeline is never contended and the live transcript is never delayed.
    // Blocks until decoded; never touches the sink or the dedup backstop
    std::string DecodeClip(std::span<const float> frames, std::uint64_t first_frame) override;

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

    DecodeLoader loader_;
    DecodeFn decode_;  // Worker-thread only once the loader has run
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Window> queue_;
    std::deque<Clip> clips_;
    ITurnSink* sink_ = nullptr;
    std::optional<Turn> previous_turn_;  // for the boundary dedup backstop
    bool busy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace sotto::asr
