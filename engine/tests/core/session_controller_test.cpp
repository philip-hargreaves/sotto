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

    std::vector<store::RecoverableSession> ScanRecoverable() override {
        return {};
    }

    std::vector<std::string> Calls() {
        const std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
};

SourceFactory FactoryFor(ScriptedSource::Script script) {
    return [script] { return std::make_unique<ScriptedSource>(script); };
}

struct RecordingTranscriber : asr::ITranscriber {
    std::vector<std::pair<std::uint64_t, std::size_t>> windows;  // first_frame, count
    int begins = 0;
    int finishes = 0;

    void Begin(asr::ITurnSink&) override {
        ++begins;
    }

    void Submit(std::span<const float> frames, std::uint64_t first_frame) override {
        windows.push_back({first_frame, frames.size()});
    }

    void Finish() override {
        ++finishes;
    }
};

TEST(SessionController, EveryCapturedFrameReachesTheTranscriberByStop) {
    RecordingEvents events;
    FakeSessionStore store;
    RecordingTranscriber transcriber;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    ASSERT_EQ(store.turns.size(), 1u);
    ASSERT_EQ(events.turns.size(), 1u);
    EXPECT_EQ(store.turns[0].text, events.turns[0].text);
    EXPECT_EQ(store.turns[0].first_frame, 0u);
    EXPECT_EQ(store.turns[0].frame_count, Window().size());
}

TEST(SessionController, StartAcksOnlyAfterAudioFlowsAndLevelsFollow) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieImmediately), events, store,
                                 transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kNeverAudio), events, store,
                                 transcriber, kTestSettle);

    EXPECT_FALSE(controller.Start());

    EXPECT_NE(controller.LastEnd().detail.find("deadline"), std::string::npos);
    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}));
}

TEST(SessionController, StartFailsWhenTheStoreRefusesASession) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    store.refuse_begin = true;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "turn s1", "finalise s1"}))
        << "the tail turn lands before the session seals";
    EXPECT_FALSE(store.frames.empty()) << "captured audio must reach the store";
}

TEST(SessionController, CancelErasesTheSession) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieAfterAudio), events, store,
                                 transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kThrowAfterAudio), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Cancel();

    EXPECT_EQ(store.Calls(), (std::vector<std::string>{"begin s1", "cancel s1"}))
        << "the source completing is not the user's keep-or-discard decision";
}

TEST(SessionController, StartWhileRunningIsRefused) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.Start());
    controller.Stop();

    EXPECT_EQ(store.begins, 1) << "the refused start must not open a second session";
}

TEST(SessionController, RestartAfterAStopGetsAFreshSource) {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 store, transcriber, kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 store, transcriber, kTestSettle);

    controller.Stop();

    EXPECT_FALSE(controller.Running());
    EXPECT_TRUE(store.Calls().empty());
}

}  // namespace
}  // namespace sotto::audio
