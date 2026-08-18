#pragma once

#include <span>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "ports/diariser.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kOverlapTurnMinFrames =
    6400;  // 0.4 s of overlap becomes its own turn

// The assembled batch chain (spec 2.2): VAD regions -> seg change points ->
// refined slices -> overlap-excluded embeddings -> clustering -> labels.
// Each long-enough overlap span is then emitted as a second turn on the
// best non-primary centroid, so a backchannel spoken over the dominant
// speaker is not absorbed. Owns its own VAD instance - Silero is stateful
// and the capture path has its own
class SpeakerDiariser : public IDiariser {
   public:
    SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime);

    std::vector<LabelledSlice> Diarise(std::span<const float> audio) override;

   private:
    audio::SileroVad vad_;
    Segmenter segmenter_;
    SpeakerEmbedder embedder_;
};

}  // namespace sotto::diar
