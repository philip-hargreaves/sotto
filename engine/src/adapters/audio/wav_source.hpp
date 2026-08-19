#pragma once

#include <atomic>
#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// Plays a wav file through the audio port: PCM16 or float32, mono, 16 kHz,
// anything else refused. The CI stand-in for a microphone, and the replay
// source for the demo tray.
class WavSource : public IAudioSource {
   public:
    struct Config {
        double speed = 0.0;    // 1 = real time, >1 faster, 0 = as fast as possible
        bool monitor = false;  // also play aloud, decimated by speed
    };

    explicit WavSource(std::string path, Config config = {});

    void Run(IAudioSink& sink) override;
    void RequestStop() override;
    void SetPaused(bool paused) override;
    void SetMonitor(bool monitor) override;

   private:
    SourceEnd RunToEnd(IAudioSink& sink);

    std::string path_;
    Config config_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> monitor_{false};
};

}  // namespace sotto::audio
