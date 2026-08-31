#include "adapters/diarisation/fbank.hpp"

#include <cstdint>

#include "kaldi-native-fbank/csrc/online-feature.h"

namespace sotto::diar {

// The blessed research contract (torchaudio kaldi defaults + ERes2NetV2
// front-end); a mismatch fails embedder parity
FbankFeatures EmbedderFbank(std::span<const float> audio) {
    knf::FbankOptions opts;
    opts.frame_opts.samp_freq = 16000.0f;
    opts.frame_opts.frame_length_ms = 25.0f;
    opts.frame_opts.frame_shift_ms = 10.0f;
    opts.frame_opts.window_type = "povey";
    opts.frame_opts.dither = 0.0f;  // knf's default is 3e-5, torchaudio's is 0
    opts.frame_opts.preemph_coeff = 0.97f;
    opts.frame_opts.remove_dc_offset = true;
    opts.frame_opts.round_to_power_of_two = true;  // 400-sample frame, 512-point FFT
    opts.frame_opts.snip_edges = true;
    opts.mel_opts.num_bins = static_cast<std::int32_t>(kMelBins);
    opts.mel_opts.low_freq = 20.0f;
    opts.mel_opts.high_freq = 0.0f;  // offset from Nyquist: 8000 Hz
    opts.use_energy = false;
    opts.use_log_fbank = true;
    opts.use_power = true;
    opts.htk_compat = false;

    knf::OnlineFbank fbank(opts);
    fbank.AcceptWaveform(16000.0f, audio.data(), static_cast<std::int32_t>(audio.size()));
    fbank.InputFinished();

    FbankFeatures out;
    out.frames = static_cast<std::size_t>(fbank.NumFramesReady());
    out.values.reserve(out.frames * kMelBins);
    for (std::size_t i = 0; i < out.frames; ++i) {
        const float* frame = fbank.GetFrame(static_cast<std::int32_t>(i));
        out.values.insert(out.values.end(), frame, frame + kMelBins);
    }

    // Per-slice mean subtraction over time, per mel bin; no variance norm
    if (out.frames > 0) {
        for (std::size_t bin = 0; bin < kMelBins; ++bin) {
            double mean = 0.0;
            for (std::size_t i = 0; i < out.frames; ++i) mean += out.values[i * kMelBins + bin];
            mean /= static_cast<double>(out.frames);
            for (std::size_t i = 0; i < out.frames; ++i) {
                out.values[i * kMelBins + bin] -= static_cast<float>(mean);
            }
        }
    }
    return out;
}

}  // namespace sotto::diar
