#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ports/diariser.hpp"

namespace sotto::diar {

// 0.30 s: at 0.40 a 0.39 s "No." answering an eczema question was dropped
// and the note generator fabricated the denial. No finer - "Okay." is 0.38 s
inline constexpr std::uint64_t kPerTurnMinClipFrames = 4800;
inline constexpr std::size_t kPerTurnMaxRepeat = 4;  // 5-gram degeneracy guard

namespace detail {

// Highest repeat count of any 5-gram, sliding: a strided scan misses a
// cycle whose period does not divide five. Legitimate speech peaks at 2
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

// Merge consecutive same-cluster slices on boundaries only. Finalise and
// capture-time speculation must run this identical operation, or the
// speculation cache's keys stop matching
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

// Give each merged turn the text of its own audio - there is nothing to
// align, so no word can land on the wrong speaker. A turn's head clamps
// past the previous end: that audio is already decoded. A turn nested
// wholly inside its neighbour (an overlap's second speaker) gets no text:
// its audio belongs to whoever talked through it, and a decode puts their
// words under the wrong name - measured, worse than the drop. An empty
// entry means the turn is dropped
inline std::vector<std::string> DecodeTurnTexts(const std::vector<LabelledSlice>& turns,
                                                std::span<const float> audio,
                                                const DecodeClipFn& decode) {
    std::vector<std::string> texts(turns.size());
    std::uint64_t prev_end = 0;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const std::uint64_t b = std::min<std::uint64_t>(turns[i].end_frame, audio.size());
        const std::uint64_t a = std::max(turns[i].first_frame, prev_end);
        const bool nested = a >= b;
        prev_end = std::max(prev_end, b);
        if (nested) continue;
        if (b - a < kPerTurnMinClipFrames) continue;
        std::string text = decode(audio.subspan(a, b - a), a);
        // A degenerate loop has no safe fallback; empty is the answer
        if (detail::MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
        texts[i] = std::move(text);
    }
    return texts;
}

}  // namespace sotto::diar
