#pragma once

#include <atomic>
#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// Plays a wav file through the audio port: PCM16 or float32, mono, 16 kHz,
// anything else refused. The CI stand-in for a microphone, and the future
// replay harness.
class WavSource : public IAudioSource {
   public:
    explicit WavSource(std::string path);

    void Run(IAudioSink& sink) override;
    void RequestStop() override;

   private:
    SourceEnd RunToEnd(IAudioSink& sink);

    std::string path_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace sotto::audio
