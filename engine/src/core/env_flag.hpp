#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace ambient {

// Pipeline switches are environment variables, so one binary carries every
// evaluation arm. The single-decode pipeline is the
// product: its switches default on and "0" turns one off. Others are off unless set
inline const char* DefaultValue(const char* name) {
    static constexpr std::pair<const char*, const char*> kDefaults[] = {
        {"AMBIENT_DIAR_SEG_CUTS_ONLY", "1"},  // diarisation from segment cuts, no live windows
        {"AMBIENT_SEG_FRONTIER", "1"},        // turns close at the settled frontier
        {"AMBIENT_NO_LIVE_ASR", "1"},         // one decode per turn, no live pass
        {"AMBIENT_NOTE_PREFILL", "1"},        // note prompt prefilled during capture
        {"AMBIENT_CLIP_CUTS", "snap"},        // Whisper chunk edges as cuts, snapped to pauses
        {"AMBIENT_RESPLIT", "1"},             // merged turns re-split by embedding at seal
        {"AMBIENT_TIDY", "1"},                // professional-transcript tidy at seal
        {"AMBIENT_CHUNK_ASSEMBLE", "1"},      // cut pieces take the parent decode's chunks
    };
    for (const auto& [key, value] : kDefaults) {
        if (std::strcmp(key, name) == 0) return value;
    }
    return nullptr;
}

inline std::string EnvValue(const char* name) {
#pragma warning(suppress : 4996)
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        const char* fallback = DefaultValue(name);
        return fallback != nullptr ? std::string(fallback) : std::string();
    }
    return std::string(value);
}

inline bool EnvFlag(const char* name) {
    const std::string value = EnvValue(name);
    return !value.empty() && value != "0";
}

}  // namespace ambient
