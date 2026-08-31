#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>

#include "ports/translator.hpp"

namespace ambient::translate {

// Runs one translation at a time off the RPC thread, announcing
// translate/partial, translate/ready and translate/failed.
class TranslateLane {
   public:
    using Emit = std::function<void(const std::string& method, const nlohmann::json& params)>;

    TranslateLane(ITranslator& translator, Emit emit)
        : translator_(translator), emit_(std::move(emit)) {}

    ~TranslateLane() {
        translator_.Cancel();
        Join();
    }

    using OnReady = std::function<void(const std::string& translated, const std::string& language)>;

    // on_ready runs before the ready notification, so a reader reacting to it
    // finds the translation stored
    bool Run(std::string text, std::string language, OnReady on_ready = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) {
            return false;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        running_ = true;
        worker_ = std::thread([this, text = std::move(text), language = std::move(language),
                               on_ready = std::move(on_ready)] {
            try {
                const auto t0 = std::chrono::steady_clock::now();
                bool first = true;
                const std::string translated = translator_.Translate(
                    text, language, [this, &t0, &first](const std::string& partial) {
                        if (first) {
                            first = false;
                            std::fprintf(
                                stderr, "ambient-engine: first translated word in %.1f s\n",
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                                    .count());
                        }
                        emit_("translate/partial", {{"text", partial}});
                    });
                if (on_ready) {
                    on_ready(translated, language);
                }
                emit_("translate/ready", {{"text", translated}, {"language", language}});
            } catch (const std::exception& e) {
                emit_("translate/failed", {{"detail", e.what()}});
            } catch (...) {
                emit_("translate/failed", {{"detail", "translation failed"}});
            }
            running_ = false;
        });
        return true;
    }

   private:
    void Join() {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker = std::move(worker_);
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    ITranslator& translator_;
    Emit emit_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

}  // namespace ambient::translate
