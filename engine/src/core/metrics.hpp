#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace ambient::metrics {

// One engine-wide sink; any thread records, readers get a copy. Devices
// and load times persist, the rest resets per session
class Registry {
   public:
    struct Snapshot {
        std::map<std::string, std::string> devices;
        std::map<std::string, double> load_seconds;
        std::map<std::string, double> stage_seconds;
        double decoded_audio_seconds = 0;
        double decode_busy_seconds = 0;
        double session_audio_seconds = 0;
        std::uint64_t lost_frames = 0;
        int diar_ticks = 0;
        int turns = -1;
        int clusters = -1;
        bool replay = false;
        double replay_speed = 0;
    };

    void RecordDevice(const std::string& role, const std::string& device) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.devices[role] = device;
    }

    void RecordLoad(const std::string& role, double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.load_seconds[role] = seconds;
    }

    void RecordStage(const std::string& stage, double seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.stage_seconds[stage] = seconds;
    }

    void RecordDecode(double audio_seconds, double busy_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.decoded_audio_seconds += audio_seconds;
        data_.decode_busy_seconds += busy_seconds;
    }

    void RecordSession(double audio_seconds, std::uint64_t lost_frames, int diar_ticks) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.session_audio_seconds = audio_seconds;
        data_.lost_frames = lost_frames;
        data_.diar_ticks = diar_ticks;
    }

    void RecordTranscript(int turns, int clusters) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.turns = turns;
        data_.clusters = clusters;
    }

    void BeginSession(bool replay, double replay_speed) {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot next;
        next.devices = std::move(data_.devices);
        next.load_seconds = std::move(data_.load_seconds);
        next.replay = replay;
        next.replay_speed = replay_speed;
        data_ = std::move(next);
    }

    Snapshot Take() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }

   private:
    mutable std::mutex mutex_;
    Snapshot data_;
};

}  // namespace ambient::metrics
