#include "adapters/models/deferred_load.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "adapters/diarisation/deferred_diariser.hpp"
#include "adapters/vad/deferred_vad.hpp"

namespace sotto {
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

TEST(DeferredVad, ResetWaitsForTheModel) {
    std::atomic<int> resets{0};
    audio::DeferredVad vad([&resets] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return std::make_unique<CountingVad>(resets);
    });

    vad.Reset();
    EXPECT_EQ(resets.load(), 1) << "Reset returns only once the model is up";
}

struct CountingDiariser : diar::IDiariser {
    std::atomic<int>& discards;

    explicit CountingDiariser(std::atomic<int>& counter) : discards(counter) {}

    diar::DiariseResult Diarise(std::span<const float>, std::span<const std::uint64_t>) override {
        return {};
    }

    void AccrueDoctor(std::span<const float>, const std::vector<diar::LabelledSlice>&,
                      int) override {}

    void DiscardCapture() override {
        ++discards;
    }
};

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
}  // namespace sotto
