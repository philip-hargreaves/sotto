#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ports/audio_source.hpp"

namespace ambient::audio {

// Replays a crashed session's stored audio at full speed, then hands the
// sink to the live source; downstream sees one continuous capture stream
class ResumeSource : public IAudioSource {
   public:
    ResumeSource(std::vector<float> stored, std::unique_ptr<IAudioSource> live)
        : stored_(std::move(stored)), live_(std::move(live)) {}

    void Run(IAudioSink& sink) override {
        constexpr std::size_t kPacketFrames = 480;
        std::size_t at = 0;
        while (at < stored_.size()) {
            if (stop_requested_.load(std::memory_order_relaxed)) {
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
            }
            while (paused_.load(std::memory_order_relaxed) &&
                   !stop_requested_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            const std::size_t count = std::min(kPacketFrames, stored_.size() - at);
            sink.OnAudio(std::span<const float>(stored_.data() + at, count), 0);
            at += count;
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            sink.OnEnd({SourceEndReason::kStopped, ""});
            return;
        }
        live_->Run(sink);
    }

    void RequestStop() override {
        stop_requested_.store(true, std::memory_order_relaxed);
        live_->RequestStop();
    }

    void SetPaused(bool paused) override {
        paused_.store(paused, std::memory_order_relaxed);
        live_->SetPaused(paused);
    }

    void SetMonitor(bool monitor) override {
        live_->SetMonitor(monitor);
    }

   private:
    std::vector<float> stored_;
    std::unique_ptr<IAudioSource> live_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
};

}  // namespace ambient::audio
