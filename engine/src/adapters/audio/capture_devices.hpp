#pragma once

#include <string>
#include <vector>

namespace sotto::audio {

// One active capture endpoint, as the settings picker shows it. The id is
// WASAPI's, so it is exactly what WasapiCapture pins when passed back
struct CaptureDevice {
    std::string id;
    std::string name;         // "Microphone Array (Realtek(R) Audio)"
    std::string short_name;   // "Microphone Array"
    bool is_default = false;  // the communications default, the pin fallback
    bool bluetooth = false;   // the hands-free quality-cliff warning's signal
};

// Enumerated fresh per call: a headset plugged in after launch must appear
std::vector<CaptureDevice> ListCaptureDevices();

// The picker's saved id against what exists right now. A choice that is
// gone resolves to the default - the caller logs that - and an empty list
// resolves to nothing, which capture then fails loudly
CaptureDevice ResolveMicrophone(const std::vector<CaptureDevice>& devices,
                                const std::string& requested);

// WASAPI wants the endpoint id back in its native encoding
std::wstring WideId(const std::string& id);

}  // namespace sotto::audio
