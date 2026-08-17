#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "ports/audio_source.hpp"
#include "ports/streaming_vad.hpp"

namespace sotto::audio {

// Cuts the capture stream into windows at speech boundaries (spec 1.2, core
// profile). Every constant below is measured; the sweeps are in
// engine-constants.md 2 - do not retune
class Endpointer {
   public:
    // Hysteresis and padding, validated configuration
    static constexpr float kEnter = 0.40f;
    static constexpr float kExit = 0.25f;
    static constexpr std::size_t kEdgePadFrames = 800;        // 50 ms
    static constexpr std::size_t kMinUtteranceFrames = 8000;  // 0.5 s
    // Core-mode closing profile
    static constexpr std::size_t kPauseCloseFrames = 11200;  // 0.7 s
    static constexpr std::size_t kBreakCloseFrames = 32000;  // 2 s
    static constexpr std::size_t kPauseGateFrames = 320000;  // 20 s accumulated
    static constexpr std::size_t kSoftCapFrames = 448000;    // 28 s, Whisper's window
    static constexpr std::size_t kBacktrackFrames = 48000;   // quietest hop in last 3 s

    struct Window {
        std::vector<float> frames;
        std::uint64_t first_frame = 0;
    };

    explicit Endpointer(IStreamingVad& vad) : vad_(vad) {}

    std::vector<Window> Push(std::span<const float> frames) {
        std::vector<Window> windows;
        for (const float sample : frames) {
            hop_[hop_fill_++] = sample;
            if (hop_fill_ == kVadHopFrames) {
                hop_fill_ = 0;
                OnHop(windows);
            }
        }
        return windows;
    }

    // The tail at stop: whatever speech is open, regardless of length
    std::optional<Window> Flush() {
        if (!speech_seen_) {
            Reset();
            return std::nullopt;
        }
        buffer_.insert(buffer_.end(), hop_.begin(),
                       hop_.begin() + static_cast<std::ptrdiff_t>(hop_fill_));
        hop_fill_ = 0;
        Window window;
        window.first_frame = window_first_frame_;
        window.frames = std::move(buffer_);
        Reset();
        return window;
    }

   private:
    struct Hop {
        std::size_t end_offset;  // in buffer_ frames
        float probability;
    };

    void OnHop(std::vector<Window>& windows) {
        const float probability = vad_.SpeechProbability(hop_);

        // Hysteresis: between the thresholds the state holds
        if (probability >= kEnter) {
            in_speech_ = true;
        } else if (probability <= kExit) {
            in_speech_ = false;
        }

        if (!speech_seen_) {
            if (!in_speech_) {
                // Idle: keep only the pad that will lead the next window
                pad_.insert(pad_.end(), hop_.begin(), hop_.end());
                const std::size_t excess =
                    pad_.size() > kEdgePadFrames ? pad_.size() - kEdgePadFrames : 0;
                pad_.erase(pad_.begin(), pad_.begin() + static_cast<std::ptrdiff_t>(excess));
                idle_frames_ += kVadHopFrames;
                return;
            }
            // Speech onset: the window starts at the retained pad
            speech_seen_ = true;
            window_first_frame_ = session_frame_ + idle_frames_ - pad_.size();
            buffer_ = std::move(pad_);
            pad_.clear();
            session_frame_ += idle_frames_;
            idle_frames_ = 0;
        }

        buffer_.insert(buffer_.end(), hop_.begin(), hop_.end());
        hops_.push_back({buffer_.size(), probability});
        while (hops_.size() > kBacktrackFrames / kVadHopFrames + 1) hops_.pop_front();

        if (in_speech_) {
            last_speech_end_ = buffer_.size();
            silence_run_ = 0;
        } else {
            silence_run_ += kVadHopFrames;
        }

        const std::size_t cut_at = std::min(last_speech_end_ + kEdgePadFrames, buffer_.size());
        const bool break_close = silence_run_ >= kBreakCloseFrames;
        const bool pause_close = silence_run_ >= kPauseCloseFrames && cut_at >= kPauseGateFrames;

        if (break_close || pause_close) {
            // A blip shorter than the minimum utterance is dropped, not decoded;
            // the audio store keeps it either way
            if (cut_at >= kMinUtteranceFrames) {
                Window window;
                window.first_frame = window_first_frame_;
                window.frames.assign(buffer_.begin(),
                                     buffer_.begin() + static_cast<std::ptrdiff_t>(cut_at));
                windows.push_back(std::move(window));
            }
            session_frame_ = window_first_frame_ + buffer_.size();
            Reset();
            return;
        }

        if (buffer_.size() >= kSoftCapFrames) {
            windows.push_back(SoftCut());
        }
    }

    // At the cap, cut at the quietest hop of the last 3 s; the remainder
    // stays as the next window's start
    Window SoftCut() {
        const std::size_t earliest =
            std::max(kMinUtteranceFrames,
                     buffer_.size() > kBacktrackFrames ? buffer_.size() - kBacktrackFrames : 0);
        std::size_t cut = buffer_.size();
        float quietest = 2.0f;
        for (const Hop& hop : hops_) {
            const std::size_t start = hop.end_offset - kVadHopFrames;
            if (start < earliest || start >= buffer_.size()) continue;
            if (hop.probability < quietest) {
                quietest = hop.probability;
                cut = start;
            }
        }

        Window window;
        window.first_frame = window_first_frame_;
        window.frames.assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cut));

        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cut));
        window_first_frame_ += cut;
        hops_.clear();
        last_speech_end_ = last_speech_end_ > cut ? last_speech_end_ - cut : 0;
        return window;
    }

    void Reset() {
        buffer_.clear();
        pad_.clear();
        hops_.clear();
        speech_seen_ = false;
        silence_run_ = 0;
        last_speech_end_ = 0;
        idle_frames_ = 0;
    }

    IStreamingVad& vad_;
    std::vector<float> hop_ = std::vector<float>(kVadHopFrames);
    std::size_t hop_fill_ = 0;
    std::vector<float> buffer_;
    std::vector<float> pad_;
    std::deque<Hop> hops_;
    bool in_speech_ = false;
    bool speech_seen_ = false;
    std::size_t silence_run_ = 0;
    std::size_t last_speech_end_ = 0;
    std::uint64_t session_frame_ = 0;  // absolute frames consumed before this window
    std::uint64_t idle_frames_ = 0;    // absolute frames consumed while idle
    std::uint64_t window_first_frame_ = 0;
};

}  // namespace sotto::audio
