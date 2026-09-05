#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/note/worker_note_writer.hpp"

namespace ambient::note {
namespace {

using Phase = NoteModelState::Phase;

// A store with a note model per tier; the fake host loads none of them
struct TieredStore {
    std::filesystem::path root;

    TieredStore() {
        root = std::filesystem::temp_directory_path() /
               ("ambient-lane-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        Stage("qwen3.5-9b-int4", "Qwen3.5 9B", "default");
        Stage("qwen3.6-35b-a3b-int4", "Qwen3.6 35B", "accuracy");
        Stage("qwen-broken-int4", "Broken", "constrained");
    }

    ~TieredStore() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void Stage(const std::string& id, const std::string& name, const std::string& tier) const {
        std::filesystem::create_directories(root / id);
        std::ofstream(root / id / "manifest.json")
            << R"({"manifestVersion": 1, "id": ")" << id << R"(", "name": ")" << name
            << R"(", "task": "note", "tier": ")" << tier
            << R"(", "licence": "Apache-2.0", "runtime": {"device": "GPU"},)"
            << R"( "files": {"model.xml": "00"}})";
    }
};

// Every transition the lane announced, waitable
struct Transitions {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<NoteModelState> seen;

    INoteLane::Listener Listener() {
        return [this](const NoteModelState& state) {
            std::lock_guard<std::mutex> lock(mutex);
            seen.push_back(state);
            changed.notify_all();
        };
    }

    // The first state at or after `from` in the given phase, within 5 s
    bool WaitFor(Phase phase, std::size_t from = 0) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] {
            for (std::size_t i = from; i < seen.size(); ++i) {
                if (seen[i].phase == phase) return true;
            }
            return false;
        });
    }

    std::vector<NoteModelState> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }
};

TEST(NoteLane, StartsIdleOnTheDefaultTierWithItsModelNamed) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);

    const auto state = lane.State();
    EXPECT_EQ(state.phase, Phase::kIdle);
    EXPECT_EQ(state.tier, "default");
    EXPECT_EQ(state.id, "qwen3.5-9b-int4");
    EXPECT_EQ(state.name, "Qwen3.5 9B");
}

TEST(NoteLane, ConfiguringAnotherTierLoadsItAtOnceAndReportsReady) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);
    Transitions seen;
    lane.SetListener(seen.Listener());

    const auto reply = lane.Configure("accuracy");

    EXPECT_EQ(reply.tier, "accuracy");
    EXPECT_EQ(reply.id, "qwen3.6-35b-a3b-int4");
    EXPECT_EQ(reply.phase, Phase::kLoading) << "warm on switch: the load starts in the call";
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));
    const auto states = seen.Snapshot();
    EXPECT_EQ(states.front().phase, Phase::kIdle) << "the switch is announced before the load";
    EXPECT_EQ(states.back().tier, "accuracy");
    EXPECT_EQ(states.back().name, "Qwen3.6 35B");
    EXPECT_GT(states.back().seconds, 0.0);
}

TEST(NoteLane, TheSameTierAgainIsANoOp) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);
    Transitions seen;
    lane.SetListener(seen.Listener());
    lane.Configure("accuracy");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));
    const auto before = seen.Snapshot().size();

    const auto reply = lane.Configure("accuracy");

    EXPECT_EQ(reply.phase, Phase::kReady);
    EXPECT_EQ(seen.Snapshot().size(), before) << "no transition, no respawn";
}

TEST(NoteLane, AFailedLoadIsReportedWithItsReason) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);
    Transitions seen;
    lane.SetListener(seen.Listener());

    lane.Configure("constrained");

    ASSERT_TRUE(seen.WaitFor(Phase::kFailed));
    const auto failed = seen.Snapshot().back();
    EXPECT_EQ(failed.tier, "constrained");
    EXPECT_EQ(failed.detail, "no such device");
    // The revert: the previous tier loads again
    const auto from = seen.Snapshot().size();
    lane.Configure("default");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady, from));
    EXPECT_EQ(seen.Snapshot().back().id, "qwen3.5-9b-int4");
}

TEST(NoteLane, AnUnstagedTierIsRefusedNamingWhatIsStaged) {
    TieredStore staged;
    std::filesystem::remove_all(staged.root / "qwen3.6-35b-a3b-int4");
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);

    try {
        lane.Configure("accuracy");
        FAIL() << "nothing claims note/accuracy";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("qwen3.5-9b-int4"), std::string::npos);
    }
    EXPECT_EQ(lane.State().tier, "default") << "a refused switch changes nothing";
}

TEST(NoteLane, TheHostServesTheConfiguredTier) {
    TieredStore staged;
    const models::ModelStore store(staged.root);
    WorkerNoteWriter lane(AMBIENT_FAKE_NOTE_HOST, staged.root, staged.root, &store);
    Transitions seen;
    lane.SetListener(seen.Listener());
    lane.Configure("accuracy");
    ASSERT_TRUE(seen.WaitFor(Phase::kReady));

    const std::string note = lane.Write({{0, 16000, "doctor", "hello"}}, {}, nullptr);

    EXPECT_EQ(note, "A note from qwen3.6-35b-a3b-int4");
    EXPECT_EQ(lane.State().phase, Phase::kReady);
}

}  // namespace
}  // namespace ambient::note
