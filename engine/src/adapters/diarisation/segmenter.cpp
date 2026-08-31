#include "adapters/diarisation/segmenter.hpp"

#include <algorithm>
#include <cstring>

namespace ambient::diar {

namespace {

// Powerset class -> dominant single speaker (-1: silence or overlap)
constexpr int kDominant[7] = {-1, 0, 1, 2, -1, -1, -1};

}  // namespace

void DecodeSegWindow(std::span<const std::int8_t> classes, std::uint64_t window_first_frame,
                     SegResult& result) {
    const double step = static_cast<double>(kSegWindowFrames) / static_cast<double>(classes.size());
    int prev = -1;
    std::size_t prev_i = 0;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const int cls = classes[i];
        if (cls >= 4) {  // pairwise-overlap classes
            const auto a0 = window_first_frame + static_cast<std::uint64_t>(i * step);
            const auto a1 = window_first_frame + static_cast<std::uint64_t>((i + 1) * step);
            if (!result.overlap_spans.empty() && result.overlap_spans.back().end_frame == a0) {
                result.overlap_spans.back().end_frame = a1;
            } else {
                result.overlap_spans.push_back({a0, a1});
            }
        }
        const int dominant = kDominant[cls];
        if (dominant >= 0) {
            if (prev >= 0 && dominant != prev &&
                i - prev_i >= static_cast<std::size_t>(kSegMinHoldFrames)) {
                result.change_points.push_back(window_first_frame +
                                               static_cast<std::uint64_t>(i * step));
            }
            if (dominant != prev) prev_i = i;
            prev = dominant;
        }
    }
}

Segmenter::Segmenter(const models::ModelStore& store, models::OvRuntime& runtime) {
    request_ =
        runtime.Load(store, "segmentation", "default", "model.onnx").model.create_infer_request();
}

SegResult Segmenter::Run(std::span<const float> audio) {
    SegResult result;
    ov::Tensor input(ov::element::f32, {1, 1, kSegWindowFrames});
    for (std::uint64_t offset = 0; offset < audio.size(); offset += kSegWindowFrames) {
        float* x = input.data<float>();
        const std::size_t have = std::min<std::size_t>(kSegWindowFrames, audio.size() - offset);
        std::memcpy(x, audio.data() + offset, have * sizeof(float));
        std::fill(x + have, x + kSegWindowFrames, 0.0f);

        request_.set_input_tensor(input);
        request_.infer();
        const ov::Tensor output = request_.get_output_tensor();  // (1, frames, 7)
        const auto shape = output.get_shape();
        const std::size_t frames = shape[1];
        const std::size_t classes = shape[2];
        const float* logits = output.data<float>();

        std::vector<std::int8_t> argmax(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const float* row = logits + i * classes;
            argmax[i] = static_cast<std::int8_t>(std::max_element(row, row + classes) - row);
        }
        DecodeSegWindow(argmax, offset, result);
    }
    return result;
}

}  // namespace ambient::diar
