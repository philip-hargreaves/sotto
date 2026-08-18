#include "adapters/diarisation/speaker_clustering.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "fastcluster.h"

namespace sotto::diar {
namespace {

std::vector<std::vector<float>> Normalised(const std::vector<std::vector<float>>& embeddings) {
    auto out = embeddings;
    for (auto& embedding : out) {
        double norm = 0.0;
        for (const float x : embedding) norm += static_cast<double>(x) * x;
        norm = std::sqrt(norm) + 1e-9;
        for (float& x : embedding) x = static_cast<float>(x / norm);
    }
    return out;
}

// Mean cosine silhouette, sklearn semantics: a singleton scores 0
double Silhouette(const std::vector<std::vector<double>>& dist, const std::vector<int>& labels,
                  int k) {
    const std::size_t n = labels.size();
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const int own = labels[i];
        double same = 0.0;
        std::size_t same_n = 0;
        std::vector<double> other(static_cast<std::size_t>(k), 0.0);
        std::vector<std::size_t> other_n(static_cast<std::size_t>(k), 0);
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            if (labels[j] == own) {
                same += dist[i][j];
                ++same_n;
            } else {
                other[static_cast<std::size_t>(labels[j])] += dist[i][j];
                ++other_n[static_cast<std::size_t>(labels[j])];
            }
        }
        if (same_n == 0) continue;
        const double a = same / static_cast<double>(same_n);
        double b = 1e18;
        for (int c = 0; c < k; ++c) {
            if (c != own && other_n[static_cast<std::size_t>(c)] > 0) {
                b = std::min(b, other[static_cast<std::size_t>(c)] /
                                    static_cast<double>(other_n[static_cast<std::size_t>(c)]));
            }
        }
        total += (b - a) / std::max(a, b);
    }
    return total / static_cast<double>(n);
}

}  // namespace

ClusterResult ClusterSpeakers(const std::vector<std::vector<float>>& embeddings,
                              const std::vector<std::uint64_t>& duration_frames) {
    const std::size_t n = embeddings.size();
    ClusterResult result;
    result.labels.assign(n, 0);
    if (n < 2) return result;

    const auto normalised = Normalised(embeddings);
    const std::size_t dims = normalised[0].size();

    std::vector<std::size_t> fit;
    for (std::size_t i = 0; i < n; ++i) {
        if (duration_frames[i] >= kFitMinFrames) fit.push_back(i);
    }
    if (fit.empty()) {
        for (std::size_t i = 0; i < n; ++i) fit.push_back(i);
    }
    const std::size_t m = fit.size();
    if (m < 3) return result;

    std::vector<std::vector<double>> dist(m, std::vector<double>(m, 0.0));
    std::vector<double> condensed(m * (m - 1) / 2);  // scipy order, as hclust expects
    std::size_t next = 0;
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = i + 1; j < m; ++j) {
            double dot = 0.0;
            for (std::size_t d = 0; d < dims; ++d) {
                dot += static_cast<double>(normalised[fit[i]][d]) * normalised[fit[j]][d];
            }
            dist[i][j] = dist[j][i] = 1.0 - dot;
            condensed[next++] = 1.0 - dot;
        }
    }

    std::vector<int> merge(2 * (m - 1));
    std::vector<double> height(m - 1);
    hclust_fast(static_cast<int>(m), condensed.data(), HCLUST_METHOD_AVERAGE, merge.data(),
                height.data());

    int k = 1;
    std::vector<int> fit_labels(m, 0);
    double best = -2.0;
    const int khi = std::min(kMaxSpeakers, static_cast<int>(m) - 1);
    for (int kk = 2; kk <= khi; ++kk) {
        std::vector<int> cut(m);
        cutree_k(static_cast<int>(m), merge.data(), kk, cut.data());
        const double score = Silhouette(dist, cut, kk);
        if (score > best) {
            best = score;
            k = kk;
            fit_labels = cut;
        }
    }

    // Centroids from the fit set, renormalised - the held-out configuration
    std::vector<std::vector<double>> sums(static_cast<std::size_t>(k),
                                          std::vector<double>(dims, 0.0));
    std::vector<std::size_t> counts(static_cast<std::size_t>(k), 0);
    for (std::size_t t = 0; t < m; ++t) {
        const auto c = static_cast<std::size_t>(fit_labels[t]);
        ++counts[c];
        for (std::size_t d = 0; d < dims; ++d) sums[c][d] += normalised[fit[t]][d];
    }
    result.count = k;
    result.centroids.assign(static_cast<std::size_t>(k), std::vector<float>(dims, 0.0f));
    for (std::size_t c = 0; c < static_cast<std::size_t>(k); ++c) {
        double norm = 0.0;
        for (std::size_t d = 0; d < dims; ++d) {
            const double mean =
                sums[c][d] / static_cast<double>(std::max<std::size_t>(counts[c], 1));
            result.centroids[c][d] = static_cast<float>(mean);
            norm += mean * mean;
        }
        norm = std::sqrt(norm) + 1e-9;
        for (float& x : result.centroids[c]) x = static_cast<float>(x / norm);
    }

    // Every slice - short ones included - goes to its nearest centroid
    for (std::size_t i = 0; i < n; ++i) {
        double best_dot = -1e18;
        int best_c = 0;
        for (int c = 0; c < k; ++c) {
            double dot = 0.0;
            for (std::size_t d = 0; d < dims; ++d) {
                dot += static_cast<double>(normalised[i][d]) *
                       result.centroids[static_cast<std::size_t>(c)][d];
            }
            if (dot > best_dot) {
                best_dot = dot;
                best_c = c;
            }
        }
        result.labels[i] = best_c;
    }
    return result;
}

}  // namespace sotto::diar
