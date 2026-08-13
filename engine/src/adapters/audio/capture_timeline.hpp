#pragma once

#include <cstdint>

namespace sotto::audio {

// Turns WASAPI packet metadata into the port's "frames lost"
// One instance per stream: positions restart from a new origin on any client rebuild.
class CaptureTimeline {
   public:
    explicit CaptureTimeline(std::uint32_t native_rate, std::uint32_t target_rate = 16000)
        : native_rate_(native_rate), target_rate_(target_rate) {}

    // Frames at the target rate lost before this packet. The first packet
    // anchors the origin and never reports loss: there is no baseline yet
    // and the discontinuity flag is undefined there.
    std::uint64_t OnPacket(std::uint64_t device_position, std::uint32_t frames,
                           bool discontinuity) {
        std::uint64_t lost = 0;
        if (!have_baseline_) {
            have_baseline_ = true;
            origin_ = device_position;
        } else {
            if (discontinuity) {
                ++discontinuity_flags_;
            }

            // Cumulative gap in native*target units, so both rates stay integral
            const std::int64_t native_delta =
                static_cast<std::int64_t>(device_position) - static_cast<std::int64_t>(origin_);
            const std::int64_t scaled =
                native_delta * static_cast<std::int64_t>(target_rate_) -
                static_cast<std::int64_t>(delivered_) * static_cast<std::int64_t>(native_rate_);

            // Round to the nearest target frame; sub-frame wobble from a
            // non-integral rate ratio must never read as loss
            const std::int64_t half = static_cast<std::int64_t>(native_rate_) / 2;
            const std::int64_t cumulative =
                (scaled + (scaled >= 0 ? half : -half)) / static_cast<std::int64_t>(native_rate_);
            if (cumulative > static_cast<std::int64_t>(total_lost_)) {
                lost = static_cast<std::uint64_t>(cumulative) - total_lost_;
                total_lost_ += lost;
            }
        }

        delivered_ += frames;
        return lost;
    }

    std::uint64_t TotalLost() const {
        return total_lost_;
    }

    std::uint64_t DiscontinuityFlags() const {
        return discontinuity_flags_;
    }

   private:
    std::uint32_t native_rate_;
    std::uint32_t target_rate_;
    bool have_baseline_ = false;
    std::uint64_t origin_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t total_lost_ = 0;
    std::uint64_t discontinuity_flags_ = 0;
};

}  // namespace sotto::audio
