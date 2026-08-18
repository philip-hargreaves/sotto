#pragma once

#include <openvino/openvino.hpp>
#include <span>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace sotto::diar {

inline constexpr std::size_t kEmbeddingDims = 192;

// ERes2NetV2 through the model store, on CPU - the researched placement for
// this conv model (INT8 lossless and 1.9x faster there; GPU buys nothing)
class SpeakerEmbedder {
   public:
    SpeakerEmbedder(const models::ModelStore& store, models::OvRuntime& runtime);

    // Unit-norm voiceprint of a speech slice; float32 [-1, 1] mono 16 kHz
    std::vector<float> Embed(std::span<const float> audio);

   private:
    ov::InferRequest request_;
};

}  // namespace sotto::diar
