#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "core/level_meter.hpp"
#include "ports/audio_source.hpp"

namespace sotto::audio {

// Session events, delivered on the capture thread
class ISessionEvents {
   public:
    virtual ~ISessionEvents() = default;

    virtual void OnLevel(const LevelReading& reading) = 0;

    // A death mid-session, never a user stop or a completed replay
    virtual void OnInterrupted(SourceEndReason reason, const std::string& detail) = 0;
};

using SourceFactory = std::function<std::unique_ptr<IAudioSource>()>;

// One capture session at a time: a fresh source per start on a guarded
// thread, levels out through the events, and Start blocking until audio
// actually flows so a session clock never starts inside the startup
// transient.
class SessionController {
   public:
    SessionController(SourceFactory factory, ISessionEvents& events,
                      std::chrono::milliseconds settle_timeout = std::chrono::seconds(3))
        : factory_(std::move(factory)), events_(events), settle_timeout_(settle_timeout) {}

    ~SessionController() {
        Stop();
    }
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // True once audio flows; false if the source ended or the deadline
    // passed first, with the reason left in LastEnd
    bool Start() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) {
                return false;
            }
            running_ = true;
            got_audio_ = false;
            ended_ = false;
            stop_requested_ = false;
            lost_frames_ = 0;
            end_ = {};
            meter_ = LevelMeter{};
        }
        source_ = factory_();
        worker_ = std::thread([this] { GuardedRun(); });

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, settle_timeout_, [this] { return got_audio_ || ended_; });
        if (got_audio_) {
            return true;
        }
        lock.unlock();
        Stop();

        // After the join: a stop-shaped end here was ours, so the caller
        // sees the deadline rather than the stop it never asked for
        std::lock_guard<std::mutex> relock(mutex_);
        if (end_.reason == SourceEndReason::kStopped) {
            end_ = {SourceEndReason::kFailed, "no audio arrived before the deadline"};
        }
        return false;
    }

    // Idempotent; a stop is the user's, so it never counts as an interruption
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        if (source_) {
            source_->RequestStop();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }

    bool Running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ && !ended_;
    }

    SourceEnd LastEnd() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return end_;
    }

    std::uint64_t LostFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lost_frames_;
    }

   private:
    struct Sink : IAudioSink {
        SessionController& controller;

        explicit Sink(SessionController& owner) : controller(owner) {}

        void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override {
            {
                std::lock_guard<std::mutex> lock(controller.mutex_);
                controller.lost_frames_ += lost_frames;
                controller.got_audio_ = true;
            }
            controller.cv_.notify_all();
            for (const auto& reading : controller.meter_.Push(frames)) {
                controller.events_.OnLevel(reading);
            }
        }

        void OnEnd(const SourceEnd& end) override {
            bool interrupted = false;
            {
                std::lock_guard<std::mutex> lock(controller.mutex_);
                controller.end_ = end;
                controller.ended_ = true;
                interrupted = controller.got_audio_ && !controller.stop_requested_ &&
                              (end.reason == SourceEndReason::kDeviceLost ||
                               end.reason == SourceEndReason::kFailed);
            }
            controller.cv_.notify_all();
            if (interrupted) {
                controller.events_.OnInterrupted(end.reason, end.detail);
            }
        }
    };

    // An escape from a thread function is std::terminate, so nothing escapes
    void GuardedRun() {
        Sink sink(*this);
        try {
            source_->Run(sink);
        } catch (const std::exception& e) {
            sink.OnEnd(
                {SourceEndReason::kFailed, std::string("capture thread threw: ") + e.what()});
        } catch (...) {
            sink.OnEnd({SourceEndReason::kFailed, "capture thread threw"});
        }
    }

    SourceFactory factory_;
    ISessionEvents& events_;
    std::chrono::milliseconds settle_timeout_;
    std::unique_ptr<IAudioSource> source_;
    std::thread worker_;
    LevelMeter meter_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool got_audio_ = false;
    bool ended_ = false;
    bool stop_requested_ = false;
    std::uint64_t lost_frames_ = 0;
    SourceEnd end_{};
};

}  // namespace sotto::audio
