#include "adapters/diarisation/speaker_diariser.hpp"

#include <algorithm>

#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_clustering.hpp"
#include "core/diar_regions.hpp"
#include "core/slice_refinement.hpp"

namespace sotto::diar {

namespace {

std::vector<float> Gather(std::span<const float> audio, const std::vector<Region>& ranges) {
    std::vector<float> clip;
    for (const Region& range : ranges) {
        const auto first = static_cast<std::size_t>(range.first_frame);
        const auto end =
            std::min<std::size_t>(static_cast<std::size_t>(range.end_frame), audio.size());
        if (end > first) clip.insert(clip.end(), audio.begin() + first, audio.begin() + end);
    }
    return clip;
}

}  // namespace

SpeakerDiariser::SpeakerDiariser(const models::ModelStore& store, models::OvRuntime& runtime,
                                 const std::filesystem::path& anchor_root)
    : vad_(store, runtime),
      segmenter_(store, runtime),
      embedder_(store, runtime),
      anchors_(anchor_root) {}

DiariseResult SpeakerDiariser::Diarise(std::span<const float> audio) {
    DiariseResult result;
    if (audio.empty()) return result;

    vad_.Reset();
    std::vector<float> probabilities;
    std::vector<float> hop(audio::kVadHopFrames, 0.0f);
    for (std::size_t offset = 0; offset < audio.size(); offset += audio::kVadHopFrames) {
        const std::size_t have = std::min<std::size_t>(audio::kVadHopFrames, audio.size() - offset);
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(offset), have, hop.begin());
        std::fill(hop.begin() + static_cast<std::ptrdiff_t>(have), hop.end(), 0.0f);
        probabilities.push_back(vad_.SpeechProbability(hop));
    }

    const auto regions = SpeechRegions(probabilities, audio.size());
    const auto seg = segmenter_.Run(audio);
    const auto slices = RefineRegions(regions, seg.change_points);

    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    std::vector<Region> kept;
    for (const Region& slice : slices) {
        const auto ranges = EmbeddingRanges(slice, seg.overlap_spans);
        if (ranges.empty()) continue;
        const auto clip = Gather(audio, ranges);
        if (clip.size() < 400) continue;  // below one fbank frame
        embeddings.push_back(embedder_.Embed(clip));
        durations.push_back(slice.end_frame - slice.first_frame);
        kept.push_back(slice);
    }

    const auto clusters = ClusterSpeakers(embeddings, durations);
    std::vector<LabelledSlice> out;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        out.push_back({kept[i].first_frame, kept[i].end_frame, clusters.labels[i]});
    }

    // A long-enough overlap span inside a slice becomes a second turn on
    // the best non-primary centroid
    if (clusters.count >= 2) {
        for (std::size_t i = 0; i < kept.size(); ++i) {
            for (const Region& span : seg.overlap_spans) {
                const auto first = std::max(span.first_frame, kept[i].first_frame);
                const auto end = std::min(span.end_frame, kept[i].end_frame);
                if (end <= first || end - first < kOverlapTurnMinFrames) continue;
                const auto clip = Gather(audio, {{first, end}});
                const auto embedding = embedder_.Embed(clip);

                const int primary = clusters.labels[i];
                int second = -1;
                double best = -1e18;
                for (int c = 0; c < clusters.count; ++c) {
                    if (c == primary) continue;
                    double dot = 0.0;
                    for (std::size_t d = 0; d < embedding.size(); ++d) {
                        dot += static_cast<double>(embedding[d]) *
                               clusters.centroids[static_cast<std::size_t>(c)][d];
                    }
                    if (dot > best) {
                        best = dot;
                        second = c;
                    }
                }
                if (second >= 0) out.push_back({first, end, second});
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const LabelledSlice& a, const LabelledSlice& b) {
        return a.first_frame < b.first_frame;
    });
    result.slices = std::move(out);
    result.cluster_count = clusters.count;

    // Each cluster's similarity to the accrued anchor; a cluster too short
    // for a voiceprint ranks below any real match
    if (const auto anchor = anchors_.Anchor()) {
        result.anchor_similarity.assign(static_cast<std::size_t>(clusters.count), -2.0);
        for (int c = 0; c < clusters.count; ++c) {
            const auto voiceprint = ClusterVoiceprint(embedder_, audio, result.slices, c);
            if (voiceprint.empty()) continue;
            double dot = 0.0;
            for (std::size_t d = 0; d < voiceprint.size(); ++d) {
                dot += static_cast<double>(voiceprint[d]) * (*anchor)[d];
            }
            result.anchor_similarity[static_cast<std::size_t>(c)] = dot;
        }
    }
    return result;
}

void SpeakerDiariser::AccrueDoctor(std::span<const float> audio,
                                   const std::vector<LabelledSlice>& slices, int doctor_cluster) {
    const auto voiceprint = ClusterVoiceprint(embedder_, audio, slices, doctor_cluster);
    if (!voiceprint.empty()) anchors_.Accrue(voiceprint);
}

}  // namespace sotto::diar
