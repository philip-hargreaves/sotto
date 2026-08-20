#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>

#include "ports/translator.hpp"

namespace sotto::translate {

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

    // False while a previous translation is still running
    bool Run(std::string text, std::string language) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) {
            return false;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        running_ = true;
        worker_ = std::thread([this, text = std::move(text), language = std::move(language)] {
            try {
                const std::string translated =
                    translator_.Translate(text, language, [this](const std::string& partial) {
                        emit_("translate/partial", {{"text", partial}});
                    });
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

}  // namespace sotto::translate
