#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ports/transcriber.hpp"

namespace sotto::diar {

// (clip, absolute first frame) -> transcribed text
using DecodeClipFn = std::function<std::string(std::span<const float>, std::uint64_t)>;

// Exact decode span -> speculated text
using TurnTexts = std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>;

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

    // Turn texts speculated by Advance, keyed on exact decode spans; valid
    // after Diarise
    virtual TurnTexts TakeTurnTexts() {
        return {};
    }

    // Drop capture state a finalise will never consume (cancel, abandon)
    virtual void DiscardCapture() {}
};

}  // namespace sotto::diar
