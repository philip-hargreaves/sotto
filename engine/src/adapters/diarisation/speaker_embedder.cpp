#include "adapters/diarisation/speaker_embedder.hpp"

#include <cmath>
#include <stdexcept>

#include "adapters/diarisation/fbank.hpp"

namespace sotto::diar {

SpeakerEmbedder::SpeakerEmbedder(const models::ModelStore& store, models::OvRuntime& runtime) {
    request_ =
        runtime.Load(store, "diarisation", "default", "model.xml").model.create_infer_request();
}

std::vector<float> SpeakerEmbedder::Embed(std::span<const float> audio) {
    auto features = EmbedderFbank(audio);
    if (features.frames == 0) throw std::invalid_argument("slice shorter than one fbank frame");

    const ov::Tensor input(ov::element::f32, {1, features.frames, kMelBins},
                           features.values.data());
    request_.set_input_tensor(input);
    request_.infer();

    const ov::Tensor output = request_.get_output_tensor();
    std::vector<float> embedding(output.data<float>(), output.data<float>() + kEmbeddingDims);
    float norm = 0.0f;
    for (const float x : embedding) norm += x * x;
    norm = std::sqrt(norm);
    for (float& x : embedding) x /= norm;
    return embedding;
}

}  // namespace sotto::diar
