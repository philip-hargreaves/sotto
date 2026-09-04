#include "adapters/diarisation/diar_worker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "adapters/diarisation/speaker_clustering.hpp"
#include "core/clip_cuts.hpp"
#include "core/padded_decode.hpp"
#include "core/resplit.hpp"
#include "core/diar_capture.hpp"
#include "core/diar_regions.hpp"
#include "core/env_flag.hpp"
#include "core/per_turn.hpp"
#include "core/slice_refinement.hpp"

namespace ambient::diar {

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
                         const DecodeClipFn& decode, int budget) {
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

    const auto settled =
        EnvFlag("AMBIENT_SEG_FRONTIER")
            ? SegSettledFrontier(s.seg_done, s.vad_probabilities.size() * audio::kVadHopFrames)
            : SettledFrontier(s.seg_done, turns, audio.size());
    if (settled == 0) return;
    // Without a budget this is finalise's catch-up; its phases are logged
    const bool catch_up = budget == std::numeric_limits<int>::max();
    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };

    // Slices behind the frontier, cut exactly as finalise cuts them
    // (including the AMBIENT_DIAR_SEG_CUTS_ONLY windowless ablation)
    auto cps = s.seg.change_points;
    if (!EnvFlag("AMBIENT_DIAR_SEG_CUTS_ONLY")) {
        for (const auto& turn : turns) {
            if (turn.first_frame <= settled) cps.push_back(turn.first_frame);
            if (turn.first_frame + turn.frame_count <= settled) {
                cps.push_back(turn.first_frame + turn.frame_count);
            }
        }
    }
    if (EnvFlag("AMBIENT_CLIP_CUTS")) {
        // =snap: onto a VAD pause and clear of other cuts; =punct: snap plus
        // sentence ends within a wider window; anything else raw
        const std::string mode = EnvValue("AMBIENT_CLIP_CUTS");
        const auto cuts = mode == "snap" || mode == "punct"
                              ? SnapClipCuts(s.clip_cuts, s.vad_probabilities, cps)
                          : mode == "exact"
                              ? SnapClipCuts(s.clip_cuts, s.vad_probabilities, cps, 0, false)
                              : s.clip_cuts;
        LogClipCuts("capture", s.clip_cuts, cuts, s.vad_probabilities);
        cps.insert(cps.end(), cuts.begin(), cuts.end());
        if (mode == "punct") {
            const auto sentence =
                SnapClipCuts(s.punct_cuts, s.vad_probabilities, cps, kPunctCutSnapFrames);
            cps.insert(cps.end(), sentence.begin(), sentence.end());
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
    const auto t_embed = Clock::now();
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
    const auto t_cluster = Clock::now();
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

    // Decode settled turns into the cache. The last turn may still grow unless
    // closed; the catch-up decodes everything
    auto merged = MergeByCluster(labelled);
    if (merged.size() < 2) return;
    // A last turn closes on silence after it, or once the frontier is
    // kTurnCloseFrames past its end (the next speaker may have started at once)
    if (!catch_up) {
        const auto end = merged.back().end_frame;
        const bool frontier_past = settled > end && settled - end >= kTurnCloseFrames;
        if (!frontier_past && !TurnClosed(s.vad_probabilities, end)) merged.pop_back();
    }
    const auto spans = DecodeSpans(merged, audio.size());
    const auto t_decode = Clock::now();
    int decoded = 0;
    int cached = 0;
    double decoded_audio = 0.0;
    const auto pad = ClipPadFrames();
    const auto cached_text = [&](std::size_t i) -> std::string {
        if (i >= spans.size()) return {};
        const auto it = s.turn_texts.find({spans[i].first_frame, spans[i].end_frame});
        return it == s.turn_texts.end() ? std::string() : it->second;
    };
    // Long spans first so a short one can be padded with its neighbours' words
    for (const bool short_pass : {false, true}) {
        for (std::size_t i = 0; i < spans.size(); ++i) {
            const auto a = spans[i].first_frame;
            const auto b = spans[i].end_frame;
            if (a >= b || b - a < MinClipFrames()) continue;
            if ((b - a < kPadBelowFrames) != short_pass) continue;
            if (s.turn_texts.contains({a, b})) {
                ++cached;
                continue;
            }
            if (auto assembled = AssembleFromChunks(s.turn_chunks, a, b)) {
                s.turn_texts[{a, b}] = JoinedText(*assembled);
                s.turn_chunks[{a, b}] = std::move(*assembled);
                ++cached;
                continue;
            }
            if (budget-- <= 0) break;
            auto chunks_of_span =
                PaddedDecode(audio, a, b, pad, decode, i > 0 ? cached_text(i - 1) : std::string(),
                             cached_text(i + 1));
            const std::string text = JoinedText(chunks_of_span);
            ++decoded;
            decoded_audio += static_cast<double>(b - a) / audio::kSampleRate;
            if (catch_up || EnvFlag("AMBIENT_CUT_DEBUG")) {
                std::fprintf(stderr, "ambient-engine: %s decoded %.1f-%.1f s\n",
                             catch_up ? "catch-up" : "tick",
                             static_cast<double>(a) / audio::kSampleRate,
                             static_cast<double>(b) / audio::kSampleRate);
            }
            if (text.empty()) continue;  // rejected, silent, or stopping
            if (detail::MaxRepeatedNgram(text) >= kPerTurnMaxRepeat) continue;
            s.turn_texts[{a, b}] = text;
            s.turn_chunks[{a, b}] = std::move(chunks_of_span);
        }
    }
    if (catch_up) {
        const auto t_end = Clock::now();
        std::fprintf(stderr,
                     "ambient-engine: catch-up frontier %.1f of %.1f s, slices %zu, "
                     "segment+cuts %.2f s, embed %.2f s, cluster %.2f s, decode %d spans "
                     "(%.1f s audio, %d cached) %.2f s, total %.2f s\n",
                     static_cast<double>(settled) / audio::kSampleRate,
                     static_cast<double>(audio.size()) / audio::kSampleRate, kept.size(),
                     seconds(t_start, t_embed), seconds(t_embed, t_cluster),
                     seconds(t_cluster, t_decode), decoded, decoded_audio, cached,
                     seconds(t_decode, t_end), seconds(t_start, t_end));
    }

    // AMBIENT_RESPLIT: edge chunks are embedded now, a few per tick, so the
    // finalise re-split only takes dot products
    if (EnvFlag("AMBIENT_RESPLIT")) {
        int embeds = catch_up ? std::numeric_limits<int>::max() : kEdgeEmbedBudget;
        for (std::size_t i = 0; i < merged.size() && embeds > 0; ++i) {
            const auto key = std::make_pair(spans[i].first_frame, spans[i].end_frame);
            const auto it = s.turn_chunks.find(key);
            if (it == s.turn_chunks.end() || it->second.size() < 2) continue;
            const auto& parts = it->second;
            const std::size_t idx[4] = {0, 1, parts.size() - 2, parts.size() - 1};
            for (const std::size_t j : idx) {
                if (embeds <= 0) break;
                const auto& p = parts[j];
                const std::uint64_t lo = std::max(p.first_frame, merged[i].first_frame);
                const std::uint64_t hi = std::min(p.first_frame + p.frame_count, merged[i].end_frame);
                if (hi <= lo || hi - lo < kResplitMinFrames) continue;
                if (s.chunk_embeddings.contains({lo, hi})) continue;
                s.chunk_embeddings[{lo, hi}] = embedder_.Embed(audio.subspan(lo, hi - lo));
                --embeds;
            }
        }
    }
    speculation_.turns = SpeculatedTurns(merged, audio.size(), s.turn_texts, &speculation_.texts);
    speculation_.centroids = clusters.centroids;
    speculation_.cluster_count = clusters.count;
    // The prefill reads this transcript; it must re-split as the seal does or
    // the prompt diverges at the first move
    if (EnvFlag("AMBIENT_RESPLIT") && clusters.count >= 2 && !speculation_.turns.empty()) {
        const auto spec_spans = DecodeSpans(speculation_.turns, audio.size());
        std::vector<std::vector<asr::Turn>> chunks(speculation_.turns.size());
        for (std::size_t i = 0; i < spec_spans.size(); ++i) {
            const auto it = s.turn_chunks.find({spec_spans[i].first_frame, spec_spans[i].end_frame});
            if (it != s.turn_chunks.end()) chunks[i] = it->second;
        }
        const std::string margin_env = EnvValue("AMBIENT_RESPLIT_MARGIN");
        const double margin = margin_env.empty() ? kResplitMargin : std::atof(margin_env.c_str());
        const auto pieces = ResplitByEmbedding(
            speculation_.turns, speculation_.texts, chunks,
            [&](std::uint64_t first, std::uint64_t end) -> std::vector<float> {
                const auto it = s.chunk_embeddings.find({first, end});
                if (it != s.chunk_embeddings.end()) return it->second;
                return {};  // not yet embedded: judged next tick
            },
            clusters.centroids, margin);
        speculation_.turns.clear();
        speculation_.texts.clear();
        for (const auto& piece : pieces) {
            speculation_.turns.push_back(piece.slice);
            speculation_.texts.push_back(piece.text);
        }
    }
}

}  // namespace ambient::diar
