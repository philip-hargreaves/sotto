#pragma once

#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/diar_worker.hpp"
#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "ports/diariser.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kOverlapTurnMinFrames =
    6400;  // 0.4 s of overlap becomes its own turn

// The assembled batch chain: VAD regions -> seg change points ->
// refined slices -> overlap-excluded embeddings -> clustering -> labels.
// Each long-enough overlap span is then emitted as a second turn on the
// best non-primary centroid, so a backchannel spoken over the dominant
// speaker is not absorbed. Owns its own VAD instance - Silero is stateful
// and the capture path has its own - and the clinician anchor
class SpeakerDiariser : public IDiariser {
   public:
    SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime,
                    const std::filesystem::path& anchor_root);

    DiariseResult Diarise(std::span<const float> audio,
                          std::span<const std::uint64_t> turn_boundaries = {}) override;

    void AccrueDoctor(std::span<const float> audio, const std::vector<LabelledSlice>& slices,
                      int doctor_cluster) override;

    // Capture-phase work; Diarise then finalises from the accumulated state
    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode) override {
        worker_.Advance(audio, turns, decode);
    }

    ResplitPieces TakeResplitPieces() override {
        return std::exchange(pieces_, {});
    }

    SpeakerEmbedder& Embedder() {
        return embedder_;
    }

   private:
    audio::SileroVad vad_;
    Segmenter segmenter_;
    SpeakerEmbedder embedder_;
    AnchorStore anchors_;
    DiarWorker worker_;
    ResplitPieces pieces_;
};

}  // namespace sotto::diar
