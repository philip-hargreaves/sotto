#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/turn_resplit.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

// What capture accumulates for finalise to splice in
struct CaptureDiarisation {
    std::vector<float> vad_probabilities;  // one per whole hop
    SegResult seg;                         // absolute frames
    std::uint64_t seg_done = 0;            // frames fully segmented
    // Slice span -> embedding; empty means too short to embed
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> embeddings;
    // Turn span -> re-decoded pieces; empty means the turn stands
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<asr::Turn>> resplit_pieces;
};

// Runs diarisation's causal stages during capture, so finalise only pays the
// unsettled tail, clustering and roles. All work stays behind the settled
// frontier, so finalise remains bit-identical to a whole-recording pass
class DiarWorker {
   public:
    DiarWorker(const models::ModelStore& store, models::OvRuntime& runtime);

    // audio and reconciled turns so far; decode re-transcribes a clip
    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode);

    // The worker is spent after this
    CaptureDiarisation Take() {
        return std::move(state_);
    }

   private:
    const std::vector<float>& EmbedSlice(std::span<const float> audio, const Region& slice);

    audio::SileroVad vad_;
    Segmenter segmenter_;
    SpeakerEmbedder embedder_;
    CaptureDiarisation state_;
};

}  // namespace sotto::diar
