#include "adapters/note/qwen_note_writer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <openvino/genai/llm_pipeline.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

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
    std::filesystem::path prompt_dir;
    metrics::Registry* metrics;
    std::mutex swap_mutex;   // guards pipeline
    std::mutex state_mutex;  // guards loader, load_error, loading
    std::shared_ptr<ov::genai::LLMPipeline> pipeline;
    std::exception_ptr load_error;
    std::thread loader;
    std::atomic<bool> loading{false};
    std::atomic<bool> cancel{false};

    void Load() {
        const models::ModelInfo& info = store.Resolve("note", "default");
        store.Verify(info);
        const std::string device = runtime.ResolveDevice(info.device);
        const auto t0 = std::chrono::steady_clock::now();
        auto built = std::make_shared<ov::genai::LLMPipeline>(
            info.dir, device, ov::AnyMap{{"CACHE_DIR", (info.dir / ".cache").string()}});
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "sotto-engine: note on %s, loaded in %.1f s\n", device.c_str(),
                     seconds);
        if (metrics != nullptr) {
            metrics->RecordDevice("note", device);
            metrics->RecordLoad("note", seconds);
        }
        WarmPromptPrefix(*built);
        std::lock_guard<std::mutex> lock(swap_mutex);
        pipeline = std::move(built);
    }

    // One discarded token parks the instruction block's KV; only the transcript
    // prefills at stop (measured 2.1 -> 1.3 s). Notes equivalent, not byte-stable
    void WarmPromptPrefix(ov::genai::LLMPipeline& built) {
        try {
            const auto t0 = std::chrono::steady_clock::now();
            ov::genai::GenerationConfig config;
            config.max_new_tokens = 1;
            config.do_sample = false;
            config.apply_chat_template = false;
            built.generate("<|im_start|>user\n" + LoadPrompt(prompt_dir / "note-narrative.md"),
                           config);
            std::fprintf(
                stderr, "sotto-engine: note prefix warmed in %.1f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: note prefix warm failed (%s)\n", e.what());
        }
    }

    std::shared_ptr<ov::genai::LLMPipeline> Pipeline() {
        std::lock_guard<std::mutex> lock(swap_mutex);
        return pipeline;
    }

    void JoinLoader() {
        std::thread finished;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            finished = std::move(loader);
        }
        if (finished.joinable()) {
            finished.join();
        }
    }
};

QwenNoteWriter::QwenNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                               std::filesystem::path prompt_dir, metrics::Registry* metrics)
    : impl_(new Impl{store, runtime, std::move(prompt_dir), metrics}) {}

QwenNoteWriter::~QwenNoteWriter() {
    impl_->JoinLoader();
}

// Starts the one background load; the ~14 s cost lands during capture, not
// on the stop path. A failed attempt is retried on the next call.
void QwenNoteWriter::Prepare() {
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->loading.load() || impl_->Pipeline() != nullptr) {
            return;
        }
        finished = std::move(impl_->loader);
        impl_->load_error = nullptr;
        impl_->loading = true;
        impl_->loader = std::thread([impl = impl_.get()] {
            try {
                impl->Load();
            } catch (...) {
                std::lock_guard<std::mutex> lock(impl->state_mutex);
                impl->load_error = std::current_exception();
            }
            impl->loading = false;
        });
    }
    if (finished.joinable()) {
        finished.join();
    }
}

std::string QwenNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                  const NoteOptions& options, const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    const auto style = options.style == "soap" ? "note-soap.md" : "note-narrative.md";
    return Generate(LoadPrompt(impl_->prompt_dir / style) + TranscriptBlock(transcript) + "\n" +
                        LoadPrompt(impl_->prompt_dir / ("detail-" + options.detail + ".md")),
                    progress);
}

std::string QwenNoteWriter::WritePatient(const std::string& note, const Progress& progress) {
    if (note.empty()) {
        throw std::runtime_error("nothing to write: the note is empty");
    }
    return Generate(LoadPrompt(impl_->prompt_dir / "patient-info.md") + note + "\n", progress);
}

std::string QwenNoteWriter::WriteLabel(const std::string& note) {
    if (note.empty()) {
        return {};
    }
    return Generate(LoadPrompt(impl_->prompt_dir / "label.md") + note + "\n", nullptr, 16);
}

std::string QwenNoteWriter::Generate(const std::string& prompt, const Progress& progress,
                                     std::size_t max_new_tokens) {
    impl_->cancel = false;
    Prepare();
    impl_->JoinLoader();
    // A strong reference for the whole generation: a swap or teardown can
    // never free the model under an in-flight call
    const auto pipeline = impl_->Pipeline();
    if (pipeline == nullptr) {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->load_error != nullptr) {
            std::rethrow_exception(impl_->load_error);
        }
        throw std::runtime_error("note model unavailable");
    }

    ov::genai::GenerationConfig config;
    config.max_new_tokens = max_new_tokens;
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
    pipeline->generate(wrapped, config, streamer);
    return Trimmed(text);
}

void QwenNoteWriter::Cancel() {
    impl_->cancel = true;
}

}  // namespace sotto::note
