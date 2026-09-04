#include "adapters/diarisation/speaker_diariser.hpp"

#include <algorithm>
#include <chrono>

#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_clustering.hpp"
#include "core/clip_cuts.hpp"
#include "core/diar_regions.hpp"
#include "core/env_flag.hpp"
#include "core/role_naming.hpp"
#include "core/slice_refinement.hpp"

namespace ambient::diar {

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
      anchors_(anchor_root),
      worker_(vad_, segmenter_, embedder_) {}

DiariseResult SpeakerDiariser::Diarise(std::span<const float> audio,
                                       std::span<const std::uint64_t> turn_boundaries) {
    DiariseResult result;
    if (audio.empty()) return result;
    // Stage laps for the finalise breakdown; measurement only
    auto lap_start = std::chrono::steady_clock::now();
    const auto lap = [&lap_start] {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - lap_start).count();
        lap_start = now;
        return seconds;
    };

    // With capture-phase state, finalise only completes it; without, the
    // whole recording is processed here. Either way the maths is identical
    CaptureDiarisation capture;
    std::vector<float> probabilities;
    SegResult seg;
    if (worker_.Engaged()) {
        worker_.Finish(audio);
        capture = worker_.Take();
        probabilities = std::move(capture.vad_probabilities);
        seg = std::move(capture.seg);
        texts_ = std::move(capture.turn_texts);
        chunks_ = std::move(capture.turn_chunks);
        chunk_embeddings_ = std::move(capture.chunk_embeddings);
    } else {
        vad_.Reset();
        std::vector<float> hop(audio::kVadHopFrames, 0.0f);
        for (std::size_t offset = 0; offset < audio.size(); offset += audio::kVadHopFrames) {
            const std::size_t have =
                std::min<std::size_t>(audio::kVadHopFrames, audio.size() - offset);
            std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(offset), have, hop.begin());
            std::fill(hop.begin() + static_cast<std::ptrdiff_t>(have), hop.end(), 0.0f);
            probabilities.push_back(vad_.SpeechProbability(hop));
        }
        seg = segmenter_.Run(audio);
    }

    result.timing.finish_s = lap();
    const auto regions = SpeechRegions(probabilities, audio.size());
    seg.change_points.insert(seg.change_points.end(), turn_boundaries.begin(),
                             turn_boundaries.end());
    if (EnvFlag("AMBIENT_CLIP_CUTS")) {
        const auto cuts = SnapClipCuts(capture.clip_cuts, probabilities, seg.change_points);
        LogClipCuts("finalise", capture.clip_cuts, cuts, probabilities);
        seg.change_points.insert(seg.change_points.end(), cuts.begin(), cuts.end());
    }
    std::sort(seg.change_points.begin(), seg.change_points.end());
    const auto slices = RefineRegions(regions, seg.change_points);

    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    std::vector<Region> kept;
    for (const Region& slice : slices) {
        const auto it = capture.embeddings.find({slice.first_frame, slice.end_frame});
        if (it != capture.embeddings.end()) {
            if (it->second.empty()) continue;  // was too short to embed
            embeddings.push_back(it->second);
            ++result.timing.embed_hits;
        } else {
            const auto ranges = EmbeddingRanges(slice, seg.overlap_spans);
            if (ranges.empty()) continue;
            const auto clip = Gather(audio, ranges);
            if (clip.size() < 400) continue;  // below one fbank frame
            embeddings.push_back(embedder_.Embed(clip));
            ++result.timing.embed_misses;
        }
        durations.push_back(slice.end_frame - slice.first_frame);
        kept.push_back(slice);
    }

    result.timing.embed_s = lap();
    const auto clusters = ClusterSpeakers(embeddings, durations);
    centroids_ = clusters.centroids;
    result.timing.cluster_s = lap();
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

    result.timing.overlap_s = lap();
    std::sort(out.begin(), out.end(), [](const LabelledSlice& a, const LabelledSlice& b) {
        return a.first_frame < b.first_frame;
    });
    result.slices = std::move(out);
    result.cluster_count = clusters.count;

    voiceprints_.clear();  // a new finalise; AnchorSimilarities refills them
    return result;
}

std::vector<double> SpeakerDiariser::AnchorSimilarities(std::span<const float> audio,
                                                        const std::vector<LabelledSlice>& slices,
                                                        int cluster_count) {
    // Each cluster's similarity to the accrued anchor; a cluster too short
    // for a voiceprint ranks below any real match
    const auto anchor = anchors_.Anchor();
    if (!anchor) return {};
    std::vector<double> similarity(static_cast<std::size_t>(cluster_count), -2.0);
    voiceprints_.assign(static_cast<std::size_t>(cluster_count), {});
    for (int c = 0; c < cluster_count; ++c) {
        auto voiceprint = ClusterVoiceprint(embedder_, audio, slices, c);
        if (voiceprint.empty()) continue;
        double dot = 0.0;
        for (std::size_t d = 0; d < voiceprint.size(); ++d) {
            dot += static_cast<double>(voiceprint[d]) * (*anchor)[d];
        }
        similarity[static_cast<std::size_t>(c)] = dot;
        voiceprints_[static_cast<std::size_t>(c)] = std::move(voiceprint);
    }
    return similarity;
}

std::vector<asr::Turn> SpeakerDiariser::SpeculativeTranscript() {
    const Speculation& spec = worker_.LastSpeculation();
    if (spec.turns.empty()) return {};
    std::vector<double> similarity;
    const auto anchor = anchors_.Anchor();
    if (anchor && spec.centroids.size() == static_cast<std::size_t>(spec.cluster_count)) {
        for (const auto& centroid : spec.centroids) {
            double dot = 0.0;
            for (std::size_t d = 0; d < centroid.size() && d < anchor->size(); ++d) {
                dot += static_cast<double>(centroid[d]) * (*anchor)[d];
            }
            similarity.push_back(dot);
        }
    }
    std::vector<RoleTurn> role_turns;
    for (std::size_t i = 0; i < spec.turns.size(); ++i) {
        role_turns.push_back({spec.turns[i].cluster,
                              spec.turns[i].end_frame - spec.turns[i].first_frame, spec.texts[i]});
    }
    const auto roles = NameRoles(role_turns, spec.cluster_count, similarity);
    std::vector<asr::Turn> out;
    for (std::size_t i = 0; i < spec.turns.size(); ++i) {
        const auto cluster = static_cast<std::size_t>(spec.turns[i].cluster);
        asr::Turn turn;
        turn.first_frame = spec.turns[i].first_frame;
        turn.frame_count = spec.turns[i].end_frame - spec.turns[i].first_frame;
        turn.speaker =
            cluster < roles.role_of_cluster.size() ? roles.role_of_cluster[cluster] : "unknown";
        turn.text = spec.texts[i];
        out.push_back(std::move(turn));
    }
    return out;
}

void SpeakerDiariser::AccrueDoctor(std::span<const float> audio,
                                   const std::vector<LabelledSlice>& slices, int doctor_cluster) {
    const auto index = static_cast<std::size_t>(doctor_cluster);
    const auto voiceprint = index < voiceprints_.size() && !voiceprints_[index].empty()
                                ? voiceprints_[index]
                                : ClusterVoiceprint(embedder_, audio, slices, doctor_cluster);
    if (!voiceprint.empty()) anchors_.Accrue(voiceprint);
}

}  // namespace ambient::diar
