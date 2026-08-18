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

// Turn span -> its re-decoded pieces; empty means the turn stands
using ResplitPieces = std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<asr::Turn>>;

// Exact decode span -> speculated text
using TurnTexts = std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>;

struct LabelledSlice {
    std::uint64_t first_frame = 0;
    std::uint64_t end_frame = 0;
    int cluster = 0;
};

struct DiariseResult {
    std::vector<LabelledSlice> slices;  // time-sorted
    int cluster_count = 0;
    std::vector<double> anchor_similarity;  // one per cluster; empty: no anchor yet
};

// Who-spoke-when: speech slices with anonymous cluster labels, plus each
// cluster's similarity to the accrued clinician anchor. turn_boundaries are
// extra slice cuts (measured +0.41 pt attribution over 57 consults). Naming
// is the caller's decision; AccrueDoctor then folds the named doctor's
// voiceprint into the anchor
class IDiariser {
   public:
    virtual ~IDiariser() = default;

    virtual DiariseResult Diarise(std::span<const float> audio,
                                  std::span<const std::uint64_t> turn_boundaries = {}) = 0;

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
