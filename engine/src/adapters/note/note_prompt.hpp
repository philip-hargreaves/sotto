#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

namespace sotto::note {

// Read fresh every note, so a prompt edit applies to the next generation
inline std::string LoadPrompt(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("note prompt missing: " + path.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// SPEAKER: text lines, the shape the prompt was tuned on
inline std::string TranscriptBlock(const std::vector<asr::Turn>& turns) {
    std::string block;
    for (const auto& turn : turns) {
        std::string role = turn.speaker.empty() ? "speaker" : turn.speaker;
        for (char& c : role) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        block += role + ": " + turn.text + "\n";
    }
    return block;
}

}  // namespace sotto::note
