#pragma once

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ambient::host {

// AMBIENT_NOTE_PREFILL: one named mutex serialises the engine's Whisper decodes
// against the note host's capture-phase prefills, so the two GPU models never run
// concurrently (the driver fault configuration). Named by
// AMBIENT_GPU_LEASE, which the engine sets and the host inherits; inert when unset
class GpuLease {
   public:
    class Guard {
       public:
        Guard() = default;
        Guard(HANDLE mutex, double waited) : mutex_(mutex), waited_(waited) {}
        Guard(Guard&& other) noexcept
            : mutex_(std::exchange(other.mutex_, nullptr)), waited_(other.waited_) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                Release();
                mutex_ = std::exchange(other.mutex_, nullptr);
                waited_ = other.waited_;
            }
            return *this;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() {
            Release();
        }

        // Seconds spent waiting for the other process to finish
        double waited() const {
            return waited_;
        }

       private:
        void Release() {
            if (mutex_ != nullptr) {
                ReleaseMutex(mutex_);
                mutex_ = nullptr;
            }
        }
        HANDLE mutex_ = nullptr;
        double waited_ = 0.0;
    };

    explicit GpuLease(const std::string& name) {
        if (!name.empty()) {
            mutex_ = CreateMutexA(nullptr, FALSE, name.c_str());
        }
    }
    ~GpuLease() {
        if (mutex_ != nullptr) CloseHandle(mutex_);
    }
    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    static GpuLease& Global() {
        static GpuLease lease([] {
#pragma warning(suppress : 4996)
            const char* name = std::getenv("AMBIENT_GPU_LEASE");
            return std::string(name != nullptr ? name : "");
        }());
        return lease;
    }

    bool Active() const {
        return mutex_ != nullptr;
    }

    // Blocks until the GPU is ours; an abandoned mutex (the other process died
    // mid-decode) counts as acquired
    Guard Acquire() {
        if (mutex_ == nullptr) return {};
        const auto t0 = std::chrono::steady_clock::now();
        const DWORD result = WaitForSingleObject(mutex_, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) return {};
        return {mutex_,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()};
    }

   private:
    HANDLE mutex_ = nullptr;
};

}  // namespace ambient::host
