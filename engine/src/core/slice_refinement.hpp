#pragma once

#include <cstdint>
#include <vector>

#include "core/diar_regions.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kSplitEdgeMarginFrames =
    800;  // 50 ms: a cut this near an edge is the edge
inline constexpr std::uint64_t kMinSliceFrames = 3200;  // 0.2 s
inline constexpr std::uint64_t kMinCleanFrames = 8000;  // 0.5 s to prefer overlap-free audio
inline constexpr std::uint64_t kOverlapTurnMinFrames =
    6400;  // 0.4 s of overlap becomes its own turn

// Split each region at the seg change points strictly inside it - the
// zero-gap speaker handover Silero cannot see. Sub-slices under 0.2 s drop
inline std::vector<Region> RefineRegions(const std::vector<Region>& regions,
                                         const std::vector<std::uint64_t>& change_points) {
    std::vector<Region> slices;
    for (const Region& region : regions) {
        std::vector<std::uint64_t> bounds{region.first_frame};
        for (const std::uint64_t cut : change_points) {
            if (cut > region.first_frame + kSplitEdgeMarginFrames &&
                cut + kSplitEdgeMarginFrames < region.end_frame) {
                bounds.push_back(cut);
            }
        }
        bounds.push_back(region.end_frame);
        for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
            if (bounds[i + 1] - bounds[i] >= kMinSliceFrames) {
                slices.push_back({bounds[i], bounds[i + 1]});
            }
        }
    }
    return slices;
}

// Overlap frames excluded when at least 0.5 s of clean audio remains;
// empty means too short to embed
inline std::vector<Region> EmbeddingRanges(const Region& slice,
                                           const std::vector<Region>& overlap_spans) {
    std::vector<Region> clean;
    std::uint64_t cursor = slice.first_frame;
    std::uint64_t clean_total = 0;
    for (const Region& span : overlap_spans) {
        if (span.end_frame <= cursor || span.first_frame >= slice.end_frame) continue;
        if (span.first_frame > cursor) {
            clean.push_back({cursor, span.first_frame});
            clean_total += span.first_frame - cursor;
        }
        cursor = std::max(cursor, span.end_frame);
    }
    if (cursor < slice.end_frame) {
        clean.push_back({cursor, slice.end_frame});
        clean_total += slice.end_frame - cursor;
    }

    if (clean_total >= kMinCleanFrames) return clean;
    if (slice.end_frame - slice.first_frame >= kMinSliceFrames) return {slice};
    return {};
}

}  // namespace sotto::diar
