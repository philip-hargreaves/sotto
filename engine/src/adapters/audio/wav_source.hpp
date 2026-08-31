#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ports/audio_source.hpp"

namespace sotto::audio {

// Plays a mono 16 kHz wav through the audio port; the CI microphone
// stand-in and the replay source
class WavSource : public IAudioSource {
   public:
    struct Config {
        double speed = 0.0;             // 1 = real time, >1 faster, 0 = as fast as possible
        bool monitor = false;           // also play aloud, decimated by speed
        std::uint64_t start_frame = 0;  // resume: skip audio already captured
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
