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

namespace ambient::diar {

// Decodes per speculation pass; small so a long pass cannot stall the
// causal stages that keep the settled frontier fresh
inline constexpr int kSpeculateBudget = 4;
inline constexpr int kEdgeEmbedBudget = 2;  // edge chunks embedded per capture tick

// What capture accumulates for finalise to consume
struct CaptureDiarisation {
    std::vector<float> vad_probabilities;  // one per hop
    SegResult seg;                         // absolute frames
    std::uint64_t seg_done = 0;            // frames fully segmented
    // Slice span -> embedding; empty means too short to embed
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> embeddings;
    TurnTexts turn_texts;    // the speculation cache, keyed on exact decode spans
    TurnChunks turn_chunks;  // the chunks behind it, same keys
    // Edge-chunk span -> embedding, computed during capture (AMBIENT_RESPLIT)
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> chunk_embeddings;
    std::vector<std::uint64_t> clip_cuts;  // AMBIENT_CLIP_CUTS: segment edges from clip decodes
};

// What the last Advance could say about the sealed transcript's opening
struct Speculation {
    std::vector<LabelledSlice> turns;  // settled, merged, text known
    std::vector<std::string> texts;
    std::vector<std::vector<float>> centroids;  // provisional clusters
    int cluster_count = 0;
};

// Capture-phase stages behind the settled frontier; finalise stays
// bit-identical and pays only the tail
class DiarWorker {
   public:
    DiarWorker(audio::SileroVad& vad, Segmenter& segmenter, SpeakerEmbedder& embedder);

    // audio and reconciled turns so far; decode re-transcribes a clip, at most
    // budget spans per call so a stop never waits long behind speculation
    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode, int budget = kSpeculateBudget);

    // The audio has ended: pad the final hop and segment the tail
    void Finish(std::span<const float> audio);

    bool Engaged() const {
        return !state_.vad_probabilities.empty();
    }

    CaptureDiarisation Take() {
        overlap_cache_.clear();
        speculation_ = {};
        return std::exchange(state_, {});
    }

    const Speculation& LastSpeculation() const {
        return speculation_;
    }

    void AddCutPoints(std::span<const std::uint64_t> cuts) {
        state_.clip_cuts.insert(state_.clip_cuts.end(), cuts.begin(), cuts.end());
    }

   private:
    const std::vector<float>& EmbedSlice(std::span<const float> audio, const Region& slice);

    // Provisional overlap-turn embeddings, recomputed across ticks otherwise
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> overlap_cache_;

    audio::SileroVad& vad_;
    Segmenter& segmenter_;
    SpeakerEmbedder& embedder_;
    CaptureDiarisation state_;
    Speculation speculation_;
};

}  // namespace ambient::diar
