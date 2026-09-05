#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/diar_regions.hpp"
#include "core/level_meter.hpp"
#include "ports/audio_source.hpp"
#include "ports/streaming_vad.hpp"

namespace ambient::audio {

struct EnrolProgress {
    double elapsed_s = 0.0;
    double speech_s = 0.0;
    LevelReading level;
};

struct EnrolCapture {
    std::vector<float> speech;  // the VAD-gated hops, concatenated
    double elapsed_s = 0.0;
    SourceEnd end;
};

// An enrolment with less clear speech than this is refused: the print would
// be a poor one and the clinician is better told to try again
inline constexpr double kEnrolMinSpeechSeconds = 20.0;

// The sink for an enrolment: meters the level, keeps the hops the VAD hears
// as speech, and asks the source to stop once the window has elapsed
class EnrolmentSink : public IAudioSink {
   public:
    EnrolmentSink(IStreamingVad& vad, std::uint64_t window_frames, std::function<void()> stop,
                  std::function<void(const EnrolProgress&)> progress)
        : vad_(vad),
          window_frames_(window_frames),
          stop_(std::move(stop)),
          progress_(std::move(progress)) {
        vad_.Reset();
    }

    void OnAudio(std::span<const float> frames, std::uint64_t) override {
        pending_.insert(pending_.end(), frames.begin(), frames.end());
        std::size_t consumed = 0;
        while (pending_.size() - consumed >= kVadHopFrames) {
            const std::span<const float> hop(pending_.data() + consumed, kVadHopFrames);
            // Until the VAD has loaded every hop counts; the level still shows
            if (!vad_.Ready() || vad_.SpeechProbability(hop) >= diar::kEnter) {
                capture_.speech.insert(capture_.speech.end(), hop.begin(), hop.end());
            }
            consumed += kVadHopFrames;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
        elapsed_frames_ += frames.size();
        capture_.elapsed_s = static_cast<double>(elapsed_frames_) / kSampleRate;
        for (const auto& reading : meter_.Push(frames)) {
            progress_({capture_.elapsed_s, SpeechSeconds(), reading});
        }
        if (elapsed_frames_ >= window_frames_ && !stopped_) {
            stopped_ = true;
            stop_();
        }
    }

    void OnEnd(const SourceEnd& end) override {
        capture_.end = end;
    }

    double SpeechSeconds() const {
        return static_cast<double>(capture_.speech.size()) / kSampleRate;
    }

    EnrolCapture Take() {
        return std::move(capture_);
    }

   private:
    IStreamingVad& vad_;
    std::uint64_t window_frames_;
    std::function<void()> stop_;
    std::function<void(const EnrolProgress&)> progress_;
    LevelMeter meter_;
    std::vector<float> pending_;
    std::uint64_t elapsed_frames_ = 0;
    bool stopped_ = false;
    EnrolCapture capture_;
};

// Why a capture cannot become a print, or empty when it can
inline std::string EnrolRejection(const EnrolCapture& capture, bool cancelled,
                                  double min_speech_s = kEnrolMinSpeechSeconds) {
    if (cancelled) return "cancelled";
    if (capture.end.reason == SourceEndReason::kFailed ||
        capture.end.reason == SourceEndReason::kDeviceLost) {
        return "microphone " +
               (capture.end.detail.empty() ? std::string("failed") : capture.end.detail);
    }
    const double speech = static_cast<double>(capture.speech.size()) / kSampleRate;
    if (speech < min_speech_s) {
        char text[96];
        std::snprintf(text, sizeof text, "not enough clear speech: %.0f s of %.0f s needed", speech,
                      min_speech_s);
        return text;
    }
    return {};
}

}  // namespace ambient::audio
