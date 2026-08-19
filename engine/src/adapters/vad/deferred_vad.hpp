#pragma once

#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "adapters/models/deferred_load.hpp"
#include "ports/streaming_vad.hpp"

namespace sotto::audio {

// VAD behind a background load; Reset (inside session/start) is the gate,
// so the engine serves while the model compiles
class DeferredVad : public IStreamingVad {
   public:
    explicit DeferredVad(std::function<std::unique_ptr<IStreamingVad>()> build)
        : inner_("vad", std::move(build)) {}

    float SpeechProbability(std::span<const float> hop) override {
        return inner_.Get().SpeechProbability(hop);
    }

    void Reset() override {
        inner_.Get().Reset();
    }

   private:
    models::DeferredLoad<IStreamingVad> inner_;
};

}  // namespace sotto::audio
