#include "adapters/audio/wav_source.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "adapters/audio/wasapi_player.hpp"

namespace sotto::audio {

namespace {

constexpr std::size_t kPacketFrames = 480;  // 30 ms, a realistic capture cadence

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;

struct WavFormat {
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t byte_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
};

struct WavHeader {
    WavFormat format;
    std::uint32_t data_bytes = 0;
};

bool ReadExact(std::ifstream& in, void* out, std::size_t bytes) {
    return static_cast<bool>(in.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes)));
}

template <typename T>
bool ReadValue(std::ifstream& in, T& out) {
    return ReadExact(in, &out, sizeof(T));
}

bool TagIs(const std::array<char, 4>& tag, const char* expected) {
    return std::memcmp(tag.data(), expected, 4) == 0;
}

// Walks the chunk list to fmt and data; skips anything else, honouring the
// pad byte after every odd-sized chunk
std::variant<WavHeader, std::string> ParseHeader(std::ifstream& in) {
    std::array<char, 4> tag{};
    std::uint32_t riff_bytes = 0;
    if (!ReadExact(in, tag.data(), 4) || !TagIs(tag, "RIFF")) {
        return std::string("not a RIFF file");
    }
    if (!ReadValue(in, riff_bytes) || !ReadExact(in, tag.data(), 4) || !TagIs(tag, "WAVE")) {
        return std::string("not a WAVE file");
    }

    WavFormat format;
    bool have_format = false;
    for (;;) {
        std::uint32_t chunk_bytes = 0;
        if (!ReadExact(in, tag.data(), 4) || !ReadValue(in, chunk_bytes)) {
            return std::string("no data chunk");
        }
        if (TagIs(tag, "fmt ")) {
            if (chunk_bytes < 16 || !ReadValue(in, format.format) ||
                !ReadValue(in, format.channels) || !ReadValue(in, format.sample_rate) ||
                !ReadValue(in, format.byte_rate) || !ReadValue(in, format.block_align) ||
                !ReadValue(in, format.bits_per_sample)) {
                return std::string("malformed fmt chunk");
            }
            have_format = true;
            in.seekg(static_cast<std::streamoff>(chunk_bytes - 16 + (chunk_bytes & 1)),
                     std::ios::cur);
        } else if (TagIs(tag, "data")) {
            if (!have_format) {
                return std::string("data chunk before fmt");
            }
            return WavHeader{format, chunk_bytes};
        } else {
            in.seekg(static_cast<std::streamoff>(chunk_bytes + (chunk_bytes & 1)), std::ios::cur);
        }
        if (!in) {
            return std::string("truncated chunk list");
        }
    }
}

std::string Refusal(const WavFormat& format) {
    const bool pcm16 = format.format == kFormatPcm && format.bits_per_sample == 16;
    const bool float32 = format.format == kFormatFloat && format.bits_per_sample == 32;
    if (!pcm16 && !float32) {
        return "only PCM16 and float32 are supported (format " + std::to_string(format.format) +
               ", " + std::to_string(format.bits_per_sample) + " bit)";
    }
    if (format.channels != 1) {
        return "expected mono, got " + std::to_string(format.channels) + " channels";
    }
    if (format.sample_rate != static_cast<std::uint32_t>(kSampleRate)) {
        return "expected 16000 Hz, got " + std::to_string(format.sample_rate) + " Hz";
    }
    return "";
}

}  // namespace

WavSource::WavSource(std::string path, Config config) : path_(std::move(path)), config_(config) {
    monitor_.store(config.monitor, std::memory_order_relaxed);
}

void WavSource::RequestStop() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

void WavSource::SetPaused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

void WavSource::SetMonitor(bool monitor) {
    monitor_.store(monitor, std::memory_order_relaxed);
}

// OnEnd is the port's one guarantee, so Run funnels every outcome through it
void WavSource::Run(IAudioSink& sink) {
    sink.OnEnd(RunToEnd(sink));
}

SourceEnd WavSource::RunToEnd(IAudioSink& sink) {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return {SourceEndReason::kFailed, "cannot open " + path_};
    }

    auto parsed = ParseHeader(in);
    if (std::holds_alternative<std::string>(parsed)) {
        return {SourceEndReason::kFailed, std::get<std::string>(parsed)};
    }
    const auto& header = std::get<WavHeader>(parsed);
    if (auto refusal = Refusal(header.format); !refusal.empty()) {
        return {SourceEndReason::kFailed, refusal};
    }

    const std::size_t bytes_per_frame = header.format.bits_per_sample / 8u;
    if (header.data_bytes % bytes_per_frame != 0) {
        return {SourceEndReason::kFailed, "data chunk is not whole frames"};
    }

    std::size_t remaining = header.data_bytes / bytes_per_frame;
    std::vector<std::int16_t> pcm(kPacketFrames);
    std::vector<float> frames(kPacketFrames);
    std::vector<float> decimated;
    std::unique_ptr<WasapiPlayer> player;
    bool player_failed = false;
    // Decimate by speed: the tape-speed effect, no exotic sample rate needed
    const std::size_t step =
        config_.speed > 1.0 ? static_cast<std::size_t>(config_.speed + 0.5) : 1;

    using Clock = std::chrono::steady_clock;
    auto origin = Clock::now();
    std::uint64_t frames_sent = 0;
    while (remaining > 0) {
        if (stop_requested_.load(std::memory_order_relaxed)) {
            return {SourceEndReason::kStopped, ""};
        }
        // Hold the packet while paused, never drop it - the downstream
        // clock is delivered audio. Stop still wins
        const bool was_paused = paused_.load(std::memory_order_relaxed);
        while (paused_.load(std::memory_order_relaxed) &&
               !stop_requested_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stop_requested_.load(std::memory_order_relaxed)) {
            return {SourceEndReason::kStopped, ""};
        }
        // Re-base after a pause: catching up would deliver a burst no
        // microphone produces
        if (was_paused) {
            origin = Clock::now();
            frames_sent = 0;
        }

        const std::size_t count = std::min(kPacketFrames, remaining);
        if (header.format.format == kFormatPcm) {
            if (!ReadExact(in, pcm.data(), count * sizeof(std::int16_t))) {
                return {SourceEndReason::kFailed, "data chunk truncated"};
            }
            for (std::size_t i = 0; i < count; ++i) {
                frames[i] = static_cast<float>(pcm[i]) / 32768.0F;
            }
        } else {
            if (!ReadExact(in, frames.data(), count * sizeof(float))) {
                return {SourceEndReason::kFailed, "data chunk truncated"};
            }
        }
        sink.OnAudio(std::span<const float>(frames.data(), count), 0);
        // After the sink and best-effort, so playback never throttles the
        // feed; the toggle opens and closes the device mid-stream
        const bool monitor = monitor_.load(std::memory_order_relaxed);
        if (monitor && !player && !player_failed) {
            player = WasapiPlayer::Open();
            player_failed = player == nullptr;
        } else if (!monitor && player) {
            player.reset();
        }
        if (player && monitor) {
            decimated.clear();
            for (std::size_t i = 0; i < count; i += step) decimated.push_back(frames[i]);
            player->Write(decimated);
        }
        remaining -= count;
        // Sleep to a deadline from a fixed origin, never for a duration -
        // per-packet sleeps drift, and the drift compounds
        if (config_.speed > 0) {
            frames_sent += count;
            const auto due = static_cast<std::int64_t>(static_cast<double>(frames_sent) * 1e6 /
                                                       (kSampleRate * config_.speed));
            std::this_thread::sleep_until(origin + std::chrono::microseconds(due));
        }
    }
    return {SourceEndReason::kCompleted, ""};
}

}  // namespace sotto::audio
