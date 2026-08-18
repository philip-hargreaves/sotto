#include "adapters/diarisation/diar_worker.hpp"

#include <algorithm>
#include <string>

#include "core/diar_capture.hpp"
#include "core/diar_regions.hpp"
#include "core/slice_refinement.hpp"

namespace sotto::diar {

DiarWorker::DiarWorker(const models::ModelStore& store, models::OvRuntime& runtime)
    : vad_(store, runtime), segmenter_(store, runtime), embedder_(store, runtime) {
    vad_.Reset();
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

    // Whole hops only; zero-padding the final partial hop is finalise's
    // job - only it knows the audio has ended
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

    // The slices behind the frontier, exactly as finalise will cut them:
    // seg change points plus settled turn edges
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
    embeddings.reserve(slices.size());
    for (const auto& slice : slices) embeddings.push_back(EmbedSlice(audio, slice));

    // Re-split completed turns across chain boundaries now; an aborted or
    // unsplit turn stores empty pieces so finalise knows it stands
    const auto chains = ChainSlices(slices, embeddings);
    for (const auto& turn : turns) {
        const std::uint64_t t1 = turn.first_frame + turn.frame_count;
        if (t1 > settled) continue;
        if (s.resplit_pieces.contains({turn.first_frame, turn.frame_count})) continue;
        std::vector<asr::Turn> pieces;
        auto cuts = ChainCutPoints(turn.first_frame, t1, chains);
        if (!cuts.empty()) {
            bool ok = true;
            std::uint64_t a = turn.first_frame;
            cuts.push_back(t1);
            for (const auto b : cuts) {
                if (b <= a || b - a < kResplitMinClipFrames || b > audio.size()) {
                    ok = false;
                    break;
                }
                asr::Turn piece;
                piece.first_frame = a;
                piece.frame_count = b - a;
                piece.text = decode(audio.subspan(a, b - a), a);
                if (piece.text.empty() || detail::MaxNgramRepeat(piece.text) >= kResplitMaxRepeat) {
                    ok = false;
                    break;
                }
                pieces.push_back(std::move(piece));
                a = b;
            }
            if (!ok) pieces.clear();
        }
        s.resplit_pieces[{turn.first_frame, turn.frame_count}] = std::move(pieces);
    }
}

}  // namespace sotto::diar
