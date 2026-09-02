#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "core/endpointer.hpp"
#include "core/env_flag.hpp"
#include "core/level_meter.hpp"
#include "core/metrics.hpp"
#include "core/note_label.hpp"
#include "core/per_turn.hpp"
#include "core/resume_source.hpp"
#include "core/role_naming.hpp"
#include "core/turn_reconcile.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/note_writer.hpp"
#include "ports/session_store.hpp"
#include "ports/transcriber.hpp"

namespace ambient::audio {

// Session events, delivered on the capture thread
class ISessionEvents {
   public:
    virtual ~ISessionEvents() = default;

    virtual void OnLevel(const LevelReading& reading) = 0;

    virtual void OnTurn(const asr::Turn& turn) = 0;

    virtual void OnInterrupted(SourceEndReason reason, const std::string& detail) = 0;

    // Finalise stages as they start ("transcript", "speakers"), on the
    // stopping thread; fired only for work that actually runs
    virtual void OnProgress(const std::string&) {}

    // The note lane, delivered on its own thread after finalise
    virtual void OnNotePartial(const std::string&) {}
    virtual void OnNoteReady(const std::string&) {}
    virtual void OnNoteFailed(const std::string&) {}

    // Patient information follows the note on the same thread
    virtual void OnPatientPartial(const std::string&) {}
    virtual void OnPatientReady(const std::string&) {}
    virtual void OnPatientFailed(const std::string&) {}
};

// A replay request, carried into the source factory; absent means microphone
struct ReplaySpec {
    std::string path;
    double speed = 1.0;
    bool monitor = false;
    std::uint64_t start_frame = 0;  // resume: skip audio already captured
};

// Resolved by the caller: the id pins the endpoint (empty = default), the
// name goes into the session snapshot
struct MicSelection {
    std::string id;
    std::string name;
};

using SourceFactory = std::function<std::unique_ptr<IAudioSource>(const std::optional<ReplaySpec>&,
                                                                  const std::string& mic_id)>;

// One session at a time; every ending has a storage outcome: Stop
// finalises, Cancel erases, an interruption abandons recoverable
class SessionController {
   public:
    // Below this the model writes from its prompt, not the consultation
    // (measured on 14 s); refusing is the only safe output
    static constexpr std::size_t kMinNoteWords = 25;

    SessionController(SourceFactory factory, ISessionEvents& events, store::ISessionStore& store,
                      asr::ITranscriber& transcriber, IStreamingVad& vad,
                      std::chrono::milliseconds settle_timeout = std::chrono::seconds(3),
                      diar::IDiariser* diariser = nullptr,
                      std::uint64_t diar_advance_frames = 5 * kSampleRate,
                      note::INoteWriter* note_writer = nullptr,
                      metrics::Registry* metrics = nullptr,
                      std::size_t min_note_words = kMinNoteWords)
        : factory_(std::move(factory)),
          events_(events),
          store_(store),
          transcriber_(transcriber),
          vad_(vad),
          diariser_(diariser),
          note_writer_(note_writer),
          metrics_(metrics),
          diar_advance_frames_(diar_advance_frames),
          settle_timeout_(settle_timeout),
          min_note_words_(min_note_words) {}

    ~SessionController() {
        Stop();
        JoinNoteThread();
    }
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // True once audio flows. resume replays stored audio ahead of the live
    // source; retain false erases once the consultation is left
    bool Start(std::optional<ReplaySpec> replay = std::nullopt,
               const store::SessionId& resume_from = {}, bool retain = true,
               const MicSelection& mic = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (running_) {
                return false;
            }
            running_ = true;
            reviewing_ = false;  // Record wins over a review
            got_audio_ = false;
            ended_ = false;
            stop_requested_ = false;
            diar_stop_ = false;
            lost_frames_ = 0;
            end_ = {};
            meter_ = LevelMeter{};
        }
        std::vector<float> resumed_audio;
        try {
            if (!resume_from.empty()) {
                resumed_audio = store_.ReadAudio(resume_from);
                std::fprintf(stderr, "ambient-engine: resuming %s with %.1f s of stored audio\n",
                             resume_from.c_str(),
                             static_cast<double>(resumed_audio.size()) / kSampleRate);
            }
            store_.EraseUnretained();  // the previous consultation is left
            store::SessionMeta meta{kSampleRate, "", ""};
            if (!replay.has_value()) {
                // What was actually opened, so a default fallback is on record
                meta.device_id = mic.id;
                meta.device_name = mic.name;
            }
            meta.retain = retain;
            const store::SessionId id = store_.Begin(meta);
            std::lock_guard<std::mutex> lock(mutex_);
            session_id_ = id;
            resumed_from_ = resume_from;
            note_prepared_ = false;
            vad_.Reset();
            endpointer_.emplace(vad_);
            vad_backlog_.clear();
            session_audio_.clear();
            session_turns_.clear();
            transcriber_.Begin(turn_sink_);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: session start failed: %s\n", e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            end_ = {SourceEndReason::kFailed, std::string("session setup failed: ") + e.what()};
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!resumed_audio.empty()) {
                if (replay.has_value()) {
                    replay->start_frame += resumed_audio.size();
                }
                source_ = std::make_unique<ResumeSource>(std::move(resumed_audio),
                                                         factory_(replay, mic.id));
            } else {
                source_ = factory_(replay, mic.id);
            }
        }
        worker_ = std::thread([this] { GuardedRun(); });
        if (diariser_ != nullptr) {
            diar_thread_ = std::thread([this] { DiarLoop(); });
        }
        if (metrics_ != nullptr) {
            metrics_->BeginSession(replay.has_value(), replay.has_value() ? replay->speed : 0.0);
        }

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
        // Re-warm in parallel with finalise; a long session may have evicted
        if (note_writer_ != nullptr && Running()) {
            note_writer_->Prepare();
        }
        EndCapture();
        FinishSession(Outcome::kFinalise);
    }

    // Idempotent; the recording is erased (D5)
    void Cancel() {
        EndCapture();
        FinishSession(Outcome::kCancel);
    }

    // Hold the source's delivery; stop and cancel always win
    void SetPaused(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_ && running_) source_->SetPaused(paused);
    }

    void SetMonitor(bool monitor) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_ && running_) source_->SetMonitor(monitor);
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

    // Reopens a stored session as the regenerate target; refused while
    // recording or writing
    bool Open(const store::SessionId& id) {
        if (Running() || note_busy_.load()) {
            return false;
        }
        try {
            (void)store_.ReadTurns(id);
        } catch (...) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        last_finalised_ = id;
        reviewing_ = true;
        return true;
    }

    // Leaving the consultation: ends a review (regenerate refuses until the
    // next seal or open) and erases what was recorded with retain off
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reviewing_) {
                reviewing_ = false;
                last_finalised_.clear();
            }
        }
        if (!Running()) {
            try {
                store_.EraseUnretained();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
    }

    bool Reviewing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reviewing_;
    }

    // The recording session's id, so the shell can resume it after a crash
    store::SessionId CurrentSession() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_id_;
    }

    // Applied to the next note; the shell sets these ahead of the stop
    void SetNoteOptions(note::NoteOptions options) {
        std::lock_guard<std::mutex> lock(mutex_);
        note_options_ = std::move(options);
    }

    // Rewrites the last finalised session's note; false when busy - the RPC
    // thread never blocks on the lane
    bool RegenerateNote(note::NoteOptions options) {
        if (note_writer_ == nullptr || Running() || note_busy_.load()) {
            return false;
        }
        store::SessionId id = LastFinalised();
        if (id.empty()) {
            return false;
        }
        std::vector<asr::Turn> turns;
        try {
            turns = store_.ReadTurns(id);
        } catch (...) {
            return false;
        }
        SetNoteOptions(std::move(options));
        StartNoteLane(std::move(id), std::move(turns));
        return true;
    }

    // The sheet rewritten from the stored note - clinician edits included -
    // so an edited note can bring the sheet back into agreement
    bool RegeneratePatient() {
        if (note_writer_ == nullptr || !note_writer_->WritesPatient() || Running() ||
            note_busy_.load()) {
            return false;
        }
        store::SessionId id = LastFinalised();
        if (id.empty()) {
            return false;
        }
        std::string note;
        try {
            note = store_.ReadDocument(id, store::DocumentKind::kNote).text;
        } catch (...) {
            return false;
        }
        if (note.empty()) {
            return false;
        }
        JoinNoteThread();
        std::lock_guard<std::mutex> lock(mutex_);
        note_busy_ = true;
        note_thread_ = std::thread([this, id = std::move(id), note = std::move(note)] {
            struct BusyGuard {
                std::atomic<bool>& flag;
                ~BusyGuard() {
                    flag = false;
                }
            } busy_guard{note_busy_};
            if (note_abort_.load()) {
                return;
            }
            try {
                const std::string patient = note_writer_->WritePatient(
                    note,
                    [this](const std::string& partial) { events_.OnPatientPartial(partial); });
                SavePatient(id, patient);
                events_.OnPatientReady(patient);
            } catch (const std::exception& e) {
                events_.OnPatientFailed(e.what());
            } catch (...) {
                events_.OnPatientFailed("patient information failed");
            }
        });
        return true;
    }

    note::NoteOptions CurrentNoteOptions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return note_options_;
    }

    bool HasNoteWriter() const {
        return note_writer_ != nullptr;
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
                // Under the lock: the diarisation thread snapshots this
                if (controller.diariser_ != nullptr && !id.empty()) {
                    controller.session_audio_.insert(controller.session_audio_.end(),
                                                     frames.begin(), frames.end());
                }
            }
            controller.cv_.notify_all();
            if (!id.empty()) {
                controller.store_.Append(id, frames, lost_frames);
                // Hops buffer while the VAD loads; storage never waits
                if (!controller.vad_.Ready()) {
                    controller.vad_backlog_.insert(controller.vad_backlog_.end(), frames.begin(),
                                                   frames.end());
                } else {
                    controller.DrainVadBacklog();
                    // AMBIENT_NO_LIVE_ASR (windowless prototype, requires the seg-cuts
                    // and seg-frontier flags): the endpointer still runs, its windows
                    // are never decoded - per-turn clips are the only ASR
                    for (const auto& window : controller.endpointer_->Push(frames)) {
                        if (EnvFlag("AMBIENT_NO_LIVE_ASR")) continue;
                        controller.transcriber_.Submit(window.frames, window.first_frame,
                                                       window.first_new_frame);
                    }
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
            // Only an interruption decides its own outcome; a stopped source leaves
            // keep-or-discard to Stop or Cancel
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

    // Diarisation's causal work, spread over the recording; the heavy
    // Advance runs outside the lock, off the capture thread
    void DiarLoop() {
        // Accelerated replay delivers audio faster than real time; a wall
        // floor keeps the tick rate sane at any speed
        constexpr auto kMinTickGap = std::chrono::seconds(1);
        std::vector<float> audio;
        std::vector<asr::Turn> turns;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this, &audio] {
                return diar_stop_ || session_audio_.size() >= audio.size() + diar_advance_frames_;
            });
            if (diar_stop_) {
                return;
            }
            audio = session_audio_;
            turns = session_turns_;
            ++diar_ticks_;
            lock.unlock();
            // Deferred until whisper is decoding so the GPU never compiles
            // two models at once; still minutes ahead of any real stop
            if (!note_prepared_ && note_writer_ != nullptr) {
                note_prepared_ = true;
                note_writer_->Prepare();
            }
            // The same reconcile finalise runs, so turn spans agree
            diar::ReconcileTurns(turns);
            try {
                diariser_->Advance(
                    audio, turns,
                    [this](std::span<const float> clip, std::uint64_t first) -> std::string {
                        {
                            std::lock_guard<std::mutex> guard(mutex_);
                            // A stop must not wait behind a speculation pass
                            if (diar_stop_) return {};
                        }
                        return transcriber_.DecodeClip(clip, first);
                    });
                // AMBIENT_NOTE_PREFILL (windowless prototype): the note host
                // extends its KV over the settled opening between whisper decodes
                if (EnvFlag("AMBIENT_NOTE_PREFILL") && note_writer_ != nullptr) {
                    const auto guess = diariser_->SpeculativeTranscript();
                    if (!guess.empty()) note_writer_->Prefill(guess, CurrentNoteOptions());
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
            lock.lock();
            cv_.wait_for(lock, kMinTickGap, [this] { return diar_stop_; });
            if (diar_stop_) {
                return;
            }
        }
    }

    void JoinDiarThread() {
        std::thread diar;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            diar_stop_ = true;
            diar = std::move(diar_thread_);
        }
        cv_.notify_all();
        if (diar.joinable()) {
            diar.join();
        }
    }

    // Capture thread while running, finalise after it joins; never both
    void DrainVadBacklog() {
        if (vad_backlog_.empty()) {
            return;
        }
        std::vector<float> backlog = std::exchange(vad_backlog_, {});
        for (const auto& window : endpointer_->Push(backlog)) {
            if (EnvFlag("AMBIENT_NO_LIVE_ASR")) continue;
            transcriber_.Submit(window.frames, window.first_frame, window.first_new_frame);
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

    // Swallowed on purpose: key and file are gone before the catalog row, so
    // the erase holds even if bookkeeping fails
    void FinishSession(Outcome outcome) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_id_.empty()) {
                return;
            }
        }
        // No capture work may run once finalise starts; stage timings let a
        // slow finalise name its stage
        const auto finalise_start = std::chrono::steady_clock::now();
        const auto stage = [this, &finalise_start](const char* name) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - finalise_start)
                    .count();
            std::fprintf(stderr, "ambient-engine: finalise %s at %.1f s\n", name, seconds);
            if (metrics_ != nullptr) {
                metrics_->RecordStage(name, seconds);
            }
        };
        JoinDiarThread();
        stage("capture joined");
        std::fprintf(stderr, "ambient-engine: session audio %.1f s, %d capture ticks\n",
                     session_audio_.size() / 16000.0, diar_ticks_);
        if (metrics_ != nullptr) {
            metrics_->RecordSession(session_audio_.size() / 16000.0, lost_frames_, diar_ticks_);
        }
        // The tail window is transcribed unless discarding; every turn stores
        // before the outcome below
        if (outcome == Outcome::kFinalise) {
            events_.OnProgress("transcript");
        }
        if (outcome != Outcome::kCancel && endpointer_.has_value()) {
            DrainVadBacklog();  // a stop can land before the VAD does
            if (const auto tail = endpointer_->Flush()) {
                if (!EnvFlag("AMBIENT_NO_LIVE_ASR")) {
                    transcriber_.Submit(tail->frames, tail->first_frame, tail->first_new_frame);
                }
            }
        }
        transcriber_.Finish();
        stage("transcriber drained");

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
        // The note lane's input: attributed turns when diarisation succeeds,
        // the reconciled transcript otherwise
        std::vector<asr::Turn> note_input;
        if (outcome == Outcome::kFinalise && note_writer_ != nullptr) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                note_input = session_turns_;
            }
            diar::ReconcileTurns(note_input);
        }
        // The attributed transcript supersedes live turns; a diarisation failure
        // keeps the transcript, never loses the session
        if (outcome == Outcome::kFinalise && diariser_ != nullptr && !session_audio_.empty()) {
            try {
                std::vector<asr::Turn> transcribed;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    transcribed = session_turns_;
                }
                diar::ReconcileTurns(transcribed);
                // Transcribed-turn edges are extra slice cuts: they land on
                // real speech boundaries and measured +0.41 pt attribution.
                // AMBIENT_DIAR_SEG_CUTS_ONLY drops them (windowless ablation)
                std::vector<std::uint64_t> boundaries;
                if (!EnvFlag("AMBIENT_DIAR_SEG_CUTS_ONLY")) {
                    for (const auto& turn : transcribed) {
                        boundaries.push_back(turn.first_frame);
                        boundaries.push_back(turn.first_frame + turn.frame_count);
                    }
                }
                events_.OnProgress("speakers");
                const auto result = diariser_->Diarise(session_audio_, boundaries);
                stage("diarised");
                {
                    const auto& t = result.timing;
                    std::fprintf(stderr,
                                 "ambient-engine: diarise finish %.2f s, embed %.2f s (%d hits, %d "
                                 "misses), cluster %.2f s, overlap %.2f s\n",
                                 t.finish_s, t.embed_s, t.embed_hits, t.embed_misses, t.cluster_s,
                                 t.overlap_s);
                    if (metrics_ != nullptr) {
                        metrics_->RecordStage("diarise finish", t.finish_s);
                        metrics_->RecordStage("diarise embed", t.embed_s);
                        metrics_->RecordStage("diarise embed misses", t.embed_misses);
                        metrics_->RecordStage("diarise cluster", t.cluster_s);
                        metrics_->RecordStage("diarise overlap", t.overlap_s);
                    }
                }
                // The re-decode is transcript work again - and on the NPU it
                // is the longest stage, so the spinner must say so
                events_.OnProgress("turns");
                // Anchor voiceprints embed on the CPU alongside the GPU turn decode; the
                // future's destructor waits, so an exception cannot orphan the task
                auto voiceprint_seconds = 0.0;
                auto anchor_similarity =
                    std::async(std::launch::async, [this, &result, &voiceprint_seconds] {
                        const auto started = std::chrono::steady_clock::now();
                        auto similarity = diariser_->AnchorSimilarities(
                            session_audio_, result.slices, result.cluster_count);
                        voiceprint_seconds = std::chrono::duration<double>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count();
                        return similarity;
                    });
                // Each merged turn gets the text of its own audio; the
                // speculation cache means this mostly decodes only the tail
                const auto turns = diar::MergeByCluster(result.slices);
                const auto cache = diariser_->TakeTurnTexts();
                const auto turn_texts = diar::DecodeTurnTexts(
                    turns, session_audio_,
                    [this](std::span<const float> clip, std::uint64_t first) {
                        return transcriber_.DecodeClip(clip, first);
                    },
                    &cache);
                stage("turns decoded");
                const auto similarity = anchor_similarity.get();
                stage("voiceprints joined");
                if (metrics_ != nullptr) {
                    metrics_->RecordStage("diarise voiceprints", voiceprint_seconds);
                }
                {
                    std::size_t with_text = 0;
                    std::uint64_t longest = 0;
                    for (std::size_t i = 0; i < turns.size(); ++i) {
                        if (!turn_texts[i].empty()) ++with_text;
                        longest = std::max(longest, turns[i].end_frame - turns[i].first_frame);
                    }
                    std::fprintf(stderr,
                                 "ambient-engine: %zu turns, %zu with text, %zu cached, longest "
                                 "%.1f s, %d clusters\n",
                                 turns.size(), with_text, cache.size(), longest / 16000.0,
                                 result.cluster_count);
                    if (metrics_ != nullptr) {
                        metrics_->RecordTranscript(static_cast<int>(with_text),
                                                   result.cluster_count);
                    }
                }
                std::vector<diar::RoleTurn> role_turns;
                for (std::size_t i = 0; i < turns.size(); ++i) {
                    role_turns.push_back({turns[i].cluster,
                                          turns[i].end_frame - turns[i].first_frame,
                                          turn_texts[i]});
                }
                const auto roles = diar::NameRoles(role_turns, result.cluster_count, similarity);
                std::vector<asr::Turn> attributed;
                for (std::size_t i = 0; i < turns.size(); ++i) {
                    if (turn_texts[i].empty()) continue;
                    asr::Turn turn;
                    turn.first_frame = turns[i].first_frame;
                    turn.frame_count = turns[i].end_frame - turns[i].first_frame;
                    turn.speaker =
                        roles.role_of_cluster[static_cast<std::size_t>(turns[i].cluster)];
                    turn.text = turn_texts[i];
                    attributed.push_back(std::move(turn));
                }
                if (!attributed.empty()) {
                    store_.ReplaceTurns(id, attributed);
                    note_input = attributed;
                }
                stage("transcript sealed");
                // The anchor learns only from named sessions, never a guess
                if (roles.doctor_cluster >= 0) {
                    diariser_->AccrueDoctor(session_audio_, result.slices, roles.doctor_cluster);
                }
                stage("anchor accrued");
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
        // Capture state a finalise did not consume must not leak into the
        // next session (cancel, abandon, a diarisation failure)
        if (diariser_ != nullptr) {
            diariser_->DiscardCapture();
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
        stage("stored");
        // The resumed-from session is superseded: everything it held flowed
        // into this one before any outcome could be reached
        std::string resumed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resumed = std::exchange(resumed_from_, {});
        }
        if (!resumed.empty()) {
            try {
                store_.Delete(resumed);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
        if (outcome == Outcome::kFinalise && note_writer_ != nullptr) {
            stage("note lane started");
            StartNoteLane(id, std::move(note_input));
        }
    }

    static std::size_t TranscriptWords(const std::vector<asr::Turn>& turns) {
        std::size_t words = 0;
        for (const auto& turn : turns) {
            bool in_word = false;
            for (const char c : turn.text) {
                const bool space = c == ' ' || c == '\t' || c == '\n';
                if (!space && !in_word) ++words;
                in_word = !space;
            }
        }
        return words;
    }

    // A store refusal never costs the note: the text still reaches the shell
    void SaveNote(const store::SessionId& id, const std::string& text,
                  const note::NoteOptions& options) {
        try {
            store::Document document;
            document.text = text;
            document.style = options.style;
            document.detail = options.detail;
            store_.SaveDocument(id, store::DocumentKind::kNote, document);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    // Asked once the documents are done; a title failing the sanitiser is not
    // stored, and a typed label is never overwritten
    void SaveLabel(const store::SessionId& id, const std::string& note_text) {
        try {
            if (!store_.ReadDocument(id, store::DocumentKind::kLabel).edited_at.empty()) {
                return;
            }
            const std::string label = core::SanitiseLabel(note_writer_->WriteLabel(note_text));
            if (label.empty()) {
                return;
            }
            store::Document document;
            document.text = label;
            store_.SaveDocument(id, store::DocumentKind::kLabel, document);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    void SavePatient(const store::SessionId& id, const std::string& text) {
        try {
            store::Document document;
            document.text = text;
            store_.SaveDocument(id, store::DocumentKind::kPatient, document);
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    // The note writes after the seal on its own thread; a new stop cancels
    // a note still writing
    void StartNoteLane(store::SessionId id, std::vector<asr::Turn> transcript) {
        JoinNoteThread();
        // Too thin: the model would write from its prompt (measured); the engine
        // authors a plain statement instead
        auto options = CurrentNoteOptions();  // before the lock: same mutex
        if (TranscriptWords(transcript) < min_note_words_) {
            const std::string note =
                "The recording was too short or did not contain enough clinical "
                "information to generate an accurate note.";
            SaveNote(id, note, options);
            events_.OnNoteReady(note);
            if (note_writer_->WritesPatient()) {
                const std::string patient =
                    "The recording was too short or did not contain enough clinical "
                    "information to generate a patient information sheet.";
                SavePatient(id, patient);
                events_.OnPatientReady(patient);
            }
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        note_busy_ = true;
        note_thread_ = std::thread([this, id = std::move(id), turns = std::move(transcript),
                                    options = std::move(options)] {
            struct BusyGuard {
                std::atomic<bool>& flag;
                ~BusyGuard() {
                    flag = false;
                }
            } busy_guard{note_busy_};
            // A join racing this thread's start must win: the writer's
            // per-generation cancel reset would otherwise erase the cancel
            if (note_abort_.load()) {
                return;
            }
            // An in-process note model needs whisper off the GPU first
            // (measured: KV-cache corruption); a worker-process model does not
            if (note_writer_->WantsTranscriberReleased()) {
                transcriber_.Release();
            }
            std::string note;
            try {
                note = note_writer_->Write(turns, options, [this](const std::string& partial) {
                    events_.OnNotePartial(partial);
                });
                SaveNote(id, note, options);
                events_.OnNoteReady(note);
            } catch (const std::exception& e) {
                events_.OnNoteFailed(e.what());
                return;
            } catch (...) {
                events_.OnNoteFailed("note generation failed");
                return;
            }
            // Patient information follows from the finished note; a failure
            // here leaves the note intact
            if (note_writer_->WritesPatient() && !note.empty()) {
                try {
                    const std::string patient = note_writer_->WritePatient(
                        note,
                        [this](const std::string& partial) { events_.OnPatientPartial(partial); });
                    SavePatient(id, patient);
                    events_.OnPatientReady(patient);
                } catch (const std::exception& e) {
                    events_.OnPatientFailed(e.what());
                } catch (...) {
                    events_.OnPatientFailed("patient information failed");
                }
            }
            // The title comes last: everything the clinician waits for is done
            if (!note.empty()) {
                SaveLabel(id, note);
            }
        });
    }

    void JoinNoteThread() {
        std::thread note;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            note = std::move(note_thread_);
        }
        if (note.joinable()) {
            note_abort_ = true;
            if (note_writer_ != nullptr) note_writer_->Cancel();
            note.join();
            note_abort_ = false;
        }
    }

    SourceFactory factory_;
    ISessionEvents& events_;
    store::ISessionStore& store_;
    asr::ITranscriber& transcriber_;
    IStreamingVad& vad_;
    diar::IDiariser* diariser_;
    note::INoteWriter* note_writer_;
    metrics::Registry* metrics_;
    std::uint64_t diar_advance_frames_;
    std::chrono::milliseconds settle_timeout_;
    std::size_t min_note_words_;
    std::unique_ptr<IAudioSource> source_;
    std::thread worker_;
    std::thread diar_thread_;
    std::thread note_thread_;  // moved out under mutex_, joined outside it
    bool diar_stop_ = false;   // under mutex_
    int diar_ticks_ = 0;       // under mutex_; diagnostics
    LevelMeter meter_;
    std::optional<Endpointer> endpointer_;
    std::vector<float> vad_backlog_;  // capture thread, then finalise
    TurnSink turn_sink_{*this};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool got_audio_ = false;
    bool ended_ = false;
    bool stop_requested_ = false;
    std::uint64_t lost_frames_ = 0;
    store::SessionId session_id_;
    store::SessionId resumed_from_;
    note::NoteOptions note_options_;
    std::atomic<bool> note_busy_{false};
    std::atomic<bool> note_abort_{false};
    bool note_prepared_ = false;  // diar thread only
    store::SessionId last_finalised_;
    bool reviewing_ = false;  // last_finalised_ came from Open, not a seal
    // Appended under mutex_ (the diarisation thread snapshots it); finalise
    // reads it after every other thread has joined
    std::vector<float> session_audio_;
    std::vector<asr::Turn> session_turns_;  // under mutex_: turns arrive on the ASR thread
    SourceEnd end_{};
};

}  // namespace ambient::audio
