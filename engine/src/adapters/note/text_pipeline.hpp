#pragma once

#include <functional>
#include <memory>
#include <string>

#include <openvino/genai/generation_config.hpp>
#include <openvino/genai/streamer_base.hpp>

namespace ambient::models {
struct ModelInfo;
}  // namespace ambient::models

namespace ambient::note {

// One resident model behind whichever GenAI pipeline its manifest names; the
// writer never learns which. A single history: a prompt that extends the
// last one costs the delta, one that diverges re-prefills
class TextPipeline {
   public:
    using Streamer = std::function<ov::genai::StreamingStatus(std::string)>;

    struct Result {
        std::size_t input_tokens = 0;
    };

    virtual ~TextPipeline() = default;

    // Streams pieces to the streamer when given; the streamer's STOP ends the generation
    virtual Result Generate(const std::string& prompt, const ov::genai::GenerationConfig& config,
                            const Streamer& streamer) = 0;

    // Whether a generate call extends the previous call's KV on this
    // pipeline (measured per implementation): the capture-phase prefill and
    // the prefix warm are pointless work otherwise
    virtual bool ExtendsKv() const = 0;
};

// Builds the pipeline the manifest names, on the resolved device, with the
// compile cache beside the model and the manifest's properties verbatim
std::unique_ptr<TextPipeline> MakeTextPipeline(const models::ModelInfo& info,
                                               const std::string& device);

}  // namespace ambient::note
