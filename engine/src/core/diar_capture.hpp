#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "core/diar_regions.hpp"
#include "ports/transcriber.hpp"

namespace ambient::diar {

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

// Windowless ablation (AMBIENT_SEG_FRONTIER, requires AMBIENT_DIAR_SEG_CUTS_ONLY):
// settledness from segmentation and VAD alone, no live turns. A region's final
// extent depends on at most kMinSilenceFrames (close) + 2*kPadFrames (padding)
// of audio past its raw end, so behind this margin spans cannot change
inline constexpr std::uint64_t kSegFrontierMarginFrames = kMinSilenceFrames + 2 * kPadFrames;

inline std::uint64_t SegSettledFrontier(std::uint64_t seg_done, std::uint64_t vad_frames) {
    const std::uint64_t vad_safe =
        vad_frames > kSegFrontierMarginFrames ? vad_frames - kSegFrontierMarginFrames : 0;
    return std::min(seg_done, vad_safe);
}

}  // namespace ambient::diar
