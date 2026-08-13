#pragma once

#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// Captures the microphone through WASAPI: event-driven shared mode, the
// system converter to 16 kHz mono float32, every queued packet drained per
// wake. The endpoint is resolved once and pinned, so a default-device change
// mid-session is deliberately not followed. One Run per instance; recovery
// after a device loss is a fresh instance, never a retry on this one.
class WasapiCapture : public IAudioSource {
   public:
    // Empty id resolves the default communications microphone at Run
    explicit WasapiCapture(std::wstring endpoint_id = {});
    ~WasapiCapture() override;
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    void Run(IAudioSink& sink) override;
    void RequestStop() override;

   private:
    SourceEnd RunToEnd(IAudioSink& sink);

    std::wstring endpoint_id_;
    void* stop_event_ = nullptr;  // HANDLE, manual-reset: stopping is terminal
};

}  // namespace sotto::audio
