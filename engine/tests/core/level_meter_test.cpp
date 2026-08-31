#include "core/level_meter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace ambient::audio {
namespace {

// 440 Hz fits 44 whole cycles into one window, so the RMS is exact
std::vector<float> Sine(float amplitude, std::size_t count = LevelMeter::kWindowFrames) {
    std::vector<float> frames(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kSampleRate;
        frames[i] = amplitude * static_cast<float>(std::sin(phase));
    }
    return frames;
}

std::vector<float> Silence(std::size_t count = LevelMeter::kWindowFrames) {
    return std::vector<float>(count, 0.0F);
}

TEST(LevelMeter, SilenceReadsZero) {
    LevelMeter meter;

    const auto readings = meter.Push(Silence());

    ASSERT_EQ(readings.size(), 1u);
    EXPECT_EQ(readings[0].level, 0.0F);
    EXPECT_FALSE(readings[0].clipped);
}

TEST(LevelMeter, EmitsOneReadingPerWindow) {
    LevelMeter meter;

    EXPECT_EQ(meter.Push(Silence(2 * LevelMeter::kWindowFrames)).size(), 2u);
    EXPECT_EQ(meter.Push(Silence(LevelMeter::kWindowFrames / 2)).size(), 0u);
    EXPECT_EQ(meter.Push(Silence(LevelMeter::kWindowFrames / 2)).size(), 1u);
}

TEST(LevelMeter, FullScaleSineReadsFullScale) {
    LevelMeter meter;

    const auto readings = meter.Push(Sine(0.995F));

    ASSERT_EQ(readings.size(), 1u);
    EXPECT_NEAR(readings[0].level, 1.0F, 0.01F);
    EXPECT_FALSE(readings[0].clipped);
}

TEST(LevelMeter, MinusTwentyDbSineReadsTwoThirds) {
    LevelMeter meter;

    const auto readings = meter.Push(Sine(0.1F));

    ASSERT_EQ(readings.size(), 1u);
    EXPECT_NEAR(readings[0].level, 2.0F / 3.0F, 0.01F);
}

TEST(LevelMeter, AttackIsInstant) {
    LevelMeter meter;
    meter.Push(Silence());

    const auto readings = meter.Push(Sine(0.995F));

    ASSERT_EQ(readings.size(), 1u);
    EXPECT_NEAR(readings[0].level, 1.0F, 0.01F);
}

TEST(LevelMeter, ReleaseDecaysExponentially) {
    LevelMeter meter;
    const float loud = meter.Push(Sine(0.995F))[0].level;

    const auto first = meter.Push(Silence());
    const auto second = meter.Push(Silence());

    EXPECT_NEAR(first[0].level, loud * LevelMeter::kReleasePerWindow, 0.001F);
    EXPECT_NEAR(second[0].level,
                loud * LevelMeter::kReleasePerWindow * LevelMeter::kReleasePerWindow, 0.001F);
}

TEST(LevelMeter, ClipFlagsOnlyTheHotWindow) {
    LevelMeter meter;
    auto hot = Silence();
    hot[7] = 1.0F;
    hot[8] = -1.0F;

    const auto flagged = meter.Push(hot);
    const auto calm = meter.Push(Silence());

    // The flag is about the samples, not the level: a single spike clips
    EXPECT_TRUE(flagged[0].clipped);
    EXPECT_FALSE(calm[0].clipped);
}

TEST(LevelMeter, ChunkingDoesNotChangeTheReadings) {
    std::vector<float> audio;
    for (const float amplitude : {0.995F, 0.1F, 0.0F, 0.5F, 0.0F}) {
        const auto window = amplitude == 0.0F ? Silence() : Sine(amplitude);
        audio.insert(audio.end(), window.begin(), window.end());
    }

    LevelMeter whole;
    LevelMeter tiny;
    LevelMeter packet;
    const auto expected = whole.Push(audio);
    std::vector<LevelReading> from_tiny;
    std::vector<LevelReading> from_packets;
    for (std::size_t i = 0; i < audio.size(); i += 7) {
        const auto chunk = std::span(audio).subspan(i, std::min<std::size_t>(7, audio.size() - i));
        for (const auto& reading : tiny.Push(chunk)) {
            from_tiny.push_back(reading);
        }
    }
    for (std::size_t i = 0; i < audio.size(); i += 480) {
        const auto chunk =
            std::span(audio).subspan(i, std::min<std::size_t>(480, audio.size() - i));
        for (const auto& reading : packet.Push(chunk)) {
            from_packets.push_back(reading);
        }
    }

    ASSERT_EQ(expected.size(), 5u);
    ASSERT_EQ(from_tiny.size(), 5u);
    ASSERT_EQ(from_packets.size(), 5u);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        // Same samples in the same order: bit-identical, not merely close
        EXPECT_EQ(expected[i].level, from_tiny[i].level);
        EXPECT_EQ(expected[i].level, from_packets[i].level);
        EXPECT_EQ(expected[i].clipped, from_tiny[i].clipped);
        EXPECT_EQ(expected[i].clipped, from_packets[i].clipped);
    }
}

}  // namespace
}  // namespace ambient::audio
