#pragma once

#include <cstdint>
#include <vector>

namespace sotto::diar {

inline constexpr int kMaxSpeakers = 4;
inline constexpr std::uint64_t kFitMinFrames = 32000;  // 2 s: only long slices vote on the count

struct ClusterResult {
    std::vector<int> labels;  // one per slice
    int count = 1;
    std::vector<std::vector<float>> centroids;  // count x dims, unit norm; empty when count is 1
};

// Average-linkage clustering over cosine distance; count by silhouette
// sweep fitted on long slices, every slice labelled by nearest centroid
ClusterResult ClusterSpeakers(const std::vector<std::vector<float>>& embeddings,
                              const std::vector<std::uint64_t>& duration_frames);

}  // namespace sotto::diar
