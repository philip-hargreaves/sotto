#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

// EcoQoS throttling measured finalise 4.0 -> 6.3 s; engine, note host and
// shell all opt out, so the state never depends on one call
namespace ambient::host {

struct ThrottlingState {
    bool known = false;      // read-back succeeded
    bool throttled = false;  // execution speed throttling is ON
    bool defaulted = true;   // no explicit policy: Windows decides
};

inline ThrottlingState ReadThrottling(HANDLE process) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    ThrottlingState out;
    if (!GetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state))) {
        return out;
    }
    out.known = true;
    out.defaulted = (state.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) == 0;
    out.throttled = (state.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    return out;
}

// Clears execution-speed throttling; returns the state read back afterwards
inline ThrottlingState DisableThrottling(HANDLE process) {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
    return ReadThrottling(process);
}

inline ThrottlingState DisableThrottlingOnSelf() {
    return DisableThrottling(GetCurrentProcess());
}

// "off" (opted out), "on" (throttled), "default" (Windows decides), "unknown"
inline std::string Describe(const ThrottlingState& state) {
    if (!state.known) return "unknown";
    if (!state.defaulted) return state.throttled ? "on" : "off";
    return "default";
}

}  // namespace ambient::host
