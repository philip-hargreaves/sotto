#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// The audio-client codes this adapter distinguishes. Values are ABI-frozen;
// the test asserts each against the SDK's own macros.
namespace hresults {
inline constexpr std::uint32_t kDeviceInvalidated = 0x88890004;
inline constexpr std::uint32_t kServiceNotRunning = 0x88890010;
inline constexpr std::uint32_t kEndpointCreateFailed = 0x8889000F;
inline constexpr std::uint32_t kResourcesInvalidated = 0x88890026;
inline constexpr std::uint32_t kAccessDenied = 0x80070005;
}  // namespace hresults

// Maps a failed call to the port's end reason. The four device-gone codes
// are kDeviceLost (ENDPOINT_CREATE_FAILED shares DEVICE_INVALIDATED's cause
// wording, so the documented surface cannot split them); denial names the
// privacy settings; everything else carries the call and code verbatim.
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
