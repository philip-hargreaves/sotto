#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace sotto::models {

// Builds T on a background thread; Get waits and rethrows a load failure
template <typename T>
class DeferredLoad {
   public:
    DeferredLoad(std::string name, std::function<std::unique_ptr<T>()> build)
        : name_(std::move(name)) {
        loader_ = std::thread([this, build = std::move(build)] {
            const auto t0 = std::chrono::steady_clock::now();
            try {
                built_ = build();
                std::fprintf(
                    stderr, "sotto-engine: %s ready in %.1f s\n", name_.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "sotto-engine: %s unavailable (%s)\n", name_.c_str(),
                             e.what());
                error_ = std::current_exception();
            } catch (...) {
                error_ = std::current_exception();
            }
            ready_.store(true);
        });
    }

    ~DeferredLoad() {
        if (loader_.joinable()) {
            loader_.join();
        }
    }

    T& Get() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (loader_.joinable()) {
                loader_.join();
            }
        }
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
        return *built_;
    }

    // True only for a successful build; never blocks
    bool Loaded() const {
        return ready_.load() && error_ == nullptr;
    }

   private:
    std::string name_;
    std::unique_ptr<T> built_;
    std::exception_ptr error_;
    std::atomic<bool> ready_{false};
    std::mutex mutex_;
    std::thread loader_;
};

}  // namespace sotto::models
