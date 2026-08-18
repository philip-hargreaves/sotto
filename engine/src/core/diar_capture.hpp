#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "ports/transcriber.hpp"

namespace sotto::diar {

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

}  // namespace sotto::diar
