#pragma once

#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "adapters/models/deferred_load.hpp"
#include "ports/streaming_vad.hpp"

namespace sotto::audio {

// VAD behind a background load. Ready lets the capture path buffer hops
// instead of blocking; a failed load throws on the first probability, so
// the session fails loudly rather than staying silent
class DeferredVad : public IStreamingVad {
   public:
    explicit DeferredVad(std::function<std::unique_ptr<IStreamingVad>()> build)
        : inner_("vad", std::move(build)) {}

    float SpeechProbability(std::span<const float> hop) override {
        return inner_.Get().SpeechProbability(hop);
    }

    // A fresh model starts reset; only an already-loaded one needs it
    void Reset() override {
        if (inner_.Loaded()) {
            inner_.Get().Reset();
        }
    }

    bool Ready() const override {
        return inner_.Settled();
    }

   private:
    models::DeferredLoad<IStreamingVad> inner_;
};

}  // namespace sotto::audio
