#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace sotto::diar {

inline constexpr std::uint64_t kResplitMinShareFrames = 4800;  // 0.3 s to count as a part
inline constexpr std::uint64_t kResplitMinClipFrames = 6400;   // 0.4 s: shorter clips hallucinate
inline constexpr std::size_t kResplitMaxRepeat = 4;            // 5-gram degeneracy guard

// (clip, absolute first frame) -> transcribed text
using DecodeClipFn = std::function<std::string(std::span<const float>, std::uint64_t)>;

namespace detail {

// Whisper falls into repetition loops on short or near-silent clips; the
// highest legitimate 5-gram repeat measured on real speech is 2
inline std::size_t MaxNgramRepeat(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!word.empty()) words.push_back(word);
    if (words.size() < 5) return 0;
    std::map<std::string, std::size_t> counts;
    std::size_t worst = 0;
    for (std::size_t i = 0; i + 5 <= words.size(); ++i) {
        std::string gram;
        for (std::size_t j = 0; j < 5; ++j) gram += words[i + j] + ' ';
        worst = std::max(worst, ++counts[gram]);
    }
    return worst;
}

}  // namespace detail

// A transcribed turn straddling two different-speaker slices has its AUDIO
// cut at the slice-boundary midpoints and each piece re-transcribed, so no
// word can land on the wrong speaker - splitting the existing text would
// have to guess which word was spoken when. Any piece under 0.4 s, or an
// empty or degenerate decode, aborts the whole turn back to the original
inline std::vector<asr::Turn> ResplitStraddles(const std::vector<asr::Turn>& turns,
                                               const std::vector<LabelledSlice>& slices,
                                               std::span<const float> audio,
                                               const DecodeClipFn& decode) {
    std::vector<asr::Turn> out;
    for (const auto& turn : turns) {
        const std::uint64_t t0 = turn.first_frame;
        const std::uint64_t t1 = turn.first_frame + turn.frame_count;

        // Slices owning a real share, in time order; nested overlap slices
        // are simultaneous speech, not a handover, and same-cluster
        // neighbours are one speaker's pause
        std::vector<const LabelledSlice*> parts;
        for (const auto& slice : slices) {
            const std::uint64_t lo = std::max(slice.first_frame, t0);
            const std::uint64_t hi = std::min(slice.end_frame, t1);
            if (hi <= lo || hi - lo < kResplitMinShareFrames) continue;
            if (!parts.empty()) {
                const auto& prev = *parts.back();
                const std::uint64_t nlo = std::max(prev.first_frame, slice.first_frame);
                const std::uint64_t nhi = std::min(prev.end_frame, slice.end_frame);
                const std::uint64_t dur =
                    std::max<std::uint64_t>(slice.end_frame - slice.first_frame, 1);
                if (nhi > nlo && nhi - nlo > dur / 2) continue;
                if (slice.cluster == prev.cluster) continue;
            }
            parts.push_back(&slice);
        }
        if (parts.size() < 2) {
            out.push_back(turn);
            continue;
        }

        std::vector<asr::Turn> pieces;
        bool ok = true;
        for (std::size_t p = 0; p < parts.size() && ok; ++p) {
            std::uint64_t a = p == 0 ? t0 : (parts[p - 1]->end_frame + parts[p]->first_frame) / 2;
            std::uint64_t b =
                p + 1 == parts.size() ? t1 : (parts[p]->end_frame + parts[p + 1]->first_frame) / 2;
            a = std::clamp(a, t0, t1);
            b = std::clamp(b, a, t1);
            if (b - a < kResplitMinClipFrames || b > audio.size()) {
                ok = false;
                break;
            }
            const std::string text = decode(audio.subspan(a, b - a), a);
            if (text.empty() || detail::MaxNgramRepeat(text) >= kResplitMaxRepeat) {
                ok = false;
                break;
            }
            asr::Turn piece;
            piece.first_frame = a;
            piece.frame_count = b - a;
            piece.text = text;
            pieces.push_back(std::move(piece));
        }
        if (ok) {
            for (auto& piece : pieces) out.push_back(std::move(piece));
        } else {
            out.push_back(turn);  // attribution's straddle split still applies
        }
    }
    return out;
}

}  // namespace sotto::diar
