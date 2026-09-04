#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/turn_assembly.hpp"
#include "ports/transcriber.hpp"

namespace ambient::diar {

// AMBIENT_TIDY: the sealed transcript tidied as a professional
// transcriber would. Three rules in this order; none moves a word between speakers:
//   1. one speaker's consecutive turns under kTidyMergeGapFrames apart merge, so a
//      fragment cut from its sentence ("So I..." / "to know what's going on.") rejoins it
//   2. a turn with no lexical content (disfluencies, or one or two stranded function
//      words) is dropped; yes, no, ok and any content word never are
//   3. every turn starts with a capital and ends with terminal punctuation;
//      a standalone "i" is "I"
inline constexpr std::uint64_t kTidyMergeGapFrames = 16000;  // 1 s

namespace detail {

inline bool IsDisfluency(const std::string& w) {
    static const char* const kWords[] = {"um", "uh",  "er",  "erm", "hm", "hmm",
                                         "mm", "mmm", "mhm", "ah",  "eh", "huh"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

inline bool IsFunctionWord(const std::string& w) {
    static const char* const kWords[] = {"and", "so", "but", "or", "the", "a",
                                         "an",  "to", "of",  "in", "i",   "it"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

// No lexical content: only disfluencies, or at most two function words
inline bool NoContent(const std::string& text) {
    std::size_t function_words = 0;
    std::size_t words = 0;
    for (const auto& raw : asr::detail::SplitWords(text)) {
        const auto w = asr::detail::NormalisedWord(raw);
        if (w.empty()) continue;
        ++words;
        if (IsDisfluency(w)) continue;
        if (IsFunctionWord(w)) {
            ++function_words;
            continue;
        }
        return false;
    }
    return words > 0 && function_words <= 2;
}

inline bool EndsSentence(const std::string& text) {
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        const auto c = static_cast<unsigned char>(*it);
        if (std::isspace(c) != 0) continue;
        if (*it == '"' || *it == '\'' || *it == ')') continue;
        return *it == '.' || *it == '?' || *it == '!';
    }
    return false;
}

inline std::string Capitalised(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) != 0) {
            text[i] = static_cast<char>(std::toupper(c));
            break;
        }
        if (std::isdigit(c) != 0) break;
    }
    // standalone i / i'm / i've / i'll / i'd
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != 'i') continue;
        const bool start = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) != 0;
        const bool end = i + 1 == text.size() ||
                         std::isspace(static_cast<unsigned char>(text[i + 1])) != 0 ||
                         text[i + 1] == '\'' || text[i + 1] == ',' || text[i + 1] == '.';
        if (start && end) text[i] = 'I';
    }
    return text;
}

inline std::string Terminated(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    if (text.empty() || EndsSentence(text)) return text;
    if (text.back() == ',' || text.back() == ';' || text.back() == ':') text.pop_back();
    if (!text.empty() && std::isalnum(static_cast<unsigned char>(text.back())) != 0) {
        text.push_back('.');
    }
    return text;
}

}  // namespace detail

inline std::vector<asr::Turn> TidyTranscript(std::vector<asr::Turn> turns) {
    std::vector<asr::Turn> merged;
    for (auto& turn : turns) {
        if (turn.text.empty()) continue;
        if (!merged.empty() && merged.back().speaker == turn.speaker) {
            auto& prev = merged.back();
            const std::uint64_t prev_end = prev.first_frame + prev.frame_count;
            const std::uint64_t gap = turn.first_frame > prev_end ? turn.first_frame - prev_end : 0;
            if (gap <= kTidyMergeGapFrames) {
                // A fragment after a finished sentence starts the next one; after
                // a trail-off ("So I...") it continues the same one
                const bool trail_off =
                    prev.text.size() >= 3 && prev.text.compare(prev.text.size() - 3, 3, "...") == 0;
                prev.text += ' ';
                prev.text += detail::EndsSentence(prev.text) && !trail_off
                                 ? detail::Capitalised(turn.text)
                                 : turn.text;
                const std::uint64_t end = turn.first_frame + turn.frame_count;
                if (end > prev_end) prev.frame_count = end - prev.first_frame;
                continue;
            }
        }
        merged.push_back(std::move(turn));
    }
    std::vector<asr::Turn> out;
    for (auto& turn : merged) {
        if (detail::NoContent(turn.text)) continue;
        turn.text = detail::Terminated(detail::Capitalised(std::move(turn.text)));
        out.push_back(std::move(turn));
    }
    return out;
}

}  // namespace ambient::diar
