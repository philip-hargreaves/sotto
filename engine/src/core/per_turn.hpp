#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "core/diar_regions.hpp"
#include "ports/diariser.hpp"

namespace sotto::diar {

// 0.30 s: at 0.40 a clinical "No." was dropped and the note fabricated the denial
inline constexpr std::uint64_t kPerTurnMinClipFrames = 4800;
inline constexpr std::size_t kPerTurnMaxRepeat = 4;  // 5-gram degeneracy guard

namespace detail {

// Most-repeated 5-gram, sliding; legitimate speech peaks at 2
inline std::size_t MaxRepeatedNgram(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const unsigned char c : text) {
        if (std::isalnum(c) != 0) {
            word.push_back(static_cast<char>(std::tolower(c)));
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(word);
    if (words.size() < 6) return 0;
    std::map<std::string, std::size_t> seen;
    std::size_t worst = 0;
    for (std::size_t i = 0; i + 5 <= words.size(); ++i) {
        std::string gram;
        for (std::size_t j = i; j < i + 5; ++j) {
            gram += words[j];
            gram.push_back(' ');
        }
        worst = std::max(worst, ++seen[gram]);
    }
    return worst;
}

}  // namespace detail

// Finalise and speculation must merge identically or cache keys stop matching
inline std::vector<LabelledSlice> MergeByCluster(const std::vector<LabelledSlice>& slices) {
    std::vector<LabelledSlice> turns;
    for (const auto& slice : slices) {
        if (!turns.empty() && turns.back().cluster == slice.cluster) {
            turns.back().end_frame = std::max(turns.back().end_frame, slice.end_frame);
        } else {
            turns.push_back(slice);
        }
    }
    return turns;
}

// Decode spans double as cache keys. Heads clamp past the previous end; a
// nested overlap turn clamps to nothing (decoding it returns the louder
// speaker's words under the wrong name)
inline std::vector<Region> DecodeSpans(const std::vector<LabelledSlice>& turns,
                                       std::uint64_t audio_frames) {
    std::vector<Region> spans(turns.size());
    std::uint64_t prev_end = 0;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const std::uint64_t b = std::min(turns[i].end_frame, audio_frames);
        const std::uint64_t a = std::max(turns[i].first_frame, prev_end);
        spans[i] = {a, b};
        prev_end = std::max(prev_end, b);
    }
    return spans;
}

// Each merged turn gets the text of its own audio; empty means dropped.
// Cached texts are used only on an exact key match, so any hit rate is safe
inline std::vector<std::string> DecodeTurnTexts(const std::vector<LabelledSlice>& turns,
                                                std::span<const float> audio,
                                                const DecodeClipFn& decode,
                                                const TurnTexts* cache = nullptr) {
    const auto spans = DecodeSpans(turns, audio.size());
    std::vector<std::string> texts(turns.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const auto [a, b] = spans[i];
        if (a >= b || b - a < kPerTurnMinClipFrames) continue;
        if (cache != nullptr) {
            const auto it = cache->find({a, b});
            if (it != cache->end()) {
                texts[i] = it->second;
                continue;
            }
        }
        std::string text = decode(audio.subspan(a, b - a), a);
        // A degenerate loop has no safe fallback; empty is the answer
        if (detail::MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
        texts[i] = std::move(text);
    }
    return texts;
}

}  // namespace sotto::diar
