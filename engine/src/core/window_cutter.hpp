#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// Cuts the capture stream into large background-transcription windows.
// The size says core configuration, not live streaming; VAD-aligned
// boundaries replace this policy later.
class WindowCutter {
   public:
    static constexpr std::size_t kWindowFrames = 30 * static_cast<std::size_t>(kSampleRate);

    struct Window {
        std::vector<float> frames;
        std::uint64_t first_frame = 0;
    };

    explicit WindowCutter(std::size_t window_frames = kWindowFrames)
        : window_frames_(window_frames) {}

    std::vector<Window> Push(std::span<const float> frames) {
        pending_.insert(pending_.end(), frames.begin(), frames.end());
        std::vector<Window> windows;
        while (pending_.size() >= window_frames_) {
            Window window;
            window.first_frame = next_first_frame_;
            window.frames.assign(pending_.begin(),
                                 pending_.begin() + static_cast<std::ptrdiff_t>(window_frames_));
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(window_frames_));
            next_first_frame_ += window_frames_;
            windows.push_back(std::move(window));
        }
        return windows;
    }

    // The tail, shorter than a window, at end of session
    std::optional<Window> Flush() {
        if (pending_.empty()) {
            return std::nullopt;
        }
        Window window;
        window.first_frame = next_first_frame_;
        window.frames = std::move(pending_);
        pending_.clear();
        next_first_frame_ += window.frames.size();
        return window;
    }

   private:
    std::size_t window_frames_;
    std::vector<float> pending_;
    std::uint64_t next_first_frame_ = 0;
};

}  // namespace sotto::audio
