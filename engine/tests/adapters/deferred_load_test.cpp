#include "adapters/models/deferred_load.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/vad/deferred_vad.hpp"

namespace ambient {
namespace {

TEST(DeferredLoad, GetWaitsForTheBuild) {
    std::atomic<bool> built{false};
    models::DeferredLoad<int> deferred("test", [&built] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        built = true;
        return std::make_unique<int>(7);
    });

    EXPECT_EQ(deferred.Get(), 7);
    EXPECT_TRUE(built.load());
    EXPECT_TRUE(deferred.Loaded());
}

TEST(DeferredLoad, AFailedBuildRethrowsAtGet) {
    models::DeferredLoad<int> deferred(
        "test", []() -> std::unique_ptr<int> { throw std::runtime_error("no model"); });

    EXPECT_THROW(deferred.Get(), std::runtime_error);
    EXPECT_FALSE(deferred.Loaded());
}

struct CountingVad : audio::IStreamingVad {
    std::atomic<int>& resets;

    explicit CountingVad(std::atomic<int>& counter) : resets(counter) {}

    float SpeechProbability(std::span<const float>) override {
        return 1.0f;
    }

    void Reset() override {
        ++resets;
    }
};

TEST(DeferredVad, NotReadyWhileLoadingAndResetNeverBlocks) {
    std::atomic<int> resets{0};
    audio::DeferredVad vad([&resets] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return std::make_unique<CountingVad>(resets);
    });

    EXPECT_FALSE(vad.Ready());
    vad.Reset();
    EXPECT_EQ(resets.load(), 0) << "a fresh model starts reset";

    (void)vad.SpeechProbability({});  // waits for the load
    EXPECT_TRUE(vad.Ready());
    vad.Reset();
    EXPECT_EQ(resets.load(), 1);
}

struct CountingDiariser : diar::IDiariser {
    std::atomic<int>& discards;

    explicit CountingDiariser(std::atomic<int>& counter) : discards(counter) {}

    diar::DiariseResult Diarise(std::span<const float>, std::span<const std::uint64_t>) override {
        return {};
    }

    void AccrueDoctor(std::span<const float>, const std::vector<diar::LabelledSlice>&,
                      int) override {}

    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<diar::LabelledSlice>&,
                                           int cluster_count) override {
        return std::vector<double>(static_cast<std::size_t>(cluster_count), 0.75);
    }

    void DiscardCapture() override {
        ++discards;
    }

    std::vector<asr::Turn> SpeculativeTranscript() override {
        return {{0, 16000, "doctor", "guessed"}};
    }
};

// The engine only ever sees the wrapper; a method it does not forward is a
// method the product does not have (this one was missed once)
TEST(DeferredDiariser, ForwardsAnchorSimilarities) {
    std::atomic<int> discards{0};
    diar::DeferredDiariser diariser(
        [&discards] { return std::make_unique<CountingDiariser>(discards); });

    const auto similarity = diariser.AnchorSimilarities({}, {}, 2);

    EXPECT_EQ(similarity, (std::vector<double>{0.75, 0.75}));
}

TEST(DeferredDiariser, ForwardsTheSpeculativeTranscript) {
    std::atomic<int> discards{0};
    diar::DeferredDiariser diariser(
        [&discards] { return std::make_unique<CountingDiariser>(discards); });

    const auto guess = diariser.SpeculativeTranscript();

    ASSERT_EQ(guess.size(), 1u);
    EXPECT_EQ(guess[0].speaker, "doctor");
}

TEST(DeferredDiariser, DiscardBeforeTheLoadIsANoOp) {
    std::atomic<int> discards{0};
    std::atomic<bool> release{false};
    diar::DeferredDiariser diariser([&discards, &release] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::make_unique<CountingDiariser>(discards);
    });

    diariser.DiscardCapture();
    EXPECT_EQ(discards.load(), 0) << "nothing accumulated, nothing to wait for";

    release = true;
    (void)diariser.Diarise({}, {});
    diariser.DiscardCapture();
    EXPECT_EQ(discards.load(), 1);
}

}  // namespace
}  // namespace ambient
