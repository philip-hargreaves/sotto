#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "core/diar_regions.hpp"
#include "core/env_flag.hpp"
#include "ports/audio_source.hpp"

namespace ambient::diar {

// AMBIENT_CLIP_CUTS: a Whisper chunk edge is a few hundred ms out, so a raw
// edge clips a word or leaves a sliver. An edge is kept only where the VAD shows
// a pause within the window, moved onto that pause, and kept clear of other cuts
inline constexpr std::uint64_t kClipCutSnapFrames = 4800;    // 300 ms each side
inline constexpr std::uint64_t kClipCutMinGapFrames = 8000;  // 0.5 s from any other cut
inline std::vector<std::uint64_t> SnapClipCuts(std::span<const std::uint64_t> cuts,
                                               std::span<const float> vad_probabilities,
                                               std::span<const std::uint64_t> other_cuts) {
    std::vector<std::uint64_t> kept;
    for (const std::uint64_t cut : cuts) {
        const std::uint64_t lo = cut > kClipCutSnapFrames ? cut - kClipCutSnapFrames : 0;
        const std::uint64_t hi = cut + kClipCutSnapFrames;
        std::uint64_t best = cut;
        float best_p = 1.0f;
        for (std::size_t hop = static_cast<std::size_t>(lo / audio::kVadHopFrames);
             hop * audio::kVadHopFrames <= hi && hop < vad_probabilities.size(); ++hop) {
            if (vad_probabilities[hop] < best_p) {
                best_p = vad_probabilities[hop];
                best = static_cast<std::uint64_t>(hop) * audio::kVadHopFrames;
            }
        }
        if (best_p >= kEnter) continue;  // no pause near: mid-sentence, drop
        const auto near = [best](std::uint64_t other) {
            return (other > best ? other - best : best - other) < kClipCutMinGapFrames;
        };
        if (std::any_of(other_cuts.begin(), other_cuts.end(), near)) continue;
        if (std::any_of(kept.begin(), kept.end(), near)) continue;
        kept.push_back(best);
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

// AMBIENT_CUT_DEBUG: every raw edge with its nearest VAD minimum, then the cuts
// kept, in seconds
inline void LogClipCuts(const char* who, std::span<const std::uint64_t> raw,
                        std::span<const std::uint64_t> kept,
                        std::span<const float> vad_probabilities) {
    if (!EnvFlag("AMBIENT_CUT_DEBUG")) return;
    constexpr std::uint64_t kLook = 2 * kClipCutSnapFrames;
    for (const std::uint64_t cut : raw) {
        const std::uint64_t lo = cut > kLook ? cut - kLook : 0;
        float best_p = 1.0f;
        std::uint64_t best = cut;
        for (std::size_t hop = static_cast<std::size_t>(lo / audio::kVadHopFrames);
             hop * audio::kVadHopFrames <= cut + kLook && hop < vad_probabilities.size(); ++hop) {
            if (vad_probabilities[hop] < best_p) {
                best_p = vad_probabilities[hop];
                best = static_cast<std::uint64_t>(hop) * audio::kVadHopFrames;
            }
        }
        std::fprintf(stderr, "cut-debug %s raw %.2f min-vad %.2f at %.2f\n", who,
                     static_cast<double>(cut) / audio::kSampleRate, static_cast<double>(best_p),
                     static_cast<double>(best) / audio::kSampleRate);
    }
    for (const std::uint64_t cut : kept) {
        std::fprintf(stderr, "cut-debug %s kept %.2f\n", who,
                     static_cast<double>(cut) / audio::kSampleRate);
    }
}

}  // namespace ambient::diar
