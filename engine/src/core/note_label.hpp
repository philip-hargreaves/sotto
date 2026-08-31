#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sotto::core {

// The consultation in a line for the session list: the note's first
// sentence, cut at a word when longer than the cap. A sentence ends at
// . ! ? followed by whitespace or the end, so "1.5 mg" does not end it
// (an abbreviation such as "Dr." does: a label, not a parser); whitespace
// collapses so a header or line break never lands in the list
inline std::string LabelFrom(std::string_view note, std::size_t max_chars = 80) {
    std::string label;
    bool space_pending = false;
    for (std::size_t i = 0; i < note.size(); ++i) {
        const char c = note[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            space_pending = !label.empty();
            continue;
        }
        if (space_pending) {
            label += ' ';
            space_pending = false;
        }
        if (c == '.' || c == '!' || c == '?') {
            const bool ends = i + 1 == note.size() || note[i + 1] == ' ' || note[i + 1] == '\n' ||
                              note[i + 1] == '\r' || note[i + 1] == '\t';
            if (ends && !label.empty()) {
                break;
            }
        }
        label += c;
    }
    if (label.size() > max_chars) {
        const auto cut = label.rfind(' ', max_chars);
        label.resize(cut == std::string::npos || cut == 0 ? max_chars : cut);
    }
    return label;
}

// A model-written title, held to the list's standard: first line only,
// wrapping quotes and trailing punctuation stripped, whitespace collapsed,
// capped at a word boundary. Empty when nothing survives - and an empty
// label means the list shows the date instead, so rejection is safe
inline std::string SanitiseLabel(std::string_view raw, std::size_t max_chars = 60) {
    const auto line_end = raw.find('\n');
    std::string label = LabelFrom(raw.substr(0, line_end), max_chars);
    while (!label.empty() && (label.front() == '"' || label.front() == '\'')) {
        label.erase(0, 1);
    }
    while (!label.empty() && (label.back() == '"' || label.back() == '\'' || label.back() == '.' ||
                              label.back() == ',' || label.back() == ':' || label.back() == ' ')) {
        label.pop_back();
    }
    return label;
}

}  // namespace sotto::core
