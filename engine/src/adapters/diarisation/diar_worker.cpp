#include "adapters/diarisation/diar_worker.hpp"

#include <algorithm>
#include <string>

#include "adapters/diarisation/speaker_clustering.hpp"
#include "core/diar_capture.hpp"
#include "core/diar_regions.hpp"
#include "core/per_turn.hpp"
#include "core/slice_refinement.hpp"

namespace sotto::diar {

DiarWorker::DiarWorker(audio::SileroVad& vad, Segmenter& segmenter, SpeakerEmbedder& embedder)
    : vad_(vad), segmenter_(segmenter), embedder_(embedder) {
    vad_.Reset();
}

void DiarWorker::Finish(std::span<const float> audio) {
    auto& s = state_;
    std::vector<float> hop(audio::kVadHopFrames, 0.0f);
    while (s.vad_probabilities.size() * audio::kVadHopFrames < audio.size()) {
        const auto at = s.vad_probabilities.size() * audio::kVadHopFrames;
        const auto have = std::min<std::size_t>(audio::kVadHopFrames, audio.size() - at);
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(at), have, hop.begin());
        std::fill(hop.begin() + static_cast<std::ptrdiff_t>(have), hop.end(), 0.0f);
        s.vad_probabilities.push_back(vad_.SpeechProbability(hop));
    }
    if (s.seg_done < audio.size()) {
        const auto part = segmenter_.Run(audio.subspan(s.seg_done));
        for (const auto c : part.change_points) s.seg.change_points.push_back(c + s.seg_done);
        for (const auto& span : part.overlap_spans) {
            s.seg.overlap_spans.push_back(
                {span.first_frame + s.seg_done, span.end_frame + s.seg_done});
        }
        s.seg_done = audio.size();
    }
}

const std::vector<float>& DiarWorker::EmbedSlice(std::span<const float> audio,
                                                 const Region& slice) {
    auto& slot = state_.embeddings[{slice.first_frame, slice.end_frame}];
    if (slot.empty()) {
        const auto ranges = EmbeddingRanges(slice, state_.seg.overlap_spans);
        std::vector<float> clip;
        for (const auto& range : ranges) {
            const auto first = static_cast<std::size_t>(range.first_frame);
            const auto end =
                std::min<std::size_t>(static_cast<std::size_t>(range.end_frame), audio.size());
            if (end > first) clip.insert(clip.end(), audio.begin() + first, audio.begin() + end);
        }
        if (clip.size() >= 400) slot = embedder_.Embed(clip);  // below one fbank frame
    }
    return slot;
}

void DiarWorker::Advance(std::span<const float> audio, std::span<const asr::Turn> turns,
                         const DecodeClipFn& decode) {
    auto& s = state_;

    // Whole hops only; finalise pads the final partial one
    while ((s.vad_probabilities.size() + 1) * audio::kVadHopFrames <= audio.size()) {
        const auto at = s.vad_probabilities.size() * audio::kVadHopFrames;
        s.vad_probabilities.push_back(
            vad_.SpeechProbability(audio.subspan(at, audio::kVadHopFrames)));
    }

    while (s.seg_done + kSegWindowFrames <= audio.size()) {
        const auto part = segmenter_.Run(audio.subspan(s.seg_done, kSegWindowFrames));
        for (const auto c : part.change_points) s.seg.change_points.push_back(c + s.seg_done);
        for (const auto& span : part.overlap_spans) {
            s.seg.overlap_spans.push_back(
                {span.first_frame + s.seg_done, span.end_frame + s.seg_done});
        }
        s.seg_done += kSegWindowFrames;
    }

    const auto settled = SettledFrontier(s.seg_done, turns, audio.size());
    if (settled == 0) return;

    // Slices behind the frontier, cut exactly as finalise cuts them
    auto cps = s.seg.change_points;
    for (const auto& turn : turns) {
        if (turn.first_frame <= settled) cps.push_back(turn.first_frame);
        if (turn.first_frame + turn.frame_count <= settled) {
            cps.push_back(turn.first_frame + turn.frame_count);
        }
    }
    std::sort(cps.begin(), cps.end());
    auto regions =
        SpeechRegions(s.vad_probabilities, s.vad_probabilities.size() * audio::kVadHopFrames);
    std::erase_if(regions, [&](const Region& r) { return r.end_frame > settled; });
    const auto slices = RefineRegions(regions, cps);

    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    std::vector<Region> kept;
    for (const auto& slice : slices) {
        const auto& e = EmbedSlice(audio, slice);
        if (e.empty()) continue;
        embeddings.push_back(e);
        durations.push_back(slice.end_frame - slice.first_frame);
        kept.push_back(slice);
    }
    if (kept.size() < 2) return;

    // Provisional labels, built exactly as finalise builds them; a span
    // that final clustering changes is simply never looked up
    const auto clusters = ClusterSpeakers(embeddings, durations);
    std::vector<LabelledSlice> labelled;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        labelled.push_back({kept[i].first_frame, kept[i].end_frame, clusters.labels[i]});
    }
    if (clusters.count >= 2) {
        for (std::size_t i = 0; i < kept.size(); ++i) {
            for (const Region& span : s.seg.overlap_spans) {
                const auto first = std::max(span.first_frame, kept[i].first_frame);
                const auto end = std::min(span.end_frame, kept[i].end_frame);
                if (end <= first || end - first < kOverlapTurnMinFrames) continue;
                // Memoised separately from slice embeddings (same span, raw
                // audio); this runs every tick over all settled overlaps
                auto& embedding = overlap_cache_[{first, end}];
                if (embedding.empty()) {
                    embedding = embedder_.Embed(
                        audio.subspan(first, static_cast<std::size_t>(end - first)));
                }
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
                if (second >= 0) labelled.push_back({first, end, second});
            }
        }
        std::sort(labelled.begin(), labelled.end(),
                  [](const LabelledSlice& a, const LabelledSlice& b) {
                      return a.first_frame < b.first_frame;
                  });
    }

    // Decode settled turns into the cache; the last turn may still grow
    auto merged = MergeByCluster(labelled);
    if (merged.size() < 2) return;
    merged.pop_back();
    const auto spans = DecodeSpans(merged, audio.size());
    int budget = kSpeculateBudget;
    for (const auto& span : spans) {
        const auto a = span.first_frame;
        const auto b = span.end_frame;
        if (a >= b || b - a < kPerTurnMinClipFrames) continue;
        if (s.turn_texts.contains({a, b})) continue;
        if (budget-- <= 0) break;
        const std::string text = decode(audio.subspan(a, b - a), a);
        if (text.empty()) continue;  // rejected, silent, or stopping
        if (detail::MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
        s.turn_texts[{a, b}] = text;
    }
}

}  // namespace sotto::diar
