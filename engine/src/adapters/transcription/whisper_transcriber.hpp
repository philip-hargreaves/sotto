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

namespace ambient::models {
class ModelStore;
class OvRuntime;
}  // namespace ambient::models

namespace ambient::metrics {
class Registry;
}  // namespace ambient::metrics

namespace ambient::asr {

// One decode: Whisper's chunks (the cut-point source) and, with the
// word-timestamp export, its words (the padded-clip trim). A plain turn list
// is chunks only
struct ClipDecode {
    std::vector<Turn> chunks;
    std::vector<Turn> words;
    ClipDecode() = default;
    ClipDecode(std::vector<Turn> c) : chunks(std::move(c)) {}  // NOLINT(google-explicit-constructor)
    ClipDecode(std::vector<Turn> c, std::vector<Turn> w) : chunks(std::move(c)), words(std::move(w)) {}
};

using DecodeFn = std::function<ClipDecode(std::span<const float>, std::uint64_t)>;

using DecodeLoader = std::function<DecodeFn()>;

// Whisper behind the transcriber port: one worker thread loads then
// decodes, so capture never waits on the GPU; Finish drains the backlog
class WhisperTranscriber : public ITranscriber {
   public:
    // device_override applies to LIVE windows only; clips keep the manifest
    // GPU, so low-power finalise pays GPU rate (the burst)
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

    // Same worker queue, after pending windows; blocks until decoded and never
    // touches the sink
    std::string DecodeClip(std::span<const float> frames, std::uint64_t first_frame) override;
    std::vector<Turn> DecodeClipChunks(std::span<const float> frames,
                                       std::uint64_t first_frame) override;

    // Frees the pipeline once the queues drain; the next Submit reloads it
    void Release() override;

    std::vector<std::uint64_t> TakeClipCuts() override;
    std::vector<std::uint64_t> TakePunctuationCuts() override;

   private:
    struct Window {
        std::vector<float> frames;
        std::uint64_t first_frame;
        std::uint64_t first_new_frame;
    };
    struct Clip {
        std::vector<float> frames;
        std::uint64_t first_frame;
        std::promise<std::vector<Turn>> chunks;
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
    std::vector<std::uint64_t> clip_cuts_;   // segment edges inside decoded clips
    std::vector<std::uint64_t> punct_cuts_;  // sentence ends inside decoded clips, estimated
    bool busy_ = false;
    bool stopping_ = false;
    bool release_requested_ = false;
    std::thread worker_;
};

}  // namespace ambient::asr
