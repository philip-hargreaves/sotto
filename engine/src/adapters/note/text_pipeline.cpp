#include "adapters/note/text_pipeline.hpp"

#include <openvino/genai/llm_pipeline.hpp>
#include <openvino/genai/visual_language/pipeline.hpp>
#include <utility>

#include "adapters/models/model_store.hpp"
#include "core/env_flag.hpp"

namespace ambient::note {

namespace {

// Manifest values reach OpenVINO as the strings its own property parsing
// accepts ("32" for a float hint), bools as bools
ov::AnyMap Properties(const models::ModelInfo& info) {
    ov::AnyMap map{{"CACHE_DIR", (info.dir / ".cache").string()}};
    for (const auto& [key, value] : info.properties.items()) {
        if (value.is_boolean()) {
            map[key] = value.get<bool>();
        } else if (value.is_string()) {
            map[key] = value.get<std::string>();
        } else {
            map[key] = value.dump();
        }
    }
    return map;
}

ov::genai::StreamerVariant Wrap(const TextPipeline::Streamer& streamer) {
    if (!streamer) return std::monostate{};
    return std::function<ov::genai::StreamingStatus(std::string)>(streamer);
}

class LlmTextPipeline : public TextPipeline {
   public:
    LlmTextPipeline(const models::ModelInfo& info, const std::string& device)
        : pipeline_(info.dir, device, Properties(info)) {}

    Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                    const Streamer& streamer) override {
        ov::genai::DecodedResults result = pipeline_.generate(prompt, config, Wrap(streamer));
        return {result.perf_metrics.get_num_input_tokens()};
    }

    // The stateful pipeline on the Intel GPU keeps one KV history across
    // calls (measured)
    bool ExtendsKv() const override {
        return true;
    }

   private:
    ov::genai::LLMPipeline pipeline_;
};

// The multimodal export used text-only; the vision towers load and idle
class VlmTextPipeline : public TextPipeline {
   public:
    VlmTextPipeline(const models::ModelInfo& info, const std::string& device)
        : pipeline_(info.dir, device, Properties(info)) {}

    Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                    const Streamer& streamer) override {
        ov::genai::VLMDecodedResults result =
            pipeline_.generate(prompt, std::vector<ov::Tensor>{}, config, Wrap(streamer));
        return {result.perf_metrics.get_num_input_tokens()};
    }

    // Measured to extend the KV like the LLM pipeline; AMBIENT_VLM_PREFILL=0
    // turns the capture-phase prefill off here
    bool ExtendsKv() const override {
        return EnvFlag("AMBIENT_VLM_PREFILL");
    }

   private:
    ov::genai::VLMPipeline pipeline_;
};

}  // namespace

std::unique_ptr<TextPipeline> MakeTextPipeline(const models::ModelInfo& info,
                                               const std::string& device) {
    if (info.pipeline == "vlm") {
        return std::make_unique<VlmTextPipeline>(info, device);
    }
    return std::make_unique<LlmTextPipeline>(info, device);
}

}  // namespace ambient::note
