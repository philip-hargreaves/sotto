#include "adapters/translate/nllb_translator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openvino/openvino.hpp>
#include <stdexcept>
#include <thread>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace sotto::translate {

namespace {

constexpr std::size_t kMaxTokens = 512;

nlohmann::json LoadLanguages(const std::filesystem::path& dir) {
    std::ifstream in(dir / "languages.json");
    if (!in) {
        throw std::runtime_error("translation model has no languages.json");
    }
    return nlohmann::json::parse(in);
}

}  // namespace

struct NllbTranslator::Impl {
    Impl(const models::ModelStore& model_store, models::OvRuntime& ov)
        : store(model_store), runtime(ov) {}

    const models::ModelStore& store;
    models::OvRuntime& runtime;
    nlohmann::json languages;
    std::int64_t eos = 2;
    std::int64_t decoder_start = 2;
    std::mutex mutex;  // one translation at a time; guards the requests
    bool loaded = false;
    std::mutex prepare_mutex;  // guards the one warm thread
    std::thread loader;
    ov::Core core;  // tokenizer models need the tokenizers extension
    ov::InferRequest tokenizer;
    ov::InferRequest detokenizer;
    ov::InferRequest encoder;
    ov::InferRequest decoder;
    std::atomic<bool> cancel{false};

    void Load() {
        if (loaded) {
            return;
        }
        const models::ModelInfo& info = store.Resolve("translation", "default");
        encoder = runtime.Load(store, "translation", "default", "openvino_encoder_model.xml")
                      .model.create_infer_request();
        decoder = runtime.Load(store, "translation", "default", "openvino_decoder_model.xml")
                      .model.create_infer_request();
        core.add_extension("openvino_tokenizers.dll");
        tokenizer = core.compile_model((info.dir / "openvino_tokenizer.xml").string(), "CPU")
                        .create_infer_request();
        detokenizer = core.compile_model((info.dir / "openvino_detokenizer.xml").string(), "CPU")
                          .create_infer_request();
        loaded = true;
    }

    std::string Detokenize(const std::vector<std::int64_t>& ids) {
        ov::Tensor input(ov::element::i64, {1, ids.size()});
        std::copy(ids.begin(), ids.end(), input.data<std::int64_t>());
        detokenizer.set_input_tensor(input);
        detokenizer.infer();
        std::string text = detokenizer.get_output_tensor().data<std::string>()[0];
        // The sentencepiece word marker survives the detokenizer
        const std::string marker = "\xE2\x96\x81";
        std::size_t at = 0;
        while ((at = text.find(marker, at)) != std::string::npos) {
            text.replace(at, marker.size(), " ");
        }
        while (!text.empty() && text.front() == ' ') {
            text.erase(text.begin());
        }
        return text;
    }

    // NLLB is a sentence-level model: one line at a time keeps the sheet's
    // structure and its quality
    std::string TranslateLine(const std::string& line, std::int64_t target,
                              const std::function<void(const std::string&)>& partial = {}) {
        ov::Tensor input(ov::element::string, ov::Shape{1});
        input.data<std::string>()[0] = line;
        tokenizer.set_input_tensor(input);
        tokenizer.infer();
        const auto ids = tokenizer.get_tensor("input_ids");
        const auto mask = tokenizer.get_tensor("attention_mask");

        encoder.set_tensor("input_ids", ids);
        encoder.set_tensor("attention_mask", mask);
        encoder.infer();
        const auto hidden = encoder.get_output_tensor(0);

        decoder.reset_state();
        ov::Tensor beam(ov::element::i32, {1});
        beam.data<std::int32_t>()[0] = 0;
        // The first step carries the decoder start token and the forced
        // target language; the state carries everything after
        std::vector<std::int64_t> step = {decoder_start, target};
        std::vector<std::int64_t> generated;
        while (generated.size() < kMaxTokens && !cancel.load()) {
            ov::Tensor step_ids(ov::element::i64, {1, step.size()});
            std::copy(step.begin(), step.end(), step_ids.data<std::int64_t>());
            decoder.set_tensor("input_ids", step_ids);
            decoder.set_tensor("encoder_hidden_states", hidden);
            decoder.set_tensor("encoder_attention_mask", mask);
            decoder.set_tensor("beam_idx", beam);
            decoder.infer();

            const auto logits = decoder.get_tensor("logits");
            const auto& shape = logits.get_shape();  // [1, steps, vocab]
            const std::size_t vocab = shape[2];
            const float* last = logits.data<float>() + (shape[1] - 1) * vocab;
            auto token = static_cast<std::int64_t>(
                std::distance(last, std::max_element(last, last + vocab)));
            // Greedy decoding can lock onto one token; break the loop with
            // the runner-up
            const auto size = generated.size();
            if (size >= 2 && token == generated[size - 1] && token == generated[size - 2]) {
                std::vector<float> copy(last, last + vocab);
                copy[static_cast<std::size_t>(token)] = -1e9f;
                token = static_cast<std::int64_t>(
                    std::distance(copy.begin(), std::max_element(copy.begin(), copy.end())));
            }
            if (token == eos) {
                break;
            }
            generated.push_back(token);
            step = {token};
            if (partial) {
                partial(Detokenize(generated));
            }
        }
        return Detokenize(generated);
    }

    // NLLB is trained on single sentences and stops early on longer input,
    // so a line is translated one sentence at a time
    std::string TranslateSentences(const std::string& line, std::int64_t target,
                                   const std::function<void(const std::string&)>& partial = {}) {
        std::string out;
        std::size_t from = 0;
        while (from < line.size() && !cancel.load()) {
            std::size_t end = line.size();
            for (const char* mark : {". ", "? ", "! "}) {
                const auto at = line.find(mark, from);
                if (at != std::string::npos && at + 1 < end) {
                    end = at + 1;
                }
            }
            const std::string sentence = line.substr(from, end - from);
            if (sentence.find_first_not_of(" \t") != std::string::npos) {
                if (!out.empty()) {
                    out += ' ';
                }
                const std::string prefix = out;
                out += TranslateLine(sentence, target, [&](const std::string& text) {
                    if (partial) partial(prefix + text);
                });
            }
            from = end + (end < line.size() ? 1 : 0);
        }
        return out;
    }
};

NllbTranslator::NllbTranslator(const models::ModelStore& store, models::OvRuntime& runtime)
    : impl_(new Impl(store, runtime)) {
    const models::ModelInfo& info = store.Resolve("translation", "default");
    impl_->languages = LoadLanguages(info.dir);
    impl_->eos = impl_->languages.at("special").at("eos").get<std::int64_t>();
    impl_->decoder_start = impl_->languages.at("special").at("decoderStart").get<std::int64_t>();
}

NllbTranslator::~NllbTranslator() {
    std::thread loader;
    {
        std::lock_guard<std::mutex> lock(impl_->prepare_mutex);
        loader = std::move(impl_->loader);
    }
    if (loader.joinable()) {
        loader.join();
    }
}

// The warm must INFER: the CPU plugin's first-inference specialisation
// measured ~12 s. A failed warm is retried by the first Translate
void NllbTranslator::Prepare() {
    std::lock_guard<std::mutex> lock(impl_->prepare_mutex);
    if (impl_->loaded || impl_->loader.joinable()) {
        return;
    }
    impl_->loader = std::thread([impl = impl_.get()] {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->Load();
            const auto& languages = impl->languages.at("languages");
            const auto target = languages.begin().value().at("id").get<std::int64_t>();
            impl->TranslateSentences("Ready.", target);
            std::fprintf(
                stderr, "sotto-engine: translator warmed in %.1f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sotto-engine: translator warm failed (%s)\n", e.what());
        }
    });
}

std::vector<std::string> NllbTranslator::Languages() {
    std::vector<std::string> names;
    for (const auto& [name, entry] : impl_->languages.at("languages").items()) {
        names.push_back(name);
    }
    return names;
}

std::string NllbTranslator::Translate(const std::string& text, const std::string& language,
                                      const Progress& progress) {
    if (text.empty()) {
        throw std::runtime_error("nothing to translate");
    }
    const auto& languages = impl_->languages.at("languages");
    if (!languages.contains(language)) {
        throw std::runtime_error("unknown language: " + language);
    }
    const auto target = languages.at(language).at("id").get<std::int64_t>();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancel = false;
    const auto t0 = std::chrono::steady_clock::now();
    impl_->Load();

    std::string translated;
    std::size_t from = 0;
    while (from <= text.size() && !impl_->cancel.load()) {
        const auto end = text.find('\n', from);
        const std::string line =
            text.substr(from, end == std::string::npos ? std::string::npos : end - from);
        if (!translated.empty()) {
            translated += '\n';
        }
        if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos) {
            const std::string prefix = translated;
            translated += impl_->TranslateSentences(line, target, [&](const std::string& text) {
                if (progress) progress(prefix + text);
            });
        }
        if (progress) {
            progress(translated);
        }
        if (end == std::string::npos) {
            break;
        }
        from = end + 1;
    }

    std::fprintf(stderr, "sotto-engine: translated to %s in %.1f s\n", language.c_str(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return translated;
}

void NllbTranslator::Cancel() {
    impl_->cancel = true;
}

}  // namespace sotto::translate
