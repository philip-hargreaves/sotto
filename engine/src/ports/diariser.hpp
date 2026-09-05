#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ports/transcriber.hpp"

namespace ambient::diar {

// (clip, absolute first frame) -> Whisper's chunks with absolute frames; the
// text is their join
using DecodeClipFn = std::function<std::vector<asr::Turn>(std::span<const float>, std::uint64_t)>;

inline std::string JoinedText(const std::vector<asr::Turn>& chunks) {
    std::string text;
    for (const auto& chunk : chunks) {
        if (chunk.text.empty()) continue;
        if (!text.empty()) text += ' ';
        text += chunk.text;
    }
    return text;
}

// Exact decode span -> speculated text
using TurnTexts = std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>;
// Exact decode span -> that decode's chunks
using TurnChunks = std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<asr::Turn>>;

struct LabelledSlice {
    std::uint64_t first_frame = 0;
    std::uint64_t end_frame = 0;
    int cluster = 0;
};

// Where a finalise-time Diarise spent its time; measurement only
struct DiariseTiming {
    double finish_s = 0;  // VAD tail + segmenting the un-segmented remainder
    double embed_s = 0;   // per-slice embeddings not served by the capture cache
    int embed_hits = 0;
    int embed_misses = 0;
    double cluster_s = 0;
    double overlap_s = 0;  // second-opinion embeds for overlap spans
};

struct DiariseResult {
    std::vector<LabelledSlice> slices;  // time-sorted
    int cluster_count = 0;
    DiariseTiming timing;
};

// Slices with anonymous labels plus anchor similarities; turn_boundaries
// measured +0.41 pt attribution. Naming is the caller's decision
class IDiariser {
   public:
    virtual ~IDiariser() = default;

    virtual DiariseResult Diarise(std::span<const float> audio,
                                  std::span<const std::uint64_t> turn_boundaries = {}) = 0;

    // Separate from Diarise so the voiceprint embeds can overlap other work;
    // kept for AccrueDoctor so the doctor's is embedded once
    virtual std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                                   const std::vector<LabelledSlice>& slices,
                                                   int cluster_count) = 0;

    virtual void AccrueDoctor(std::span<const float> audio,
                              const std::vector<LabelledSlice>& slices, int doctor_cluster) = 0;

    // Capture-phase work with the audio and reconciled turns so far;
    // optional - without it Diarise processes the whole recording
    virtual void Advance(std::span<const float>, std::span<const asr::Turn>, const DecodeClipFn&) {}

    // Finalise's catch-up: Advance without a budget, so every settled span is
    // decoded and cut before Diarise, however far capture lagged
    virtual void Settle(std::span<const float>, std::span<const asr::Turn>, const DecodeClipFn&) {}

    // Turn texts speculated by Advance, keyed on exact decode spans; valid
    // after Diarise
    virtual TurnTexts TakeTurnTexts() {
        return {};
    }

    // The chunks behind TakeTurnTexts, same keys; valid after Diarise
    virtual TurnChunks TakeTurnChunks() {
        return {};
    }

    // Cluster centroids (unit norm), valid after Diarise; the re-split judges
    // edge chunks against them
    virtual std::vector<std::vector<float>> ClusterCentroids() {
        return {};
    }

    // Embedding of [first, end) of the session audio, unit norm; empty when
    // too short. Served from the capture-phase cache where it has the span
    virtual std::vector<float> EmbedSpan(std::span<const float>, std::uint64_t, std::uint64_t) {
        return {};
    }

    // The sealed transcript's opening as far as capture can know it: settled
    // turns with cached texts and provisional roles. Valid after Advance, on
    // its thread; empty when nothing has settled
    virtual std::vector<asr::Turn> SpeculativeTranscript() {
        return {};
    }

    // Extra cut points (absolute frames) for the next Advance and Diarise;
    // AMBIENT_CLIP_CUTS feeds Whisper's chunk edges here
    virtual void AddCutPoints(std::span<const std::uint64_t>) {}

    // A voiceprint of the given speech (unit norm), empty when too short or
    // when no embedder is available; and the enrolment that replaces the anchor
    virtual std::vector<float> EmbedVoice(std::span<const float>) {
        return {};
    }
    virtual void ReplaceAnchor(std::span<const float>, std::uint64_t) {}

    // The doctor cluster's voiceprint, and the print learning from it; split so
    // the learning can wait until the note lane agrees this was a consultation
    virtual std::vector<float> DoctorVoiceprint(std::span<const float>,
                                                const std::vector<LabelledSlice>&, int) {
        return {};
    }
    virtual void AccrueVoiceprint(std::span<const float>) {}

    // Drop capture state a finalise will never consume (cancel, abandon)
    virtual void DiscardCapture() {}
};

}  // namespace ambient::diar
