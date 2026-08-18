#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "core/role_naming.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kReconcileGapFrames = 24000;  // 1.5 s: beyond this is no boundary
inline constexpr double kReconcileMinSimilarity = 0.66;
inline constexpr std::size_t kReconcileMaxTailWords = 10;

namespace detail {

inline std::size_t WordEdit(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min(
                {prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1), prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

inline std::string DropTailWords(const std::string& text, std::size_t count) {
    std::size_t end = text.size();
    for (std::size_t dropped = 0; dropped < count && end > 0; ++dropped) {
        while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
        while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) == 0) --end;
    }
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(0, end);
}

}  // namespace detail

// Whisper hears a window boundary twice, and its timestamps are sloppy
// enough that the time trim can miss the repeat: adjacent turns then carry
// the same phrase and overlap in time. Fuzzy-match the earlier turn's tail
// against the later turn's head (word edit distance, so different
// renderings still match) and drop the matched tail from the EARLIER turn -
// the later window heard that speech with full context. Spans are clamped
// afterwards so turns no longer overlap
inline void ReconcileTurns(std::vector<asr::Turn>& turns) {
    for (std::size_t i = 1; i < turns.size(); ++i) {
        auto& prev = turns[i - 1];
        const auto& next = turns[i];
        if (prev.text.empty() || next.text.empty()) continue;
        const std::uint64_t prev_end = prev.first_frame + prev.frame_count;
        if (next.first_frame > prev_end + kReconcileGapFrames) continue;

        const auto prev_words = detail::WordsOf(prev.text);
        const auto next_words = detail::WordsOf(next.text);
        const std::size_t max_tail = std::min(kReconcileMaxTailWords, prev_words.size());
        std::size_t drop = 0;
        double best = 0.0;
        for (std::size_t tail_len = 2; tail_len <= max_tail; ++tail_len) {
            const std::vector<std::string> tail(
                prev_words.end() - static_cast<std::ptrdiff_t>(tail_len), prev_words.end());
            for (std::size_t head_len = tail_len - 1;
                 head_len <= std::min(tail_len + 1, next_words.size()); ++head_len) {
                if (head_len == 0) continue;
                const std::vector<std::string> head(
                    next_words.begin(), next_words.begin() + static_cast<std::ptrdiff_t>(head_len));
                const double sim = 1.0 - static_cast<double>(detail::WordEdit(tail, head)) /
                                             static_cast<double>(std::max(tail_len, head_len));
                if (sim >= kReconcileMinSimilarity && sim * static_cast<double>(tail_len) > best) {
                    best = sim * static_cast<double>(tail_len);
                    drop = tail_len;
                }
            }
        }
        if (drop >= 2) prev.text = detail::DropTailWords(prev.text, drop);
        if (prev_end > next.first_frame && next.first_frame > prev.first_frame) {
            prev.frame_count = next.first_frame - prev.first_frame;
        }
    }
}

}  // namespace sotto::diar
