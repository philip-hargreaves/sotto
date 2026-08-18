#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sotto::diar {

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

// Who-spoke-when over a whole recording: speech slices with anonymous
// cluster labels, plus each cluster's similarity to the accrued clinician
// anchor when one exists. turn_boundaries are the transcribed turns' edges,
// used as extra slice cuts (measured +0.41 pt attribution over 57
// consults). Naming is the caller's decision; AccrueDoctor folds the named
// doctor's voiceprint into the anchor afterwards
class IDiariser {
   public:
    virtual ~IDiariser() = default;

    virtual DiariseResult Diarise(std::span<const float> audio,
                                  std::span<const std::uint64_t> turn_boundaries = {}) = 0;

    virtual void AccrueDoctor(std::span<const float> audio,
                              const std::vector<LabelledSlice>& slices, int doctor_cluster) = 0;
};

}  // namespace sotto::diar
