#pragma once

#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

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
