#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "core/endpointer.hpp"
#include "core/level_meter.hpp"
#include "core/role_naming.hpp"
#include "core/speaker_attribution.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/session_store.hpp"
#include "ports/transcriber.hpp"

namespace sotto::audio {

// Session events, delivered on the capture thread
class ISessionEvents {
   public:
    virtual ~ISessionEvents() = default;

    virtual void OnLevel(const LevelReading& reading) = 0;

    virtual void OnTurn(const asr::Turn& turn) = 0;

    // A death mid-session
    virtual void OnInterrupted(SourceEndReason reason, const std::string& detail) = 0;
};

using SourceFactory = std::function<std::unique_ptr<IAudioSource>()>;

// One capture session at a time. Every way a session can end has a storage
// outcome: Stop finalises, Cancel erases, an interruption abandons (kept,
// recoverable), and a failed start erases. A source that completes on its
// own keeps the session open for the user's Stop or Cancel to decide.
class SessionController {
   public:
    SessionController(SourceFactory factory, ISessionEvents& events, store::ISessionStore& store,
                      asr::ITranscriber& transcriber, IStreamingVad& vad,
                      std::chrono::milliseconds settle_timeout = std::chrono::seconds(3),
                      diar::IDiariser* diariser = nullptr)
        : factory_(std::move(factory)),
          events_(events),
          store_(store),
          transcriber_(transcriber),
          vad_(vad),
          diariser_(diariser),
          settle_timeout_(settle_timeout) {}

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
        try {
            const store::SessionId id = store_.Begin({kSampleRate, "", ""});
            std::lock_guard<std::mutex> lock(mutex_);
            session_id_ = id;
            vad_.Reset();
            endpointer_.emplace(vad_);
            session_audio_.clear();
            session_turns_.clear();
            transcriber_.Begin(turn_sink_);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            end_ = {SourceEndReason::kFailed,
                    std::string("store refused the session: ") + e.what()};
            return false;
        }
        source_ = factory_();
        worker_ = std::thread([this] { GuardedRun(); });

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, settle_timeout_, [this] { return got_audio_ || ended_; });
        if (got_audio_) {
            return true;
        }
        lock.unlock();
        Cancel();  // a session that never produced audio leaves no trace

        std::lock_guard<std::mutex> relock(mutex_);
        if (end_.reason == SourceEndReason::kStopped) {
            end_ = {SourceEndReason::kFailed, "no audio arrived before the deadline"};
        }
        return false;
    }

    // Idempotent; a stop is the user's, so it never counts as an interruption.
    // The recording is kept
    void Stop() {
        EndCapture();
        FinishSession(Outcome::kFinalise);
    }

    // Idempotent; the recording is erased (D5)
    void Cancel() {
        EndCapture();
        FinishSession(Outcome::kCancel);
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

    // The most recently finalised session, for the shell's post-stop
    // transcript fetch; empty until a session has finalised
    store::SessionId LastFinalised() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_finalised_;
    }

   private:
    enum class Outcome { kFinalise, kCancel, kAbandon };

    // Turns may arrive on the transcriber's own thread; a turn after the
    // session closed is dropped, and one the store refuses is not announced
    struct TurnSink : asr::ITurnSink {
        SessionController& controller;

        explicit TurnSink(SessionController& owner) : controller(owner) {}

        void OnTurn(const asr::Turn& turn) override {
            store::SessionId id;
            {
                std::lock_guard<std::mutex> lock(controller.mutex_);
                id = controller.session_id_;
            }
            if (id.empty()) {
                return;
            }
            try {
                controller.store_.AppendTurn(id, turn);
                {
                    std::lock_guard<std::mutex> lock(controller.mutex_);
                    controller.session_turns_.push_back(turn);
                }
                controller.events_.OnTurn(turn);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
    };

    struct Sink : IAudioSink {
        SessionController& controller;

        explicit Sink(SessionController& owner) : controller(owner) {}

        void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) override {
            store::SessionId id;
            {
                std::lock_guard<std::mutex> lock(controller.mutex_);
                controller.lost_frames_ += lost_frames;
                controller.got_audio_ = true;
                id = controller.session_id_;
            }
            controller.cv_.notify_all();
            if (!id.empty()) {
                controller.store_.Append(id, frames, lost_frames);
                if (controller.diariser_ != nullptr) {
                    controller.session_audio_.insert(controller.session_audio_.end(),
                                                     frames.begin(), frames.end());
                }
                for (const auto& window : controller.endpointer_->Push(frames)) {
                    controller.transcriber_.Submit(window.frames, window.first_frame,
                                                   window.first_new_frame);
                }
            }
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
            // Only an interruption decides its own storage outcome: the audio
            // is gone and the recording must survive for recovery. A stopped
            // or completed source leaves keep-or-discard to Stop or Cancel;
            // audio ending is not the user's decision about the recording
            if (interrupted) {
                controller.FinishSession(Outcome::kAbandon);
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

    void EndCapture() {
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

    // A store failure here is swallowed on purpose: Cancel deletes the key
    // and file before the catalog row, so the erase holds even if bookkeeping
    // fails, and a failed finalise leaves the session recoverable
    void FinishSession(Outcome outcome) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_id_.empty()) {
                return;
            }
        }
        // The transcript completes before the session seals: the tail window
        // is transcribed unless the recording is being discarded, and every
        // turn is stored before the outcome below runs
        if (outcome != Outcome::kCancel && endpointer_.has_value()) {
            if (const auto tail = endpointer_->Flush()) {
                transcriber_.Submit(tail->frames, tail->first_frame, tail->first_new_frame);
            }
        }
        transcriber_.Finish();

        store::SessionId id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = std::exchange(session_id_, {});
            if (outcome == Outcome::kFinalise) {
                last_finalised_ = id;
            }
        }
        if (id.empty()) {
            return;
        }
        // The speaker-attributed transcript supersedes the live turns before
        // the seal; a diarisation failure keeps the unattributed transcript,
        // never the session
        if (outcome == Outcome::kFinalise && diariser_ != nullptr && !session_audio_.empty()) {
            try {
                const auto result = diariser_->Diarise(session_audio_);
                std::vector<asr::Turn> transcribed;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    transcribed = session_turns_;
                }
                const auto texts = diar::AssignSliceTexts(transcribed, result.slices);
                std::vector<diar::RoleTurn> role_turns;
                for (std::size_t i = 0; i < result.slices.size(); ++i) {
                    role_turns.push_back({result.slices[i].cluster,
                                          result.slices[i].end_frame - result.slices[i].first_frame,
                                          texts[i]});
                }
                const auto roles =
                    diar::NameRoles(role_turns, result.cluster_count, result.anchor_similarity);
                const auto attributed =
                    diar::BuildAttributedTurns(result.slices, texts, roles.role_of_cluster);
                if (!attributed.empty()) {
                    store_.ReplaceTurns(id, attributed);
                }
                // The anchor learns only from named sessions, never a guess
                if (roles.doctor_cluster >= 0) {
                    diariser_->AccrueDoctor(session_audio_, result.slices, roles.doctor_cluster);
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
        session_audio_.clear();
        session_audio_.shrink_to_fit();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_turns_.clear();
        }
        try {
            switch (outcome) {
                case Outcome::kFinalise:
                    store_.Finalise(id);
                    break;
                case Outcome::kCancel:
                    store_.Cancel(id);
                    break;
                case Outcome::kAbandon:
                    store_.Abandon(id);
                    break;
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    SourceFactory factory_;
    ISessionEvents& events_;
    store::ISessionStore& store_;
    asr::ITranscriber& transcriber_;
    IStreamingVad& vad_;
    diar::IDiariser* diariser_;
    std::chrono::milliseconds settle_timeout_;
    std::unique_ptr<IAudioSource> source_;
    std::thread worker_;
    LevelMeter meter_;
    std::optional<Endpointer> endpointer_;
    TurnSink turn_sink_{*this};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool got_audio_ = false;
    bool ended_ = false;
    bool stop_requested_ = false;
    std::uint64_t lost_frames_ = 0;
    store::SessionId session_id_;
    store::SessionId last_finalised_;
    // Capture-thread writes; finalise reads after the capture thread joins
    std::vector<float> session_audio_;
    std::vector<asr::Turn> session_turns_;  // under mutex_: turns arrive on the ASR thread
    SourceEnd end_{};
};

}  // namespace sotto::audio
