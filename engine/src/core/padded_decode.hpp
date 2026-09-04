#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "core/env_flag.hpp"
#include "core/turn_assembly.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"

namespace ambient::diar {

// AMBIENT_CLIP_PAD=<ms>: a clip shorter than kPadBelowFrames is decoded with up
// to <ms> of neighbouring audio each side, so Whisper hears the question an
// answer replies to. The neighbours' words are trimmed off by matching their own
// decodes; a side that cannot be matched falls back to the clip's own audio.
// AMBIENT_CLIP_FLOOR_MS lowers the 300 ms clip floor to match.
// Closed ablation: the padded-in question biases the answer
inline constexpr std::uint64_t kPadBelowFrames = 48000;  // 3 s

inline std::uint64_t ClipPadFrames() {
    static const std::uint64_t frames = [] {
        const std::string value = EnvValue("AMBIENT_CLIP_PAD");
        return value.empty() ? 0ull : static_cast<std::uint64_t>(std::atol(value.c_str())) * 16;
    }();
    return frames;
}

inline std::uint64_t MinClipFrames() {
    static const std::uint64_t frames = [] {
        const std::string value = EnvValue("AMBIENT_CLIP_FLOOR_MS");
        return value.empty() ? 4800ull : static_cast<std::uint64_t>(std::atol(value.c_str())) * 16;
    }();
    return frames;
}

namespace detail {

inline std::vector<std::string> NormalisedWords(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& word : asr::detail::SplitWords(text)) {
        const auto n = asr::detail::NormalisedWord(word);
        if (!n.empty()) out.push_back(n);
    }
    return out;
}

// The same word rendered twice by Whisper: equal, or one a prefix or suffix of
// the other (ok/okay, a partial word at the pad edge)
inline bool SameWord(const std::string& x, const std::string& y) {
    if (x == y) return true;
    const auto& shorter = x.size() < y.size() ? x : y;
    const auto& longer = x.size() < y.size() ? y : x;
    if (shorter.size() < 3 && shorter != "ok") return false;
    return longer.compare(0, shorter.size(), shorter) == 0 ||
           longer.compare(longer.size() - shorter.size(), shorter.size(), shorter) == 0;
}

}  // namespace detail

// Word timings smear up to about 0.5 s into the pause either side of a cut, and
// the neighbours' decodes drop partial words, so neither timing nor text alone
// places the edge. Words well inside the clip are its own; edge-zone words are
// too unless, from the edge inwards, they repeat the neighbour's boundary words
inline constexpr std::uint64_t kEdgeZoneFrames = 8000;  // 0.5 s

inline std::vector<asr::Turn> TrimToClip(const std::vector<asr::Turn>& words, std::uint64_t a,
                                         std::uint64_t b, const std::string& before,
                                         const std::string& after,
                                         std::uint64_t zone = kEdgeZoneFrames) {
    const std::uint64_t lo = a > zone ? a - zone : 0;
    std::vector<asr::Turn> kept;
    for (const auto& w : words) {
        const std::uint64_t mid = w.first_frame + w.frame_count / 2;
        if (mid >= lo && mid < b + zone) kept.push_back(w);
    }
    const auto mid_of = [](const asr::Turn& w) { return w.first_frame + w.frame_count / 2; };
    const auto same = [](const asr::Turn& w, const std::string& n) {
        return detail::SameWord(asr::detail::NormalisedWord(w.text), n);
    };
    // Left edge: the longest run from the edge that repeats the tail of `before`
    const auto before_words = detail::NormalisedWords(before);
    std::size_t left_edge = 0;
    while (left_edge < kept.size() && mid_of(kept[left_edge]) < a + zone) ++left_edge;
    std::size_t drop_front = 0;
    for (std::size_t n = std::min(left_edge, before_words.size()); n > 0 && drop_front == 0; --n) {
        bool match = true;
        for (std::size_t i = 0; i < n && match; ++i) {
            match = same(kept[i], before_words[before_words.size() - n + i]);
        }
        if (match) drop_front = n;
    }
    // Right edge: the longest run up to the edge that repeats the head of `after`
    const auto after_words = detail::NormalisedWords(after);
    std::size_t right_edge = 0;
    while (right_edge + drop_front < kept.size() &&
           mid_of(kept[kept.size() - 1 - right_edge]) >= b - std::min(b, zone)) {
        ++right_edge;
    }
    std::size_t drop_back = 0;
    for (std::size_t n = std::min(right_edge, after_words.size()); n > 0 && drop_back == 0; --n) {
        bool match = true;
        for (std::size_t i = 0; i < n && match; ++i) {
            match = same(kept[kept.size() - n + i], after_words[i]);
        }
        if (match) drop_back = n;
    }
    kept.erase(kept.end() - static_cast<std::ptrdiff_t>(drop_back), kept.end());
    kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(drop_front));
    return kept;
}

// [a, b) decoded with context: pad the sides that have a neighbour, keep the
// clip's own words. An empty result falls back to the plain clip, so padding
// can only add words
inline std::vector<asr::Turn> PaddedDecode(std::span<const float> audio, std::uint64_t a,
                                           std::uint64_t b, std::uint64_t pad,
                                           const DecodeClipFn& decode, const std::string& before,
                                           const std::string& after) {
    const auto plain = [&] { return decode(audio.subspan(a, b - a), a); };
    if (pad == 0 || b - a >= kPadBelowFrames) return plain();
    // AMBIENT_CLIP_PAD_SIDE=after|before: pad one side only
    static const std::string side = EnvValue("AMBIENT_CLIP_PAD_SIDE");
    const bool pad_before = !before.empty() && side != "after";
    const bool pad_after = !after.empty() && side != "before";
    const std::uint64_t a0 = pad_before ? a - std::min(pad, a) : a;
    const std::uint64_t b0 = pad_after ? std::min<std::uint64_t>(b + pad, audio.size()) : b;
    if (a0 == a && b0 == b) return plain();

    const auto words = decode(audio.subspan(a0, b0 - a0), a0);
    const auto inside = TrimToClip(words, a, b, before, after);
    const bool debug = EnvFlag("AMBIENT_CUT_DEBUG");
    if (inside.empty()) {
        if (debug) {
            std::fprintf(stderr, "pad %.1f-%.1f: %zu words, none inside, plain\n", a / 16000.0,
                         b / 16000.0, words.size());
        }
        return plain();
    }
    if (debug) {
        std::fprintf(stderr, "pad %.1f-%.1f: %zu of %zu words kept: '%s'\n", a / 16000.0,
                     b / 16000.0, inside.size(), words.size(), JoinedText(inside).c_str());
    }
    return inside;
}

}  // namespace ambient::diar
