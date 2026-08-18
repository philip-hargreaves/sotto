#include "adapters/diarisation/fbank.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace sotto::diar {
namespace {

std::vector<float> Tone(std::size_t samples) {
    std::vector<float> audio(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 16000.0f);
    }
    return audio;
}

TEST(Fbank, FrameCountFollowsSnipEdges) {
    // snip_edges: frames = 1 + (samples - 400) / 160
    const auto features = EmbedderFbank(Tone(16000));
    EXPECT_EQ(features.frames, 1u + (16000u - 400u) / 160u);
    EXPECT_EQ(features.values.size(), features.frames * kMelBins);
}

TEST(Fbank, InputShorterThanOneFrameYieldsNothing) {
    const auto features = EmbedderFbank(Tone(399));
    EXPECT_EQ(features.frames, 0u);
    EXPECT_TRUE(features.values.empty());
}

TEST(Fbank, EveryMelBinIsMeanNormalised) {
    const auto features = EmbedderFbank(Tone(8000));
    ASSERT_GT(features.frames, 0u);
    for (std::size_t bin = 0; bin < kMelBins; ++bin) {
        double mean = 0.0;
        for (std::size_t i = 0; i < features.frames; ++i) {
            mean += features.values[i * kMelBins + bin];
        }
        mean /= static_cast<double>(features.frames);
        EXPECT_NEAR(mean, 0.0, 1e-4) << "bin " << bin;
    }
}

}  // namespace
}  // namespace sotto::diar
