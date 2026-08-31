#pragma once

#include "ports/streaming_vad.hpp"

namespace ambient::audio {

// Fallback when no VAD model is staged: everything is speech, so the
// endpointer degenerates to capped cuts
class PassthroughVad : public IStreamingVad {
   public:
    float SpeechProbability(std::span<const float>) override {
        return 1.0f;
    }

    void Reset() override {}
};

}  // namespace ambient::audio
