#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "adapters/models/deferred_load.hpp"
#include "ports/diariser.hpp"

namespace sotto::diar {

// Diarisation behind a background load. Callers already run off the capture
// thread (diar thread, finalise), so waiting there is safe; a session that
// ends before the load has nothing to discard.
class DeferredDiariser : public IDiariser {
   public:
    explicit DeferredDiariser(std::function<std::unique_ptr<IDiariser>()> build)
        : inner_("diarisation", std::move(build)) {}

    DiariseResult Diarise(std::span<const float> audio,
                          std::span<const std::uint64_t> turn_boundaries = {}) override {
        return inner_.Get().Diarise(audio, turn_boundaries);
    }

    void AccrueDoctor(std::span<const float> audio, const std::vector<LabelledSlice>& slices,
                      int doctor_cluster) override {
        inner_.Get().AccrueDoctor(audio, slices, doctor_cluster);
    }

    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode) override {
        inner_.Get().Advance(audio, turns, decode);
    }

    TurnTexts TakeTurnTexts() override {
        return inner_.Get().TakeTurnTexts();
    }

    void DiscardCapture() override {
        if (inner_.Loaded()) {
            inner_.Get().DiscardCapture();
        }
    }

   private:
    models::DeferredLoad<IDiariser> inner_;
};

}  // namespace sotto::diar
