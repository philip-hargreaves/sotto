#pragma once

#include <cstddef>
#include <span>

namespace ambient::audio {

// 32 ms at 16 kHz, Silero's native hop
inline constexpr std::size_t kVadHopFrames = 512;

// Streaming voice activity: one probability per hop, recurrent state across
// hops within a session
class IStreamingVad {
   public:
    virtual ~IStreamingVad() = default;

    // hop must be exactly kVadHopFrames
    virtual float SpeechProbability(std::span<const float> hop) = 0;

    virtual void Reset() = 0;

    // False while a background load is pending; callers buffer, never block
    virtual bool Ready() const {
        return true;
    }
};

}  // namespace ambient::audio
