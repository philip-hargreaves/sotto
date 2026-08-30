#include "core/session_controller.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/vad/passthrough_vad.hpp"

namespace sotto::audio {
namespace {

constexpr auto kTestSettle = std::chrono::milliseconds(200);

// One 100 ms window at an amplitude the meter reads as full scale
std::vector<float> Window() {
    return std::vector<float>(LevelMeter::kWindowFrames, 0.70710678F);
}

class ScriptedSource : public IAudioSource {
   public:
    enum class Script {
        kStreamUntilStopped,
        kDieImmediately,
        kDieAfterAudio,
        kNeverAudio,
        kThrowAfterAudio,
        kCompleteAfterAudio,
    };

    explicit ScriptedSource(Script script) : script_(script) {}

    void Run(IAudioSink& sink) override {
        const auto window = Window();
        switch (script_) {
            case Script::kDieImmediately:
                sink.OnEnd({SourceEndReason::kFailed, "would not open"});
                return;
            case Script::kNeverAudio:
                WaitForStop();
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
            case Script::kDieAfterAudio:
                sink.OnAudio(window, 0);
                sink.OnEnd({SourceEndReason::kDeviceLost, "unplugged"});
                return;
            case Script::kThrowAfterAudio:
                sink.OnAudio(window, 0);
                throw std::runtime_error("driver exploded");
            case Script::kCompleteAfterAudio:
                sink.OnAudio(window, 3);
                sink.OnEnd({SourceEndReason::kCompleted, ""});
                return;
            case Script::kStreamUntilStopped:
                while (!stop_.load()) {
                    sink.OnAudio(window, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
        }
    }

    void RequestStop() override {
        stop_.store(true);
    }

    void SetPaused(bool paused) override {
        ++pause_calls;
        last_paused = paused;
    }

    void SetMonitor(bool monitor) override {
        ++monitor_calls;
        last_monitor = monitor;
    }

    std::atomic<int> pause_calls{0};
    std::atomic<bool> last_paused{false};
    std::atomic<int> monitor_calls{0};
    std::atomic<bool> last_monitor{false};

   private:
    void WaitForStop() {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    Script script_;
    std::atomic<bool> stop_{false};
};

// Written on the capture thread; read after the controller has joined it
struct RecordingEvents : ISessionEvents {
    std::mutex mutex;
    std::vector<float> levels;
    std::vector<asr::Turn> turns;
    std::vector<SourceEndReason> interruptions;
    std::string last_detail;

    void OnLevel(const LevelReading& reading) override {
        const std::lock_guard<std::mutex> lock(mutex);
        levels.push_back(reading.level);
    }

    void OnTurn(const asr::Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }

    void OnInterrupted(SourceEndReason reason, const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        interruptions.push_back(reason);
        last_detail = detail;
    }

    void OnProgress(const std::string& stage) override {
        const std::lock_guard<std::mutex> lock(mutex);
        progress.push_back(stage);
    }

    std::vector<std::string> progress;

    void OnNotePartial(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_partials.push_back(text);
    }

    void OnNoteReady(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_ready = text;
        note_done = true;
    }

    void OnNoteFailed(const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_failed = detail;
        note_done = true;
    }

    void OnPatientPartial(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_partials.push_back(text);
    }

    void OnPatientReady(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_ready = text;
        patient_done = true;
    }

    void OnPatientFailed(const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_failed = detail;
        patient_done = true;
    }

    std::vector<std::string> note_partials;
    std::string note_ready;
    std::string note_failed;
    bool note_done = false;
    std::vector<std::string> patient_partials;
    std::string patient_ready;
    std::string patient_failed;
    bool patient_done = false;

    bool WaitForPatient() {
        for (int i = 0; i < 400; ++i) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (patient_done) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool WaitForNote() {
        for (int i = 0; i < 400; ++i) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (note_done) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    bool WaitForInterruption() {
        for (int i = 0; i < 400; ++i) {
            {
                const std::lock_guard<std::mutex> lock(mutex);
                if (!interruptions.empty()) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

// Records the call sequence; the tests assert which storage outcome each way
// of ending a session produced
struct FakeSessionStore : store::ISessionStore {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::vector<float> frames;
    std::vector<asr::Turn> turns;
    std::uint64_t lost = 0;
    bool refuse_begin = false;
    int begins = 0;

    store::SessionId Begin(const store::SessionMeta& meta) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_begin) {
            throw std::runtime_error("store is broken");
        }
        EXPECT_EQ(meta.sample_rate, kSampleRate);
        const auto id = "s" + std::to_string(++begins);
        calls.push_back("begin " + id);
        return id;
    }

    void Append(const store::SessionId&, std::span<const float> audio,
                std::uint64_t lost_frames) override {
        const std::lock_guard<std::mutex> lock(mutex);
        frames.insert(frames.end(), audio.begin(), audio.end());
        lost += lost_frames;
    }

    void AppendTurn(const store::SessionId& id, const asr::Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("turn " + id);
        turns.push_back(turn);
    }

    void ReplaceTurns(const store::SessionId& id, std::span<const asr::Turn> replacement) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("replace " + id);
        turns.assign(replacement.begin(), replacement.end());
    }

    void Finalise(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("finalise " + id);
    }

    void Cancel(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("cancel " + id);
    }

    void Abandon(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("abandon " + id);
    }

    bool refuse_read_audio = false;

    std::vector<store::RecoverableSession> ScanRecoverable() override {
        return {};
    }

    std::vector<store::SessionSummary> ListSessions() override {
        return {};
    }

    void SaveNote(const store::SessionId& id, const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("note " + id);
        note = text;
    }

    std::string ReadNote(const store::SessionId&) override {
        const std::lock_guard<std::mutex> lock(mutex);
        return note;
    }

    void SavePatient(const store::SessionId& id, const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("patient " + id);
        patient = text;
    }

    std::string ReadPatient(const store::SessionId&) override {
        const std::lock_guard<std::mutex> lock(mutex);
        return patient;
    }

    std::string patient;

    std::vector<asr::Turn> ReadTurns(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("readTurns " + id);
        return turns;
    }

    std::vector<float> ReadAudio(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_read_audio) {
            throw std::runtime_error("no session " + id);
        }
        calls.push_back("readAudio " + id);
        return stored_audio;
    }

    void Delete(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("delete " + id);
    }

    std::vector<float> stored_audio;

    std::vector<std::string> Calls() {
        const std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }

    std::string note;
};

struct FakeNoteWriter : note::INoteWriter {
    std::string result = "the clinical note";
    bool fail = false;
    bool patient = false;
    bool fail_patient = false;
    std::atomic<bool> block{false};
    std::atomic<bool> cancelled{false};
    std::atomic<int> prepares{0};
    std::mutex mutex;
    std::vector<std::vector<asr::Turn>> calls;
    note::NoteOptions last_options;
    std::string patient_input;

    void Prepare() override {
        ++prepares;
    }

    bool WritesPatient() const override {
        return patient;
    }

    std::string WritePatient(const std::string& note_text, const Progress& progress) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            patient_input = note_text;
        }
        progress("Your appointment");
        if (fail_patient) {
            throw std::runtime_error("patient generation failed");
        }
        return "the patient sheet";
    }

    std::string Write(const std::vector<asr::Turn>& transcript, const note::NoteOptions& options,
                      const Progress& progress) override {
        cancelled = false;  // per generation, like the real writer
        {
            const std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(transcript);
            last_options = options;
        }
        progress("The patient");
        progress("The patient presents");
        while (block.load() && !cancelled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fail) {
            throw std::runtime_error("generation failed");
        }
        return cancelled.load() ? "interrupted" : result;
    }

    void Cancel() override {
        cancelled = true;
    }
};

SourceFactory FactoryFor(ScriptedSource::Script script) {
    return [script](const std::optional<ReplaySpec>&) {
        return std::make_unique<ScriptedSource>(script);
    };
}

struct RecordingTranscriber : asr::ITranscriber {
    std::vector<std::pair<std::uint64_t, std::size_t>> windows;  // first_frame, count
    int begins = 0;
    int finishes = 0;
    std::atomic<int> releases{0};

    void Release() override {
        ++releases;
    }

    void Begin(asr::ITurnSink&) override {
        ++begins;
    }

    void Submit(std::span<const float> frames, std::uint64_t first_frame,
                std::uint64_t /*first_new_frame*/ = 0) override {
        windows.push_back({first_frame, frames.size()});
    }

    void Finish() override {
        ++finishes;
    }
};

bool WaitForFrames(FakeSessionStore& store, std::size_t n);

struct GatedVad : PassthroughVad {
    std::atomic<bool> ready{true};

    bool Ready() const override {
        return ready.load();
    }
};

TEST(SessionController, WindowsAreIdenticalWhenTheVadArrivesLate) {
    const auto run = [](bool vad_ready) {
        RecordingEvents events;
        FakeSessionStore store;
        RecordingTranscriber transcriber;
        GatedVad vad;
        vad.ready = vad_ready;
        SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio),
                                     events, store, transcriber, vad, kTestSettle);
        EXPECT_TRUE(controller.Start());
        controller.Stop();
        return transcriber.windows;
    };

    const auto ready = run(true);
    const auto late = run(false);  // drained whole at finalise

    ASSERT_FALSE(ready.empty());
    EXPECT_EQ(ready, late) << "a deferred VAD must not change window boundaries";
}

TEST(SessionController, HopsBufferedWhileTheVadLoadsDrainMidSession) {
    RecordingEvents events;
    FakeSessionStore store;
    RecordingTranscriber transcriber;
    GatedVad vad;
    vad.ready = false;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    WaitForFrames(store, 200);
    EXPECT_TRUE(transcriber.windows.empty()) << "nothing decodes before the VAD is up";

    vad.ready = true;
    WaitForFrames(store, 800);
    controller.Stop();

    std::size_t submitted = 0;
    for (const auto& [first_frame, count] : transcriber.windows) {
        EXPECT_EQ(first_frame, submitted) << "the backlog drains contiguously from frame zero";
        submitted += count;
    }
    EXPECT_EQ(submitted, store.frames.size());
}

TEST(SessionController, MetricsCarryTheSessionAndItsStages) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    metrics::Registry registry;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 nullptr, &registry);

    ASSERT_TRUE(controller.Start(ReplaySpec{"x.wav", 4.0, false}));
    controller.Stop();

    const auto s = registry.Take();
    EXPECT_TRUE(s.replay);
    EXPECT_EQ(s.replay_speed, 4.0);
    EXPECT_TRUE(s.stage_seconds.contains("transcriber drained"));
    EXPECT_TRUE(s.stage_seconds.contains("capture joined"));
}

TEST(SessionController, EveryCapturedFrameReachesTheTranscriberByStop) {
    RecordingEvents events;
    FakeSessionStore store;
    RecordingTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_EQ(transcriber.begins, 1);
    EXPECT_EQ(transcriber.finishes, 1);
    ASSERT_FALSE(transcriber.windows.empty());
    EXPECT_EQ(transcriber.windows.front().first, 0u);
    std::size_t submitted = 0;
    for (const auto& [first_frame, count] : transcriber.windows) {
        EXPECT_EQ(first_frame, submitted) << "windows must be contiguous";
        submitted += count;
    }
    EXPECT_EQ(submitted, store.frames.size()) << "the tail must be flushed at stop";
}

TEST(SessionController, TurnsReachTheStoreAndTheEvents) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_EQ(store.turns.size(), 1u);
    ASSERT_EQ(events.turns.size(), 1u);
    EXPECT_EQ(store.turns[0].text, events.turns[0].text);
    EXPECT_EQ(store.turns[0].first_frame, 0u);
    EXPECT_EQ(store.turns[0].frame_count, Window().size());
}

TEST(SessionController, TheNoteFollowsTheSeal) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    // The warm-up rides the diarisation thread's first tick; a source that
    // completes at exactly the tick threshold races the stop, so stream
    // until the warm-up has been observed
    for (int i = 0; i < 500 && writer.prepares.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_EQ(events.note_partials,
              (std::vector<std::string>{"The patient", "The patient presents"}));
    EXPECT_EQ(events.note_ready, "the clinical note");
    EXPECT_TRUE(events.note_failed.empty());
    EXPECT_EQ(store.note, "the clinical note");
    const auto calls = store.Calls();
    EXPECT_EQ(calls.back(), "note s1") << "the note is stored after the seal";
    ASSERT_EQ(writer.calls.size(), 1u);
    EXPECT_FALSE(writer.calls[0].empty()) << "the writer gets the transcript";
    EXPECT_GE(writer.prepares.load(), 1) << "the weights warm while the session records";
}

TEST(SessionController, TheNoteLaneFreesTheTranscriberFirst) {
    RecordingEvents events;
    FakeSessionStore store;
    RecordingTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_EQ(transcriber.releases.load(), 1) << "the GPU is freed before the note writes";
}

TEST(SessionController, PatientInformationFollowsTheNote) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    writer.patient = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForPatient());
    EXPECT_EQ(events.note_ready, "the clinical note");
    EXPECT_EQ(writer.patient_input, "the clinical note") << "the note is the patient input";
    EXPECT_EQ(events.patient_partials, (std::vector<std::string>{"Your appointment"}));
    EXPECT_EQ(events.patient_ready, "the patient sheet");
    EXPECT_EQ(store.patient, "the patient sheet");
}

TEST(SessionController, AFailedPatientLeavesTheNoteIntact) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    writer.patient = true;
    writer.fail_patient = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForPatient());
    EXPECT_EQ(events.patient_failed, "patient generation failed");
    EXPECT_TRUE(events.patient_ready.empty());
    EXPECT_EQ(events.note_ready, "the clinical note");
    EXPECT_EQ(store.note, "the clinical note");
    EXPECT_TRUE(store.patient.empty());
}

TEST(SessionController, ANoteOnlyWriterSkipsThePatientLane) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_TRUE(events.patient_partials.empty());
    EXPECT_TRUE(writer.patient_input.empty());
}

TEST(SessionController, AFailedNoteAnnouncesAndStoresNothing) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    writer.fail = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_EQ(events.note_failed, "generation failed");
    EXPECT_TRUE(events.note_ready.empty());
    EXPECT_TRUE(store.note.empty());
}

TEST(SessionController, DestructionCancelsANoteStillWriting) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    writer.block = true;
    {
        SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio),
                                     events, store, transcriber, vad, kTestSettle, nullptr,
                                     5 * kSampleRate, &writer, nullptr, 0);
        ASSERT_TRUE(controller.Start());
        controller.Stop();
        // Destruction must catch the write in flight, not before it starts
        for (int i = 0; i < 400; ++i) {
            {
                const std::lock_guard<std::mutex> lock(events.mutex);
                if (!events.note_partials.empty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    EXPECT_TRUE(writer.cancelled.load());
    EXPECT_EQ(events.note_ready, "interrupted") << "an interrupted write returns what it had";
}

TEST(SessionController, CancelWritesNoNote) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_TRUE(writer.calls.empty());
    EXPECT_TRUE(events.note_partials.empty());
}

TEST(SessionController, StartAcksOnlyAfterAudioFlowsAndLevelsFollow) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    EXPECT_TRUE(controller.Running());
    controller.Stop();

    EXPECT_FALSE(controller.Running());
    ASSERT_FALSE(events.levels.empty());
    EXPECT_NEAR(events.levels.front(), 1.0F, 0.01F);
    EXPECT_TRUE(events.interruptions.empty()) << "a user stop is not an interruption";
}

TEST(SessionController, StartFailsWhenTheSourceDiesFirst) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieImmediately), events, store,
                                 transcriber, vad, kTestSettle);

    EXPECT_FALSE(controller.Start());

    EXPECT_EQ(controller.LastEnd().reason, SourceEndReason::kFailed);
    EXPECT_EQ(controller.LastEnd().detail, "would not open");
    EXPECT_TRUE(events.interruptions.empty());
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}))
        << "a session that never produced audio leaves no trace";
}

TEST(SessionController, StartFailsWhenNoAudioArrivesBeforeTheDeadline) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kNeverAudio), events, store,
                                 transcriber, vad, kTestSettle);

    EXPECT_FALSE(controller.Start());

    EXPECT_NE(controller.LastEnd().detail.find("deadline"), std::string::npos);
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}));
}

TEST(SessionController, StartFailsWhenTheStoreRefusesASession) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    store.refuse_begin = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    EXPECT_FALSE(controller.Start());
    EXPECT_FALSE(controller.Running());
    EXPECT_EQ(controller.LastEnd().reason, SourceEndReason::kFailed);
    EXPECT_NE(controller.LastEnd().detail.find("store"), std::string::npos);

    // The refusal is not sticky: the next start works
    store.refuse_begin = false;
    ASSERT_TRUE(controller.Start());
    controller.Stop();
}

TEST(SessionController, StopFinalisesTheSession) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "turn s1", "finalise s1"}))
        << "the tail turn lands before the session seals";
    EXPECT_FALSE(store.frames.empty()) << "captured audio must reach the store";
}

struct FakeDiariser : diar::IDiariser {
    int calls = 0;
    int accruals = 0;
    int accrued_cluster = -1;
    std::size_t audio_frames = 0;
    int clusters = 1;
    std::vector<double> similarities;
    diar::DiariseTiming timing;

    std::size_t boundary_cuts = 0;
    std::vector<std::uint64_t> bounds;

    // Written on the diarisation thread; read after the controller joins it
    int advances = 0;
    std::size_t advanced_frames = 0;
    std::size_t advanced_turns = 0;
    int takes = 0;
    int discards = 0;
    bool speculate_first_turn = false;

    diar::DiariseResult Diarise(std::span<const float> audio,
                                std::span<const std::uint64_t> turn_boundaries) override {
        boundary_cuts = turn_boundaries.size();
        bounds.assign(turn_boundaries.begin(), turn_boundaries.end());
        ++calls;
        audio_frames = audio.size();
        diar::DiariseResult result;
        result.cluster_count = clusters;
        result.timing = timing;
        if (clusters == 1) {
            result.slices = {{0, audio.size(), 0}};
        } else {
            const auto half = audio.size() / 2;
            result.slices = {{0, half, 0}, {half, audio.size(), 1}};
        }
        return result;
    }

    int similarity_calls = 0;

    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<diar::LabelledSlice>&,
                                           int) override {
        ++similarity_calls;
        return similarities;
    }

    void AccrueDoctor(std::span<const float>, const std::vector<diar::LabelledSlice>&,
                      int doctor_cluster) override {
        ++accruals;
        accrued_cluster = doctor_cluster;
    }

    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const diar::DecodeClipFn&) override {
        ++advances;
        advanced_frames = audio.size();
        advanced_turns = turns.size();
    }

    // Pretends capture speculated the first cluster's turn (its span is the
    // first half of the audio Diarise saw)
    diar::TurnTexts TakeTurnTexts() override {
        ++takes;
        diar::TurnTexts cache;
        if (speculate_first_turn && audio_frames > 0) {
            cache[{0, audio_frames / 2}] = "speculated words";
        }
        return cache;
    }

    void DiscardCapture() override {
        ++discards;
    }
};

// Streams until the store holds enough audio that each of two merged turns
// clears the 0.3 s decode floor; wall-clock sleeps are too coarse on Windows
bool WaitForFrames(FakeSessionStore& store, std::size_t n) {
    for (int i = 0; i < 400; ++i) {
        {
            const std::lock_guard<std::mutex> lock(store.mutex);
            if (store.frames.size() >= n) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

TEST(SessionController, StopReplacesTurnsWithTheAttributedTranscript) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    EXPECT_EQ(diariser.calls, 1);
    EXPECT_FALSE(store.frames.empty());
    EXPECT_EQ(diariser.audio_frames, store.frames.size())
        << "the diariser hears exactly the captured audio";
    EXPECT_EQ(store.Calls(),
              (std::vector<std::string>{"begin s1", "turn s1", "replace s1", "finalise s1"}))
        << "the attributed transcript supersedes the live turns before the seal";
    ASSERT_EQ(store.turns.size(), 1u);
    EXPECT_EQ(store.turns[0].speaker, "speaker 1") << "one cluster cannot be named";
    EXPECT_EQ(diariser.accruals, 0) << "an abstained session must not teach the anchor";
}

TEST(SessionController, AnchorSimilaritiesNameTheRolesAndAccrue) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.similarities = {0.2, 0.8};
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    ASSERT_FALSE(store.turns.empty());
    for (const auto& turn : store.turns) {
        EXPECT_TRUE(turn.speaker == "doctor" || turn.speaker == "patient") << turn.speaker;
    }
    EXPECT_EQ(diariser.accruals, 1);
    EXPECT_EQ(diariser.accrued_cluster, 1) << "the nearer cluster to the anchor is the doctor";
    EXPECT_EQ(diariser.similarity_calls, 1) << "the anchor is consulted once, off the decode path";
    EXPECT_GT(diariser.boundary_cuts, 0u) << "transcribed-turn edges reach the diariser as cuts";
}

TEST(SessionController, FinaliseDecodesEachTurnFromItsOwnAudio) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.similarities = {0.2, 0.8};
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    ASSERT_EQ(store.turns.size(), 2u) << "one merged turn per cluster";
    const auto half = diariser.audio_frames / 2;
    EXPECT_EQ(store.turns[0].text, "re-decoded " + std::to_string(half) + " frames at 0");
    EXPECT_EQ(store.turns[1].first_frame, half);
    EXPECT_NE(store.turns[0].speaker, store.turns[1].speaker);
}

TEST(SessionController, DiarisationAdvancesDuringCapture) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    // One 100 ms window per tick, so a short session advances several times
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser,
                                 LevelMeter::kWindowFrames);

    ASSERT_TRUE(controller.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    controller.Stop();

    EXPECT_GT(diariser.advances, 0) << "capture-phase work ran during the recording";
    EXPECT_GT(diariser.advanced_frames, 0u);
    EXPECT_LE(diariser.advanced_frames, store.frames.size())
        << "Advance only ever sees captured audio";
    EXPECT_EQ(diariser.calls, 1);
}

TEST(SessionController, ASpeculatedTurnTextIsUsedWithoutRedecoding) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.similarities = {0.2, 0.8};
    diariser.speculate_first_turn = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    EXPECT_EQ(diariser.takes, 1);
    ASSERT_EQ(store.turns.size(), 2u);
    EXPECT_EQ(store.turns[0].text, "speculated words") << "the cache hit stands";
    EXPECT_NE(store.turns[1].text.find("re-decoded"), std::string::npos)
        << "the miss decodes fresh";
}

TEST(SessionController, PauseReachesTheSourceAndStopStillWins) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    ScriptedSource* source = nullptr;
    SourceFactory factory = [&source](const std::optional<ReplaySpec>&) {
        auto s = std::make_unique<ScriptedSource>(ScriptedSource::Script::kStreamUntilStopped);
        source = s.get();
        return s;
    };
    SessionController controller(std::move(factory), events, store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.SetPaused(true);
    controller.SetPaused(false);
    controller.Stop();

    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->pause_calls.load(), 2);
    EXPECT_FALSE(source->last_paused.load());
}

TEST(SessionController, MonitorToggleReachesTheSource) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    ScriptedSource* source = nullptr;
    SourceFactory factory = [&source](const std::optional<ReplaySpec>&) {
        auto s = std::make_unique<ScriptedSource>(ScriptedSource::Script::kStreamUntilStopped);
        source = s.get();
        return s;
    };
    SessionController controller(std::move(factory), events, store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.SetMonitor(true);
    controller.SetMonitor(false);
    controller.Stop();

    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->monitor_calls.load(), 2);
    EXPECT_FALSE(source->last_monitor.load());
}

TEST(SessionController, AReplaySpecReachesTheFactory) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    std::optional<ReplaySpec> seen;
    SourceFactory factory = [&seen](const std::optional<ReplaySpec>& replay) {
        seen = replay;
        return std::make_unique<ScriptedSource>(ScriptedSource::Script::kStreamUntilStopped);
    };
    SessionController controller(std::move(factory), events, store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start(ReplaySpec{"C:/tracks/elbow.wav", 4.0, true}));
    controller.Stop();

    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->path, "C:/tracks/elbow.wav");
    EXPECT_EQ(seen->speed, 4.0);
    EXPECT_TRUE(seen->monitor);
}

TEST(SessionController, CancelDiscardsCaptureState) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser,
                                 LevelMeter::kWindowFrames);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_GE(diariser.discards, 1) << "a cancelled session's capture state must not leak";
    EXPECT_EQ(diariser.takes, 0) << "nothing splices on cancel";
    EXPECT_EQ(diariser.calls, 0) << "nothing diarises on cancel";
}

TEST(SessionController, RedecodesNeverReachTheStore) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.clusters = 2;
    diariser.similarities = {0.2, 0.8};
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    const auto calls = store.Calls();
    const auto replace = std::find(calls.begin(), calls.end(), "replace s1");
    ASSERT_NE(replace, calls.end());
    for (auto it = calls.begin(); it != calls.end(); ++it) {
        if (*it == "turn s1") {
            EXPECT_LT(it - calls.begin(), replace - calls.begin())
                << "a re-decode appended to the store";
        }
    }
}

TEST(SessionController, AmbiguousLexicalEvidenceKeepsNumberedSpeakers) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;  // scripted text carries no role signal
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.clusters = 2;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    ASSERT_FALSE(store.turns.empty());
    for (const auto& turn : store.turns) {
        EXPECT_TRUE(turn.speaker == "speaker 1" || turn.speaker == "speaker 2") << turn.speaker;
    }
    EXPECT_EQ(diariser.accruals, 0);
}

TEST(SessionController, CancelNeverDiarises) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_EQ(diariser.calls, 0);
    EXPECT_TRUE(events.progress.empty()) << "a cancel finalises nothing to report";
}

TEST(SessionController, StopReportsEachFinaliseStageAsItStarts) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    EXPECT_EQ(events.progress, (std::vector<std::string>{"transcript", "speakers"}));
}

TEST(SessionController, StopWithoutADiariserReportsOnlyTheTranscriptStage) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_EQ(events.progress, (std::vector<std::string>{"transcript"}))
        << "a stage that never runs is never announced";
}

TEST(SessionController, DiariseTimingReachesTheMetrics) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    diariser.timing.embed_s = 0.25;
    diariser.timing.embed_misses = 3;
    metrics::Registry registry;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, &diariser, 5 * kSampleRate,
                                 nullptr, &registry);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitForFrames(store, 12800));
    controller.Stop();

    const auto s = registry.Take();
    EXPECT_EQ(s.stage_seconds.at("diarise embed"), 0.25);
    EXPECT_EQ(s.stage_seconds.at("diarise embed misses"), 3);
    EXPECT_TRUE(s.stage_seconds.contains("diarise voiceprints"))
        << "the overlapped voiceprint task is timed by the controller";
    EXPECT_TRUE(s.stage_seconds.contains("voiceprints joined"));
    EXPECT_TRUE(s.stage_seconds.contains("diarised"));
}

TEST(SessionController, CancelErasesTheSession) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_FALSE(controller.Running());
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}));
    EXPECT_TRUE(events.interruptions.empty()) << "a user cancel is not an interruption";
}

TEST(SessionController, MidSessionDeathRaisesInterruptedAndAbandons) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieAfterAudio), events, store,
                                 transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(events.WaitForInterruption());
    controller.Stop();

    ASSERT_EQ(events.interruptions.size(), 1u);
    EXPECT_EQ(events.interruptions[0], SourceEndReason::kDeviceLost);
    EXPECT_EQ(events.last_detail, "unplugged");
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "turn s1", "abandon s1"}))
        << "an interrupted recording is kept for recovery, not finalised";
}

TEST(SessionController, AThrowingSourceIsGuardedAndReported) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kThrowAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(events.WaitForInterruption());
    controller.Stop();

    ASSERT_EQ(events.interruptions.size(), 1u);
    EXPECT_EQ(events.interruptions[0], SourceEndReason::kFailed);
    EXPECT_NE(events.last_detail.find("driver exploded"), std::string::npos);
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "turn s1", "abandon s1"}));
}

TEST(SessionController, ACompletedReplayEndsQuietlyAndFinalises) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_TRUE(events.interruptions.empty());
    EXPECT_EQ(controller.LostFrames(), 3u);
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "turn s1", "finalise s1"}));
    EXPECT_EQ(store.frames.size(), Window().size());
    EXPECT_EQ(store.lost, 3u) << "loss accounting must reach the store";
}

TEST(SessionController, CancelAfterACompletedReplayStillErases) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}))
        << "the source completing is not the user's keep-or-discard decision";
}

TEST(SessionController, StartWhileRunningIsRefused) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.Start());
    controller.Stop();

    EXPECT_EQ(store.begins, 1) << "the refused start must not open a second session";
}

TEST(SessionController, RestartAfterAStopGetsAFreshSource) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    ASSERT_TRUE(controller.Start());
    controller.Stop();

    // Two runs, two windows, two level readings, two stored sessions
    EXPECT_EQ(events.levels.size(), 2u);
    EXPECT_EQ(store.begins, 2);
}

TEST(SessionController, StopBeforeStartIsANoOp) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle);

    controller.Stop();

    EXPECT_FALSE(controller.Running());
    EXPECT_TRUE(store.Calls().empty());
}

TEST(SessionController, NoteOptionsReachTheWriter) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    controller.SetNoteOptions({"soap", "concise"});
    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_EQ(writer.last_options.style, "soap");
    EXPECT_EQ(writer.last_options.detail, "concise");
}

TEST(SessionController, RegenerateRewritesTheLastNoteWithNewOptions) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    ASSERT_TRUE(events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(events.mutex);
        events.note_done = false;
    }

    ASSERT_TRUE(controller.RegenerateNote({"soap", "concise"}));
    ASSERT_TRUE(events.WaitForNote());

    ASSERT_EQ(writer.calls.size(), 2u);
    EXPECT_EQ(writer.last_options.style, "soap");
    EXPECT_EQ(writer.last_options.detail, "concise");
    EXPECT_EQ(store.note, "the clinical note") << "the regenerated note is re-saved";
}

TEST(SessionController, RegenerateRefusedWhileRecordingOrWithNothingFinalised) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeNoteWriter writer;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer, nullptr, 0);

    EXPECT_FALSE(controller.RegenerateNote({})) << "nothing finalised yet";

    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.RegenerateNote({})) << "a live session refuses";
    controller.Stop();
}

TEST(SessionController, AThinTranscriptWritesAPlainStatementInsteadOfFabricating) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;  // one 5-word turn, far below the floor
    PassthroughVad vad;
    FakeNoteWriter writer;
    writer.patient = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle, nullptr, 5 * kSampleRate,
                                 &writer);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_TRUE(events.WaitForNote());
    EXPECT_TRUE(writer.calls.empty()) << "the model must never see a transcript this thin";
    EXPECT_NE(events.note_ready.find("too short or did not contain enough clinical"),
              std::string::npos);
    EXPECT_NE(events.patient_ready.find("patient information sheet"), std::string::npos);
    EXPECT_TRUE(events.note_failed.empty()) << "a thin recording is not an error state";
    EXPECT_EQ(store.note, events.note_ready) << "the statement is the session's record";
}

TEST(SessionController, AResumedSessionReplaysStoredAudioThenSupersedesTheOld) {
    RecordingEvents events;
    FakeSessionStore store;
    store.stored_audio = std::vector<float>(LevelMeter::kWindowFrames * 3, 0.5F);
    RecordingTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    ASSERT_TRUE(controller.Start(std::nullopt, "old-session"));
    ASSERT_TRUE(WaitForFrames(store, LevelMeter::kWindowFrames * 3 + Window().size()));
    controller.Stop();

    const auto calls = store.Calls();
    EXPECT_NE(std::find(calls.begin(), calls.end(), "readAudio old-session"), calls.end());
    EXPECT_NE(std::find(calls.begin(), calls.end(), "delete old-session"), calls.end())
        << "the old session is superseded once the new one seals";
    // The new session's store holds the stored audio and the live audio as
    // one continuous stream
    EXPECT_EQ(store.frames.size(), LevelMeter::kWindowFrames * 3 + Window().size());
}

TEST(SessionController, AResumeOfAMissingSessionFailsTheStart) {
    RecordingEvents events;
    FakeSessionStore store;
    store.refuse_read_audio = true;
    RecordingTranscriber transcriber;
    PassthroughVad vad;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, vad, kTestSettle);

    EXPECT_FALSE(controller.Start(std::nullopt, "gone"));
    EXPECT_FALSE(controller.Running());
}

}  // namespace
}  // namespace sotto::audio
