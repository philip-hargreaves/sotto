#pragma once

#include <memory>
#include <span>

namespace ambient::audio {

// Best-effort playback to the default render endpoint, 16 kHz mono float.
// Write drops rather than waits, so playback never throttles the pipeline feed
class WasapiPlayer {
   public:
    // nullptr when there is no render endpoint; the caller stays silent
    static std::unique_ptr<WasapiPlayer> Open();
    ~WasapiPlayer();
    WasapiPlayer(const WasapiPlayer&) = delete;
    WasapiPlayer& operator=(const WasapiPlayer&) = delete;

    void Write(std::span<const float> frames);

   private:
    struct Impl;
    explicit WasapiPlayer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ambient::audio
