#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <span>
#include <vector>

namespace sotto::audio {

// Single-producer single-consumer ring of float frames, in the Lamport style:
// the producer owns write_, the consumer owns read_, and each acquires the
// other's index once per call. Indices grow monotonically and are masked on
// access, so capacity is rounded up to a power of two.
#pragma warning(push)
#pragma warning(disable : 4324)  // Padding from alignas is the point
class AudioRing {
   public:
    explicit AudioRing(std::size_t min_capacity)
        : capacity_(std::bit_ceil(min_capacity)), mask_(capacity_ - 1), buffer_(capacity_) {}

    std::size_t Capacity() const {
        return capacity_;
    }

    // Producer only. Returns the frames written; short of frames.size() means
    // the ring is full and the caller decides what an overrun means.
    std::size_t TryPush(std::span<const float> frames) {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t read = read_.load(std::memory_order_acquire);
        const std::size_t free = capacity_ - (write - read);
        const std::size_t count = std::min(frames.size(), free);
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[(write + i) & mask_] = frames[i];
        }
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    // Consumer only. Fills out from the front of the ring, returns frames read.
    std::size_t TryPop(std::span<float> out) {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        const std::size_t write = write_.load(std::memory_order_acquire);
        const std::size_t available = write - read;
        const std::size_t count = std::min(out.size(), available);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = buffer_[(read + i) & mask_];
        }
        read_.store(read + count, std::memory_order_release);
        return count;
    }

   private:
    std::size_t capacity_;
    std::size_t mask_;
    std::vector<float> buffer_;
    // On separate cache lines so the two sides never false-share
    alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> write_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<std::size_t> read_{0};
};
#pragma warning(pop)

}  // namespace sotto::audio
