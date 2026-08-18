#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

// What capture accumulates for finalise to splice in
struct CaptureDiarisation {
    std::vector<float> vad_probabilities;  // one per hop
    SegResult seg;                         // absolute frames
    std::uint64_t seg_done = 0;            // frames fully segmented
    // Slice span -> embedding; empty means too short to embed
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> embeddings;
    ResplitPieces resplit_pieces;
};

// Runs diarisation's causal stages during capture, so finalise only pays the
// unsettled tail, clustering and roles. All work stays behind the settled
// frontier, so finalise remains bit-identical to a whole-recording pass.
// Borrows the diariser's models; nothing else may drive them mid-capture
class DiarWorker {
   public:
    DiarWorker(audio::SileroVad& vad, Segmenter& segmenter, SpeakerEmbedder& embedder);

    // audio and reconciled turns so far; decode re-transcribes a clip
    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode);

    // The audio has ended: pad the final hop and segment the tail
    void Finish(std::span<const float> audio);

    bool Engaged() const {
        return !state_.vad_probabilities.empty();
    }

    // Resets the worker for the next session
    CaptureDiarisation Take() {
        return std::exchange(state_, {});
    }

   private:
    const std::vector<float>& EmbedSlice(std::span<const float> audio, const Region& slice);

    audio::SileroVad& vad_;
    Segmenter& segmenter_;
    SpeakerEmbedder& embedder_;
    CaptureDiarisation state_;
};

}  // namespace sotto::diar
