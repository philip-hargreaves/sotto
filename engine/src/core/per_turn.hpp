#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/diar_regions.hpp"
#include "core/env_flag.hpp"
#include "core/padded_decode.hpp"
#include "ports/diariser.hpp"

namespace ambient::diar {

// 0.30 s: at 0.40 a clinical "No." was dropped and the note fabricated the denial.
// MinClipFrames() is the runtime value (AMBIENT_CLIP_FLOOR_MS)
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

// Spans double as cache keys; heads clamp past the previous end, a nested
// overlap turn clamps to nothing
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

// AMBIENT_CHUNK_ASSEMBLE: a span whose edges sit on a cached decode's chunk edges
// (a cut re-sliced a decoded turn) takes those chunks instead of a second decode
inline constexpr std::uint64_t kAssembleTolFrames = 5600;  // 0.35 s: snap window plus span clamp

inline std::optional<std::vector<asr::Turn>> AssembleFromChunks(const TurnChunks& cache,
                                                                std::uint64_t a, std::uint64_t b) {
    if (!EnvFlag("AMBIENT_CHUNK_ASSEMBLE")) return std::nullopt;
    const auto close_to = [](std::uint64_t x, std::uint64_t y) {
        return (x > y ? x - y : y - x) <= kAssembleTolFrames;
    };
    for (const auto& [span, chunks] : cache) {
        if (span.first > a + kAssembleTolFrames || span.second + kAssembleTolFrames < b) continue;
        if (chunks.empty()) continue;
        std::vector<asr::Turn> inside;
        for (const auto& c : chunks) {
            const std::uint64_t mid = c.first_frame + c.frame_count / 2;
            if (mid >= a && mid < b) inside.push_back(c);
        }
        if (inside.empty()) continue;
        const auto& first = inside.front();
        const auto& last = inside.back();
        if (!close_to(first.first_frame, a) || !close_to(last.first_frame + last.frame_count, b)) continue;
        if (EnvFlag("AMBIENT_CUT_DEBUG")) {
            std::fprintf(stderr, "ambient-engine: assembled %.2f-%.2f s from decode %.2f-%.2f s (%zu chunks)\n",
                         a / 16000.0, b / 16000.0, span.first / 16000.0, span.second / 16000.0,
                         inside.size());
        }
        return inside;
    }
    return std::nullopt;
}

// Each merged turn gets the text of its own audio; empty means dropped.
// Cached texts are used only on an exact key match, so any hit rate is safe.
// Long spans first, so short ones can be padded with their neighbours' words
inline std::vector<std::string> DecodeTurnTexts(const std::vector<LabelledSlice>& turns,
                                                std::span<const float> audio,
                                                const DecodeClipFn& decode,
                                                const TurnTexts* cache = nullptr,
                                                const TurnChunks* chunk_cache = nullptr,
                                                std::vector<std::vector<asr::Turn>>* chunks_out = nullptr) {
    const auto spans = DecodeSpans(turns, audio.size());
    std::vector<std::string> texts(turns.size());
    if (chunks_out != nullptr) chunks_out->assign(turns.size(), {});
    const auto pad = ClipPadFrames();
    for (const bool short_pass : {false, true}) {
        for (std::size_t i = 0; i < turns.size(); ++i) {
            const auto [a, b] = spans[i];
            if (a >= b || b - a < MinClipFrames()) continue;
            if ((b - a < kPadBelowFrames) != short_pass) continue;
            if (cache != nullptr) {
                const auto it = cache->find({a, b});
                if (it != cache->end()) {
                    texts[i] = it->second;
                    if (chunks_out != nullptr && chunk_cache != nullptr) {
                        const auto ct = chunk_cache->find({a, b});
                        if (ct != chunk_cache->end()) (*chunks_out)[i] = ct->second;
                    }
                    continue;
                }
            }
            if (chunk_cache != nullptr) {
                if (auto assembled = AssembleFromChunks(*chunk_cache, a, b)) {
                    texts[i] = JoinedText(*assembled);
                    if (chunks_out != nullptr) (*chunks_out)[i] = std::move(*assembled);
                    continue;
                }
            }
            auto chunks = PaddedDecode(audio, a, b, pad, decode,
                                       i > 0 ? texts[i - 1] : std::string(),
                                       i + 1 < turns.size() ? texts[i + 1] : std::string());
            std::string text = JoinedText(chunks);
            // A degenerate loop has no safe fallback; empty is the answer
            if (detail::MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
            texts[i] = std::move(text);
            if (chunks_out != nullptr) (*chunks_out)[i] = std::move(chunks);
        }
    }
    return texts;
}

// Merged turns whose text the cache already holds, in order, up to the first
// span finalise would have to decode: from there on the sealed transcript is
// unknowable. Spans below the clip floor are skipped as finalise skips them
inline std::vector<LabelledSlice> SpeculatedTurns(const std::vector<LabelledSlice>& merged,
                                                  std::uint64_t audio_frames,
                                                  const TurnTexts& cache,
                                                  std::vector<std::string>* texts) {
    const auto spans = DecodeSpans(merged, audio_frames);
    std::vector<LabelledSlice> known;
    texts->clear();
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const auto [a, b] = spans[i];
        if (a >= b || b - a < MinClipFrames()) continue;
        const auto it = cache.find({a, b});
        if (it == cache.end()) break;
        known.push_back(merged[i]);
        texts->push_back(it->second);
    }
    return known;
}

}  // namespace ambient::diar
