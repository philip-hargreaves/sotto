#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

// A transcribed turn goes whole to the slice owning most of it unless a
// second slice holds enough to claim a share
inline constexpr std::uint64_t kClaimMinFrames = 4800;  // 0.3 s
inline constexpr double kClaimMinShare = 0.2;
// A genuinely shared turn splits only when there is enough to split
inline constexpr std::uint64_t kSplitMinFrames = 19200;  // 1.2 s
inline constexpr std::size_t kSplitMinChars = 16;

namespace detail {

inline std::string Trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.pop_back();
    return s;
}

inline void AppendText(std::string& text, const std::string& more) {
    if (more.empty()) return;
    if (!text.empty()) text += ' ';
    text += more;
}

// Cut position for text straddling a speaker change: the sentence
// punctuation nearest the time-interpolated boundary (speaker changes fall
// on sentence ends), a comma next, else the nearest word gap
inline std::size_t SplitPoint(const std::string& text, double fraction) {
    const long n = static_cast<long>(text.size());
    const long target = static_cast<long>(fraction * static_cast<double>(n));
    const long window = static_cast<long>(0.35 * static_cast<double>(n)) + 1;
    long best_sentence = -1, best_comma = -1, best_space = -1;
    for (long i = 0; i + 1 < n; ++i) {
        if (text[static_cast<std::size_t>(i + 1)] != ' ') continue;
        const char c = text[static_cast<std::size_t>(i)];
        const auto closer = [&](long current) {
            return current < 0 || std::labs(i + 2 - target) < std::labs(current - target);
        };
        if ((c == '.' || c == '?' || c == '!') && closer(best_sentence)) {
            best_sentence = i + 2;  // cut after "X. "
        } else if (c == ',' && closer(best_comma)) {
            best_comma = i + 2;
        } else if (closer(best_space)) {
            best_space = i + 2;
        }
    }
    for (const long candidate : {best_sentence, best_comma, best_space}) {
        if (candidate >= 0 && std::labs(candidate - target) <= window) {
            return static_cast<std::size_t>(candidate);
        }
    }
    return static_cast<std::size_t>(std::clamp(target, 0L, n));
}

inline std::uint64_t Overlap(const LabelledSlice& span, std::uint64_t first, std::uint64_t end) {
    const std::uint64_t lo = std::max(span.first_frame, first);
    const std::uint64_t hi = std::min(span.end_frame, end);
    return hi > lo ? hi - lo : 0;
}

}  // namespace detail

// Assign every transcribed turn's text to a diarised slice:
// majority overlap; a turn genuinely shared between two slices splits at
// the punctuation nearest the handover; a short shared turn goes to the
// most SPECIFIC claimant (largest share of its own length), so a
// backchannel claims its "Okay." from inside a monologue; a turn
// overlapping nothing goes to the nearest slice by midpoint - words are
// never dropped
inline std::vector<std::string> AssignSliceTexts(const std::vector<asr::Turn>& transcribed,
                                                 const std::vector<LabelledSlice>& slices) {
    std::vector<std::string> texts(slices.size());
    if (slices.empty()) return texts;

    for (const asr::Turn& turn : transcribed) {
        if (turn.text.empty()) continue;
        const std::uint64_t first = turn.first_frame;
        const std::uint64_t end = turn.first_frame + turn.frame_count;
        const double length = std::max<double>(static_cast<double>(turn.frame_count), 1.0);
        const double mid = static_cast<double>(first) + 0.5 * static_cast<double>(turn.frame_count);

        long best = -1, second = -1;
        std::uint64_t best_ov = 0, second_ov = 0;
        double nearest_gap = std::numeric_limits<double>::max();
        std::size_t nearest = 0;
        for (std::size_t i = 0; i < slices.size(); ++i) {
            const std::uint64_t ov = detail::Overlap(slices[i], first, end);
            if (ov > best_ov) {
                second_ov = best_ov;
                second = best;
                best_ov = ov;
                best = static_cast<long>(i);
            } else if (ov > second_ov) {
                second_ov = ov;
                second = static_cast<long>(i);
            }
            const double gap = mid < static_cast<double>(slices[i].first_frame)
                                   ? static_cast<double>(slices[i].first_frame) - mid
                               : mid > static_cast<double>(slices[i].end_frame)
                                   ? mid - static_cast<double>(slices[i].end_frame)
                                   : 0.0;
            if (gap < nearest_gap) {
                nearest_gap = gap;
                nearest = i;
            }
        }
        if (best < 0 || best_ov == 0) {  // overlaps nothing: nearest by midpoint, never dropped
            detail::AppendText(texts[nearest], turn.text);
            continue;
        }

        const bool shared = second >= 0 && second_ov >= kClaimMinFrames &&
                            static_cast<double>(second_ov) >= kClaimMinShare * length;
        if (!shared) {
            detail::AppendText(texts[static_cast<std::size_t>(best)], turn.text);
        } else if (turn.frame_count >= kSplitMinFrames && turn.text.size() >= kSplitMinChars) {
            const auto b = static_cast<std::size_t>(best);
            const auto s = static_cast<std::size_t>(second);
            const std::size_t earlier = slices[b].first_frame <= slices[s].first_frame ? b : s;
            const std::size_t later = earlier == b ? s : b;
            const double boundary =
                std::clamp(0.5 * (static_cast<double>(slices[earlier].end_frame) +
                                  static_cast<double>(slices[later].first_frame)),
                           static_cast<double>(first), static_cast<double>(end));
            const std::size_t cut =
                detail::SplitPoint(turn.text, (boundary - static_cast<double>(first)) / length);
            detail::AppendText(texts[earlier], detail::Trimmed(turn.text.substr(0, cut)));
            detail::AppendText(texts[later], detail::Trimmed(turn.text.substr(cut)));
        } else {
            const auto b = static_cast<std::size_t>(best);
            const auto s = static_cast<std::size_t>(second);
            const double db = std::max<double>(
                static_cast<double>(slices[b].end_frame - slices[b].first_frame), 1.0);
            const double ds = std::max<double>(
                static_cast<double>(slices[s].end_frame - slices[s].first_frame), 1.0);
            const bool specific =
                static_cast<double>(second_ov) / ds > static_cast<double>(best_ov) / db;
            detail::AppendText(texts[specific ? s : b], turn.text);
        }
    }

    return texts;
}

// Slices plus their texts become display turns: consecutive same-speaker
// slices merge, wordless slices disappear. role_of_cluster names each
// cluster; empty falls back to "speaker N"
inline std::vector<asr::Turn> BuildAttributedTurns(
    const std::vector<LabelledSlice>& slices, const std::vector<std::string>& texts,
    const std::vector<std::string>& role_of_cluster = {}) {
    std::vector<asr::Turn> out;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        if (texts[i].empty()) continue;
        const auto cluster = static_cast<std::size_t>(slices[i].cluster);
        const std::string speaker = cluster < role_of_cluster.size()
                                        ? role_of_cluster[cluster]
                                        : "speaker " + std::to_string(slices[i].cluster + 1);
        if (!out.empty() && out.back().speaker == speaker) {
            out.back().frame_count = slices[i].end_frame - out.back().first_frame;
            detail::AppendText(out.back().text, texts[i]);
        } else {
            asr::Turn turn;
            turn.first_frame = slices[i].first_frame;
            turn.frame_count = slices[i].end_frame - slices[i].first_frame;
            turn.speaker = speaker;
            turn.text = texts[i];
            out.push_back(std::move(turn));
        }
    }
    return out;
}

inline std::vector<asr::Turn> AttributeSpeakers(const std::vector<asr::Turn>& transcribed,
                                                const std::vector<LabelledSlice>& slices) {
    return BuildAttributedTurns(slices, AssignSliceTexts(transcribed, slices));
}

}  // namespace sotto::diar
