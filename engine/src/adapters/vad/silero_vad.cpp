#include "adapters/vad/silero_vad.hpp"

#include <cstring>
#include <stdexcept>

#include "ports/audio_source.hpp"

namespace ambient::audio {

SileroVad::SileroVad(const models::ModelStore& store, models::OvRuntime& runtime) {
    models::LoadedModel loaded = runtime.Load(store, "vad", "default", "model.onnx");
    request_ = loaded.model.create_infer_request();

    ov::Tensor sample_rate(ov::element::i64, ov::Shape{});
    *sample_rate.data<std::int64_t>() = kSampleRate;
    request_.set_tensor("sr", sample_rate);

    Reset();
}

float SileroVad::SpeechProbability(std::span<const float> hop) {
    if (hop.size() != kVadHopFrames) {
        throw std::runtime_error("vad hop must be exactly 512 frames");
    }

    ov::Tensor input(ov::element::f32, {1, kContext + kVadHopFrames});
    std::memcpy(input.data<float>(), context_.data(), kContext * sizeof(float));
    std::memcpy(input.data<float>() + kContext, hop.data(), hop.size_bytes());
    request_.set_tensor("input", input);
    request_.set_tensor("state", state_);
    request_.infer();

    const ov::Tensor next_state = request_.get_tensor("stateN");
    std::memcpy(state_.data<float>(), next_state.data<float>(), next_state.get_byte_size());
    std::memcpy(context_.data(), hop.data() + hop.size() - kContext, kContext * sizeof(float));
    return request_.get_tensor("output").data<float>()[0];
}

void SileroVad::Reset() {
    std::memset(state_.data<float>(), 0, state_.get_byte_size());
    context_.fill(0.0f);
}

}  // namespace ambient::audio
