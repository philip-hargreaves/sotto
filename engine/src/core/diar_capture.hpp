#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/diar_regions.hpp"
#include "core/turn_resplit.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

// Cosine distance: same-speaker neighbours measure 0.03-0.22, different
// speakers 0.43-0.79, so 0.30 sits in the gap
inline constexpr double kChainMaxDistance = 0.30;
// Short slices carry noisy voiceprints: a chain break needs both sides this long
inline constexpr std::uint64_t kChainMinSideFrames = 8000;  // 0.5 s

// The frame below which diarisation state is final: segmentation has passed
// it and the turn containing it has ended, so later audio changes nothing
inline std::uint64_t SettledFrontier(std::uint64_t seg_done, std::span<const asr::Turn> turns,
                                     std::uint64_t audio_frames) {
    std::uint64_t last_turn_end = 0;
    for (const auto& turn : turns) {
        const std::uint64_t end = turn.first_frame + turn.frame_count;
        if (end <= audio_frames && end > last_turn_end) last_turn_end = end;
    }
    return std::min(seg_done, last_turn_end);
}

// Merge adjacent same-sounding slices into chains; a chain boundary is a
// speaker-change candidate, decided locally because clustering cannot run
// during capture. A slice too short to embed (empty entry) joins no chain
inline std::vector<Region> ChainSlices(const std::vector<Region>& slices,
                                       const std::vector<std::vector<float>>& embeddings) {
    std::vector<Region> chains;
    const std::vector<float>* prev = nullptr;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto& e = embeddings[i];
        if (e.empty()) continue;
        bool merge = false;
        if (!chains.empty() && prev != nullptr) {
            double dot = 0.0;
            for (std::size_t d = 0; d < e.size(); ++d) dot += (*prev)[d] * e[d];
            merge = 1.0 - dot < kChainMaxDistance;
            const bool substantial =
                slices[i].end_frame - slices[i].first_frame >= kChainMinSideFrames &&
                chains.back().end_frame - chains.back().first_frame >= kChainMinSideFrames;
            if (!substantial) merge = true;
        }
        if (merge) {
            chains.back().end_frame = slices[i].end_frame;
        } else {
            chains.push_back(slices[i]);
        }
        prev = &e;
    }
    return chains;
}

// Where to cut a turn's audio: chain-boundary midpoints inside it, where
// each side keeps a real share of the turn
inline std::vector<std::uint64_t> ChainCutPoints(std::uint64_t turn_first, std::uint64_t turn_end,
                                                 const std::vector<Region>& chains) {
    std::vector<std::uint64_t> cuts;
    for (std::size_t i = 1; i < chains.size(); ++i) {
        const std::uint64_t boundary = (chains[i - 1].end_frame + chains[i].first_frame) / 2;
        if (boundary > turn_first + kResplitMinShareFrames &&
            boundary + kResplitMinShareFrames < turn_end) {
            cuts.push_back(boundary);
        }
    }
    return cuts;
}

}  // namespace sotto::diar
