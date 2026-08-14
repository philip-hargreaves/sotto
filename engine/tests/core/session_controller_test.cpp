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
    std::vector<SourceEndReason> interruptions;
    std::string last_detail;

    void OnLevel(const LevelReading& reading) override {
        const std::lock_guard<std::mutex> lock(mutex);
        levels.push_back(reading.level);
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

SourceFactory FactoryFor(ScriptedSource::Script script) {
    return [script] { return std::make_unique<ScriptedSource>(script); };
}

TEST(SessionController, StartAcksOnlyAfterAudioFlowsAndLevelsFollow) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 kTestSettle);

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
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieImmediately), events,
                                 kTestSettle);

    EXPECT_FALSE(controller.Start());

    EXPECT_EQ(controller.LastEnd().reason, SourceEndReason::kFailed);
    EXPECT_EQ(controller.LastEnd().detail, "would not open");
    EXPECT_TRUE(events.interruptions.empty());
}

TEST(SessionController, StartFailsWhenNoAudioArrivesBeforeTheDeadline) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kNeverAudio), events,
                                 kTestSettle);

    EXPECT_FALSE(controller.Start());

    EXPECT_NE(controller.LastEnd().detail.find("deadline"), std::string::npos);
}

TEST(SessionController, MidSessionDeathRaisesInterrupted) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kDieAfterAudio), events,
                                 kTestSettle);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(events.WaitForInterruption());
    controller.Stop();

    ASSERT_EQ(events.interruptions.size(), 1u);
    EXPECT_EQ(events.interruptions[0], SourceEndReason::kDeviceLost);
    EXPECT_EQ(events.last_detail, "unplugged");
}

TEST(SessionController, AThrowingSourceIsGuardedAndReported) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kThrowAfterAudio), events,
                                 kTestSettle);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(events.WaitForInterruption());
    controller.Stop();

    ASSERT_EQ(events.interruptions.size(), 1u);
    EXPECT_EQ(events.interruptions[0], SourceEndReason::kFailed);
    EXPECT_NE(events.last_detail.find("driver exploded"), std::string::npos);
}

TEST(SessionController, ACompletedReplayEndsQuietly) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();

    EXPECT_TRUE(events.interruptions.empty());
    EXPECT_EQ(controller.LostFrames(), 3u);
}

TEST(SessionController, StartWhileRunningIsRefused) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 kTestSettle);

    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.Start());
    controller.Stop();
}

TEST(SessionController, RestartAfterAStopGetsAFreshSource) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kCompleteAfterAudio), events,
                                 kTestSettle);

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    ASSERT_TRUE(controller.Start());
    controller.Stop();

    // Two runs, two windows, two level readings
    EXPECT_EQ(events.levels.size(), 2u);
}

TEST(SessionController, StopBeforeStartIsANoOp) {
    RecordingEvents events;
    SessionController controller(FactoryFor(ScriptedSource::Script::kStreamUntilStopped), events,
                                 kTestSettle);

    controller.Stop();

    EXPECT_FALSE(controller.Running());
}

}  // namespace
}  // namespace sotto::audio
