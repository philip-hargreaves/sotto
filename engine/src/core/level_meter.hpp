#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ports/audio_source.hpp"

namespace sotto::audio {

struct LevelReading {
    float level = 0.0F;  // 0..1 over a dBFS scale from the floor up
    bool clipped = false;
};

// Mic-activity level from the samples alone: one reading per 100 ms window,
// RMS on a dBFS scale, instant attack, exponential release. No wall clock,
// so identical audio gives identical readings at any replay speed.
class LevelMeter {
   public:
    static constexpr std::size_t kWindowFrames = static_cast<std::size_t>(kSampleRate) / 10;
    static constexpr float kFloorDb = -60.0F;
    static constexpr float kClipLevel = 0.999F;
    static constexpr float kReleasePerWindow = 0.71653F;  // exp(-0.1 s / 0.3 s)

    std::vector<LevelReading> Push(std::span<const float> frames) {
        std::vector<LevelReading> readings;
        for (const float sample : frames) {
            sum_squares_ += static_cast<double>(sample) * sample;
            if (sample >= kClipLevel || sample <= -kClipLevel) {
                clipped_ = true;
            }
            if (++filled_ == kWindowFrames) {
                readings.push_back(CloseWindow());
            }
        }
        return readings;
    }

   private:
    LevelReading CloseWindow() {
        const double rms = std::sqrt(sum_squares_ / kWindowFrames);
        envelope_ = std::max(Normalise(rms), envelope_ * kReleasePerWindow);
        const LevelReading reading{envelope_, clipped_};
        sum_squares_ = 0.0;
        filled_ = 0;
        clipped_ = false;
        return reading;
    }

    // AES17 dBFS: a full-scale sine reads 0 dB, hence the sqrt 2
    static float Normalise(double rms) {
        if (rms <= 0.0) {
            return 0.0F;
        }
        const double dbfs = 20.0 * std::log10(rms * std::numbers::sqrt2);
        return static_cast<float>(
            std::clamp((dbfs - kFloorDb) / -static_cast<double>(kFloorDb), 0.0, 1.0));
    }

    double sum_squares_ = 0.0;
    std::size_t filled_ = 0;
    bool clipped_ = false;
    float envelope_ = 0.0F;
};

}  // namespace sotto::audio
