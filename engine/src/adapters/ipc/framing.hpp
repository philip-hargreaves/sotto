#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ambient::ipc {

// Pipe imposes no size limit; capped to prevent unbounded allocation
inline constexpr std::uint32_t kMaxFrameBytes = 4u * 1024 * 1024;

inline constexpr std::size_t kHeaderBytes = 4;

// 4-byte little-endian payload length, then the payload.
inline std::string EncodeFrame(std::string_view payload) {
    if (payload.size() > kMaxFrameBytes) {
        throw std::length_error("frame exceeds kMaxFrameBytes");
    }
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.reserve(kHeaderBytes + payload.size());
    frame.push_back(static_cast<char>(len & 0xFF));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>((len >> 16) & 0xFF));
    frame.push_back(static_cast<char>((len >> 24) & 0xFF));
    frame.append(payload);
    return frame;
}

// Reassembles length-prefixed frames from a byte stream fed in arbitrary chunks.
class FrameDecoder {
   public:
    void Push(std::string_view bytes) {
        if (failed_) return;
        buffer_.append(bytes);
    }

    std::optional<std::string> Next() {
        if (failed_ || buffer_.size() < kHeaderBytes) return std::nullopt;
        const auto b = [this](std::size_t i) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[i]));
        };
        const std::uint32_t len = b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
        if (len > kMaxFrameBytes) {
            failed_ = true;
            buffer_.clear();
            return std::nullopt;
        }
        if (buffer_.size() < kHeaderBytes + len) return std::nullopt;
        std::string payload = buffer_.substr(kHeaderBytes, len);
        buffer_.erase(0, kHeaderBytes + len);
        return payload;
    }

    bool failed() const {
        return failed_;
    }

   private:
    std::string buffer_;
    bool failed_ = false;
};

}  // namespace ambient::ipc
