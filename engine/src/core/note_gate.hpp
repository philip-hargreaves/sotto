#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ambient::note {

// The note model's first line when the transcript is not a healthcare
// consultation; the engine reports a refusal instead of saving a note
inline constexpr std::string_view kNotAConsultation = "NOT A CONSULTATION:";

inline std::string_view TrimmedView(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r' ||
                             text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' ||
                             text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// The reason after the sentinel when the note is a refusal, else nothing
inline std::optional<std::string> RefusalReason(std::string_view note) {
    const auto text = TrimmedView(note);
    if (text.substr(0, kNotAConsultation.size()) != kNotAConsultation) return std::nullopt;
    auto reason = text.substr(kNotAConsultation.size());
    if (const auto line_end = reason.find('\n'); line_end != std::string_view::npos) {
        reason = reason.substr(0, line_end);
    }
    return std::string(TrimmedView(reason));
}

// Holds the streamed note back until its opening shows it is not a refusal,
// so a refusal never flashes on screen as if it were the note
class RefusalFilter {
   public:
    explicit RefusalFilter(std::function<void(const std::string&)> forward)
        : forward_(std::move(forward)) {}

    // Cumulative text so far, as the writer streams it
    void operator()(const std::string& text) {
        if (refused_) return;
        if (!decided_) {
            const auto opening = TrimmedView(text);
            const auto n = std::min(opening.size(), kNotAConsultation.size());
            if (opening.substr(0, n) == kNotAConsultation.substr(0, n)) {
                if (opening.size() < kNotAConsultation.size()) return;  // still could be
                refused_ = true;
                return;
            }
            decided_ = true;
        }
        forward_(text);
    }

    bool Refused() const {
        return refused_;
    }

   private:
    std::function<void(const std::string&)> forward_;
    bool decided_ = false;
    bool refused_ = false;
};

}  // namespace ambient::note
