#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace ambient::asr {

// Pure steps between a window's decode and its emitted turns; measured
// configuration - do not retune
inline constexpr std::uint64_t kAnchorSnapFrames = 24000;  // 1.5 s
inline constexpr std::uint64_t kAdjacentGapFrames = 4000;  // 0.25 s
inline constexpr std::size_t kMaxDedupWords = 4;

namespace detail {

inline std::string NormalisedWord(const std::string& word) {
    std::string out;
    for (const char c : word) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 || c == '\'') out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

inline std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

inline std::string WithoutLeadingWords(const std::string& text, std::size_t count) {
    std::size_t pos = 0;
    for (std::size_t dropped = 0; dropped < count && pos < text.size(); ++dropped) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) == 0) ++pos;
    }
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos;
    return text.substr(pos);
}

}  // namespace detail

// Whisper's first-segment stamp drifts late at window starts; a small
// drift snaps back to the sample-accurate window start
inline void AnchorFirstTurn(std::vector<Turn>& turns, std::uint64_t window_first_frame) {
    if (turns.empty()) return;
    Turn& first = turns.front();
    if (first.first_frame <= window_first_frame) return;
    const std::uint64_t drift = first.first_frame - window_first_frame;
    if (drift > kAnchorSnapFrames) return;
    first.first_frame = window_first_frame;
    first.frame_count += drift;
}

// A midpoint in the re-heard overlap was already emitted; dropping by time
// is robust where text matching is not
inline void DropReheardTurns(std::vector<Turn>& turns, std::uint64_t first_new_frame) {
    std::erase_if(turns, [first_new_frame](const Turn& turn) {
        return turn.first_frame + turn.frame_count / 2 < first_new_frame;
    });
}

// A forced mid-word cut can emit the boundary word twice; only forced cuts
// are time-contiguous, so the longest common word run strips from the
// newer turn. Real silence keeps genuine repetition
inline void StripBoundaryDuplicates(const Turn& prev, Turn& next) {
    if (prev.text.empty() || next.text.empty()) return;
    if (next.first_frame > prev.first_frame + prev.frame_count + kAdjacentGapFrames) return;
    const auto prev_words = detail::SplitWords(prev.text);
    const auto next_words = detail::SplitWords(next.text);
    for (std::size_t run = std::min({kMaxDedupWords, prev_words.size(), next_words.size()});
         run >= 1; --run) {
        bool match = true;
        for (std::size_t i = 0; i < run && match; ++i) {
            const std::string word =
                detail::NormalisedWord(prev_words[prev_words.size() - run + i]);
            match = !word.empty() && word == detail::NormalisedWord(next_words[i]);
        }
        if (match) {
            next.text = detail::WithoutLeadingWords(next.text, run);
            return;
        }
    }
}

}  // namespace ambient::asr
