#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/env_flag.hpp"
#include "ports/diariser.hpp"

namespace ambient::diar {

// AMBIENT_RESPLIT: a merged turn can carry the other speaker's sentence at an
// edge when the segmenter's boundary fell short and no cut caught it. At finalise
// each turn's edge chunks are embedded and compared with the cluster centroids; a
// chunk nearer the other cluster by the margin becomes that speaker's turn. Only
// edge chunks move, inwards until one stays, so a turn loses a borrowed head or
// tail but is never shuffled. Chunks under kResplitMinFrames embed too poorly to move
inline constexpr std::uint64_t kResplitMinFrames = 9600;  // 0.6 s
inline constexpr double kResplitMargin = 0.20;             // cosine; calibrated by cross-validation

using EmbedSpanFn = std::function<std::vector<float>(std::uint64_t first, std::uint64_t end)>;

struct ResplitTurn {
    LabelledSlice slice;
    std::string text;
};

namespace detail {

inline double Dot(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) sum += a[i] * b[i];
    return sum;
}

// The other cluster the chunk's voice fits better by the margin, or -1
inline int BetterCluster(const std::vector<float>& e, int own,
                         const std::vector<std::vector<float>>& centroids, double margin,
                         double* own_score, double* best_score) {
    if (e.empty() || own < 0 || static_cast<std::size_t>(own) >= centroids.size()) return -1;
    *own_score = Dot(e, centroids[static_cast<std::size_t>(own)]);
    int best = -1;
    *best_score = -2.0;
    for (std::size_t c = 0; c < centroids.size(); ++c) {
        if (static_cast<int>(c) == own) continue;
        const double s = Dot(e, centroids[c]);
        if (s > *best_score) {
            *best_score = s;
            best = static_cast<int>(c);
        }
    }
    return best >= 0 && *best_score - *own_score >= margin ? best : -1;
}

}  // namespace detail

// dry_run: every edge chunk is scored and logged, nothing moves; the calibration
// study reads the log against the reference
inline std::vector<ResplitTurn> ResplitByEmbedding(const std::vector<LabelledSlice>& turns,
                                                   const std::vector<std::string>& texts,
                                                   const std::vector<std::vector<asr::Turn>>& chunks,
                                                   const EmbedSpanFn& embed,
                                                   const std::vector<std::vector<float>>& centroids,
                                                   double margin = kResplitMargin,
                                                   bool dry_run = false) {
    std::vector<ResplitTurn> out;
    const bool debug = EnvFlag("AMBIENT_CUT_DEBUG") || dry_run;
    if (dry_run) margin = 0.0;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const auto& turn = turns[i];
        const auto& parts = i < chunks.size() ? chunks[i] : std::vector<asr::Turn>{};
        if (centroids.size() < 2 || parts.size() < 2 || texts[i].empty()) {
            out.push_back({turn, texts[i]});
            continue;
        }
        // Which edge chunks move: from the front, then from the back
        std::vector<int> owner(parts.size(), turn.cluster);
        const auto judge = [&](std::size_t j) -> bool {
            const auto& p = parts[j];
            // A chunk's stamps can overrun the turn; only the turn's own audio is judged
            const std::uint64_t lo = std::max(p.first_frame, turn.first_frame);
            const std::uint64_t hi = std::min(p.first_frame + p.frame_count, turn.end_frame);
            if (hi <= lo || hi - lo < kResplitMinFrames) return false;
            double own = 0.0;
            double best = 0.0;
            const int other = detail::BetterCluster(embed(lo, hi), turn.cluster, centroids, margin,
                                                    &own, &best);
            if (debug) {
                std::fprintf(stderr,
                             "ambient-engine: resplit-candidate %.2f-%.2f s cluster %d own %.3f other "
                             "%.3f %s '%s'\n",
                             lo / 16000.0, hi / 16000.0, turn.cluster, own, best,
                             other >= 0 ? "moves" : "stays", p.text.c_str());
            }
            if (other < 0) return false;
            owner[j] = other;
            return true;
        };
        std::size_t front = 0;
        while (front + 1 < parts.size() && judge(front)) ++front;
        std::size_t back = parts.size();
        while (back > front + 1 && judge(back - 1)) --back;
        if (dry_run || (front == 0 && back == parts.size())) {
            out.push_back({turn, texts[i]});
            continue;
        }
        // Rebuild: moved head chunks, the remaining middle as the turn, moved tail chunks
        const auto piece = [&](std::size_t from, std::size_t to, int cluster) {
            ResplitTurn r;
            r.slice = {parts[from].first_frame, parts[to - 1].first_frame + parts[to - 1].frame_count,
                       cluster};
            for (std::size_t k = from; k < to; ++k) {
                if (parts[k].text.empty()) continue;
                if (!r.text.empty()) r.text += ' ';
                r.text += parts[k].text;
            }
            return r;
        };
        for (std::size_t j = 0; j < front; ++j) out.push_back(piece(j, j + 1, owner[j]));
        {
            auto middle = piece(front, back, turn.cluster);
            // The turn keeps its own span edges where nothing moved
            if (front == 0) middle.slice.first_frame = turn.first_frame;
            if (back == parts.size()) middle.slice.end_frame = turn.end_frame;
            out.push_back(std::move(middle));
        }
        for (std::size_t j = back; j < parts.size(); ++j) out.push_back(piece(j, j + 1, owner[j]));
    }
    return out;
}

}  // namespace ambient::diar
