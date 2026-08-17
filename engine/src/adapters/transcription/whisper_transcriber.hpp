#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "ports/transcriber.hpp"

namespace sotto::models {
class ModelStore;
class OvRuntime;
}  // namespace sotto::models

namespace sotto::asr {

using DecodeFn = std::function<std::vector<Turn>(std::span<const float>, std::uint64_t)>;

// Whisper behind the transcriber port. Submit only enqueues; one worker
// thread decodes, so capture never waits on the GPU. Assumes decode runs
// faster than realtime, so the queue stays near empty
class WhisperTranscriber : public ITranscriber {
   public:
    WhisperTranscriber(const models::ModelStore& store, models::OvRuntime& runtime);
    explicit WhisperTranscriber(DecodeFn decode);  // Tests inject the decode
    ~WhisperTranscriber() override;

    void Begin(ITurnSink& sink) override;
    void Submit(std::span<const float> frames, std::uint64_t first_frame) override;
    void Finish() override;

   private:
    struct Window {
        std::vector<float> frames;
        std::uint64_t first_frame;
    };

    void WorkerLoop();

    DecodeFn decode_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Window> queue_;
    ITurnSink* sink_ = nullptr;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace sotto::asr
