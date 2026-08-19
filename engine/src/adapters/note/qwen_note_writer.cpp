#include "adapters/note/qwen_note_writer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <openvino/genai/llm_pipeline.hpp>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "adapters/note/note_prompt.hpp"
#include "core/metrics.hpp"

namespace sotto::note {

namespace {

std::string Trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \n\r\t");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \n\r\t") - begin + 1);
}

}  // namespace

struct QwenNoteWriter::Impl {
    const models::ModelStore& store;
    models::OvRuntime& runtime;
    std::filesystem::path prompt_path;
    metrics::Registry* metrics;
    std::unique_ptr<ov::genai::LLMPipeline> pipeline;
    std::atomic<bool> cancel{false};
    std::atomic<bool> prefetching{false};
    std::thread prefetch;
};

QwenNoteWriter::QwenNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                               std::filesystem::path prompt_path, metrics::Registry* metrics)
    : impl_(new Impl{store, runtime, std::move(prompt_path), metrics, nullptr, {}, {}, {}}) {}

QwenNoteWriter::~QwenNoteWriter() {
    if (impl_->prefetch.joinable()) {
        impl_->prefetch.join();
    }
}

void QwenNoteWriter::Prepare() {
    if (impl_->prefetching.exchange(true)) {
        return;  // one prefetch at a time
    }
    if (impl_->prefetch.joinable()) {
        impl_->prefetch.join();
    }
    impl_->prefetch = std::thread([impl = impl_.get()] {
        try {
            const models::ModelInfo& info = impl->store.Resolve("note", "default");
            std::vector<char> buffer(std::size_t{1} << 20);
            for (const auto& entry : std::filesystem::directory_iterator(info.dir)) {
                if (!entry.is_regular_file()) continue;
                std::ifstream in(entry.path(), std::ios::binary);
                while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
                }
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
        impl->prefetching = false;
    });
}

std::string QwenNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                  const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    impl_->cancel = false;
    const std::string prompt = LoadPrompt(impl_->prompt_path) + TranscriptBlock(transcript);

    // Loaded per note and freed after: an idle resident model through a
    // real-time session measured fatal (GPU demotion), as did cohabiting
    // with Whisper during generation
    {
        const models::ModelInfo& info = impl_->store.Resolve("note", "default");
        impl_->store.Verify(info);
        const std::string device = impl_->runtime.ResolveDevice(info.device);
        const auto t0 = std::chrono::steady_clock::now();
        impl_->pipeline = std::make_unique<ov::genai::LLMPipeline>(
            info.dir, device, ov::AnyMap{{"CACHE_DIR", (info.dir / ".cache").string()}});
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "sotto-engine: note on %s, loaded in %.1f s\n", device.c_str(),
                     seconds);
        if (impl_->metrics != nullptr) {
            impl_->metrics->RecordDevice("note", device);
            impl_->metrics->RecordLoad("note", seconds);
        }
    }

    ov::genai::GenerationConfig config;
    config.max_new_tokens = 1024;
    config.do_sample = false;
    config.apply_chat_template = false;
    // The template the prompt was tuned with: user turn, empty think block
    const std::string wrapped = "<|im_start|>user\n" + prompt +
                                "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";

    // The streamer carries both the partials out and the cancel in; on
    // cancel the accumulated text is returned as-is
    std::string text;
    const auto streamer = [this, &text, &progress](std::string piece) {
        if (impl_->cancel.load()) {
            return ov::genai::StreamingStatus::STOP;
        }
        text += piece;
        if (progress) {
            progress(text);
        }
        return ov::genai::StreamingStatus::RUNNING;
    };
    try {
        impl_->pipeline->generate(wrapped, config, streamer);
    } catch (...) {
        impl_->pipeline.reset();
        throw;
    }
    impl_->pipeline.reset();
    return Trimmed(text);
}

void QwenNoteWriter::Cancel() {
    impl_->cancel = true;
}

}  // namespace sotto::note
