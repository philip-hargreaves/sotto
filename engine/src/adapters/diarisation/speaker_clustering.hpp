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

// Average-linkage agglomerative clustering over cosine distance. The count
// is a silhouette sweep over k in 2..kMaxSpeakers fitted on long slices
// only; centroids are built from the fit and every slice - short ones
// included - is labelled by its nearest centroid. Fewer than three long
// slices cannot support a count and collapse to one cluster
ClusterResult ClusterSpeakers(const std::vector<std::vector<float>>& embeddings,
                              const std::vector<std::uint64_t>& duration_frames);

}  // namespace sotto::diar
