#pragma once

#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/diarisation/diar_worker.hpp"
#include "adapters/diarisation/segmenter.hpp"
#include "adapters/diarisation/speaker_embedder.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "ports/diariser.hpp"

namespace ambient::diar {

// The batch chain: VAD -> segmentation -> slices -> embeddings -> clusters,
// with long overlap spans emitted as second turns. Owns its own VAD
// (Silero is stateful) and the clinician anchor
class SpeakerDiariser : public IDiariser {
   public:
    SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime,
                    const std::filesystem::path& anchor_root);

    DiariseResult Diarise(std::span<const float> audio,
                          std::span<const std::uint64_t> turn_boundaries = {}) override;

    std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                           const std::vector<LabelledSlice>& slices,
                                           int cluster_count) override;

    // Reuses the voiceprint AnchorSimilarities computed for the cluster;
    // embeds only when there is none
    void AccrueDoctor(std::span<const float> audio, const std::vector<LabelledSlice>& slices,
                      int doctor_cluster) override;

    // Capture-phase work; Diarise then finalises from the accumulated state
    void Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                 const DecodeClipFn& decode) override {
        worker_.Advance(audio, turns, decode);
    }

    void Settle(std::span<const float> audio, std::span<const asr::Turn> turns,
                const DecodeClipFn& decode) override {
        worker_.Advance(audio, turns, decode, std::numeric_limits<int>::max());
    }

    TurnTexts TakeTurnTexts() override {
        return std::exchange(texts_, {});
    }

    TurnChunks TakeTurnChunks() override {
        return std::exchange(chunks_, {});
    }

    std::vector<std::vector<float>> ClusterCentroids() override {
        return centroids_;
    }

    std::vector<float> EmbedSpan(std::span<const float> audio, std::uint64_t first,
                                 std::uint64_t end) override {
        const auto it = chunk_embeddings_.find({first, end});
        if (it != chunk_embeddings_.end()) return it->second;
        if (end <= first || end - first < 400 || end > audio.size()) return {};
        return embedder_.Embed(audio.subspan(first, end - first));
    }

    // Roles named as finalise names them, with cluster centroids standing in
    // for the voiceprints against the anchor
    std::vector<asr::Turn> SpeculativeTranscript() override;

    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        worker_.AddCutPoints(cuts);
    }

    void AddPunctuationCuts(std::span<const std::uint64_t> cuts) override {
        worker_.AddPunctuationCuts(cuts);
    }

    void DiscardCapture() override {
        (void)worker_.Take();
        texts_.clear();
        chunks_.clear();
        chunk_embeddings_.clear();
        voiceprints_.clear();
    }

    SpeakerEmbedder& Embedder() {
        return embedder_;
    }

   private:
    audio::SileroVad vad_;
    Segmenter segmenter_;
    SpeakerEmbedder embedder_;
    AnchorStore anchors_;
    DiarWorker worker_;
    TurnTexts texts_;
    TurnChunks chunks_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> chunk_embeddings_;
    std::vector<std::vector<float>> centroids_;    // finalise clusters, from Diarise
    std::vector<std::vector<float>> voiceprints_;  // per cluster, from AnchorSimilarities
};

}  // namespace ambient::diar
