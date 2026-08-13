#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

namespace hresults {
inline constexpr std::uint32_t kDeviceInvalidated = 0x88890004;
inline constexpr std::uint32_t kServiceNotRunning = 0x88890010;
inline constexpr std::uint32_t kEndpointCreateFailed = 0x8889000F;
inline constexpr std::uint32_t kResourcesInvalidated = 0x88890026;
inline constexpr std::uint32_t kAccessDenied = 0x80070005;
}  // namespace hresults

// Maps a failed call to the port's end reason
inline SourceEnd EndForCaptureError(const char* call, std::uint32_t hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", hr);

    switch (hr) {
        case hresults::kDeviceInvalidated:
        case hresults::kServiceNotRunning:
        case hresults::kEndpointCreateFailed:
        case hresults::kResourcesInvalidated:
            return {SourceEndReason::kDeviceLost, std::string(call) + " reported " + code};
        case hresults::kAccessDenied:
            return {SourceEndReason::kFailed,
                    "microphone access denied (Windows privacy settings or policy), " +
                        std::string(call) + " reported " + code};
        default:
            return {SourceEndReason::kFailed, std::string(call) + " failed, hr=" + code};
    }
}

}  // namespace sotto::audio
