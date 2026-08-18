#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace sotto::diar {

inline constexpr std::size_t kMelBins = 80;

struct FbankFeatures {
    std::vector<float> values;  // frames x kMelBins, row-major
    std::size_t frames = 0;
};

// The speaker embedder's input feature: kaldi fbank to the researched
// contract, mean-normalised per slice. Audio must be float32 in [-1, 1]
// mono 16 kHz - int16-range values break parity in near-silent bins even
// though the mean subtraction removes the constant shift
FbankFeatures EmbedderFbank(std::span<const float> audio);

}  // namespace sotto::diar
