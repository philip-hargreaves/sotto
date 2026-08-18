#pragma once

#include <span>
#include <vector>

#include "adapters/diarisation/speaker_embedder.hpp"
#include "core/diar_regions.hpp"
#include "ports/diariser.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kVoiceprintCapFrames = 90 * 16000;  // long-exposure cap
inline constexpr std::uint64_t kVoiceprintMinFrames = 16000;       // under 1 s carries no identity

// The cluster's slices in time order until the cap (the crossing slice
// kept whole); empty under one second
inline std::vector<Region> VoiceprintRanges(const std::vector<LabelledSlice>& slices, int cluster) {
    std::vector<Region> ranges;
    std::uint64_t total = 0;
    for (const auto& slice : slices) {
        if (slice.cluster != cluster) continue;
        ranges.push_back({slice.first_frame, slice.end_frame});
        total += slice.end_frame - slice.first_frame;
        if (total >= kVoiceprintCapFrames) break;
    }
    if (total < kVoiceprintMinFrames) return {};
    return ranges;
}

// One voiceprint per cluster: its audio concatenated and embedded once,
// not a mean of per-slice embeddings. Empty when too short
std::vector<float> ClusterVoiceprint(SpeakerEmbedder& embedder, std::span<const float> audio,
                                     const std::vector<LabelledSlice>& slices, int cluster);

}  // namespace sotto::diar
