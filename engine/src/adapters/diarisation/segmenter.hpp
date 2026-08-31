#pragma once

#include <cstdint>
#include <openvino/openvino.hpp>
#include <span>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"
#include "core/diar_regions.hpp"

namespace ambient::diar {

inline constexpr std::size_t kSegWindowFrames = 160000;  // the model's fixed 10 s input
inline constexpr int kSegMinHoldFrames = 6;              // ~100 ms flicker guard

struct SegResult {
    std::vector<std::uint64_t> change_points;  // absolute frames where the dominant speaker flips
    std::vector<Region> overlap_spans;         // absolute frames where two speakers are active
};

// Pure decode of one 10 s window's powerset argmax; a change counts only
// after kSegMinHoldFrames
void DecodeSegWindow(std::span<const std::int8_t> classes, std::uint64_t window_first_frame,
                     SegResult& result);

// pyannote segmentation-3.0 through the model store, on CPU (its bi-LSTM
// excludes the NPU). Windows are non-overlapping, the last zero-padded
class Segmenter {
   public:
    Segmenter(const models::ModelStore& store, models::OvRuntime& runtime);

    SegResult Run(std::span<const float> audio);

   private:
    ov::InferRequest request_;
};

}  // namespace ambient::diar
