#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "adapters/models/deferred_load.hpp"
#include "ports/diariser.hpp"

namespace ambient::diar {

// Diarisation behind a background load; callers already run off the capture thread
class DeferredDiariser : public IDiariser {
   public:
    explicit DeferredDiariser(std::function<std::unique_ptr<IDiariser>()> build)
        : inner_("diarisation", std::move(build)) {}

    DiariseResult Diarise(std::span<const float> audio,
                          std::span<const std::uint64_t> turn_boundaries = {}) override {
        return inner_.Get().Diarise(audio, turn_boundaries);
    }

    void AccrueDoctor(std::span<const float> audio, const std::vector<LabelledSlice>& slices,
                      int doctor_cluster) override {
        inner_.Get().AccrueDoctor(audio, slices, doctor_cluster);
    }

    std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                           const std::vector<LabelledSlice>& slices,
                                           int cluster_count) override {
        return inner_.Get().AnchorSimilarities(audio, slices, cluster_count);
    }

    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode) override {
        inner_.Get().Advance(audio, turns, decode);
    }

    void Settle(std::span<const float> audio, std::span<const asr::Turn> turns,
                const DecodeClipFn& decode) override {
        inner_.Get().Settle(audio, turns, decode);
    }

    TurnTexts TakeTurnTexts() override {
        return inner_.Get().TakeTurnTexts();
    }

    TurnChunks TakeTurnChunks() override {
        return inner_.Get().TakeTurnChunks();
    }

    std::vector<std::vector<float>> ClusterCentroids() override {
        return inner_.Get().ClusterCentroids();
    }

    std::vector<float> EmbedSpan(std::span<const float> audio, std::uint64_t first,
                                 std::uint64_t end) override {
        return inner_.Get().EmbedSpan(audio, first, end);
    }

    std::vector<asr::Turn> SpeculativeTranscript() override {
        return inner_.Get().SpeculativeTranscript();
    }

    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        inner_.Get().AddCutPoints(cuts);
    }

    std::vector<float> DoctorVoiceprint(std::span<const float> audio,
                                        const std::vector<LabelledSlice>& slices,
                                        int doctor_cluster) override {
        return inner_.Get().DoctorVoiceprint(audio, slices, doctor_cluster);
    }

    void AccrueVoiceprint(std::span<const float> voiceprint) override {
        inner_.Get().AccrueVoiceprint(voiceprint);
    }

    std::vector<float> EmbedVoice(std::span<const float> audio) override {
        return inner_.Get().EmbedVoice(audio);
    }

    void ReplaceAnchor(std::span<const float> voiceprint, std::uint64_t enrolled_at) override {
        inner_.Get().ReplaceAnchor(voiceprint, enrolled_at);
    }

    void DiscardCapture() override {
        if (inner_.Loaded()) {
            inner_.Get().DiscardCapture();
        }
    }

   private:
    models::DeferredLoad<IDiariser> inner_;
};

}  // namespace ambient::diar
