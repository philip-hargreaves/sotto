#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ambient::audio {

// The pipeline's one format: 16 kHz mono float32
inline constexpr int kSampleRate = 16000;

enum class SourceEndReason {
    kCompleted,   // The source ran out of audio
    kStopped,     // RequestStop was honoured
    kDeviceLost,  // The device vanished mid-capture
    kFailed,      // The source broke; detail says how
};

struct SourceEnd {
    SourceEndReason reason;
    std::string detail;
};

// Receives the stream on the source's thread.
class IAudioSink {
   public:
    virtual ~IAudioSink() = default;

    // lost_frames counts audio that belonged before this packet but never
    // arrived and never will; a complete recording is all zeros
    virtual void OnAudio(std::span<const float> frames, std::uint64_t lost_frames) = 0;

    // Always the last call, whatever the reason
    virtual void OnEnd(const SourceEnd& end) = 0;
};

// One capture stream. Run blocks until the stream ends; RequestStop may be
// called from any thread and ends the stream as kStopped.
class IAudioSource {
   public:
    virtual ~IAudioSource() = default;

    virtual void Run(IAudioSink& sink) = 0;
    virtual void RequestStop() = 0;

    // Hold delivery without ending the stream; optional, any thread
    virtual void SetPaused(bool) {}

    // Toggle audible monitoring mid-stream; optional, any thread
    virtual void SetMonitor(bool) {}
};

}  // namespace ambient::audio
