// Dev evaluation tool, never shipped. Default: production diarisation over
// one wav, "start end cluster" per slice in seconds (the attribution
// scorer's format). --turn-cuts also splits slices at transcribed-turn
// boundaries (the A/B). --roles runs the full finalise flow - ASR, text
// assignment, cold-start naming - and prints roles, margin and per-cluster
// voiceprints for the role-acceptance scorer
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_clustering.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/endpointer.hpp"
#include "core/role_naming.hpp"
#include "core/slice_refinement.hpp"
#include "core/speaker_attribution.hpp"
#include "core/turn_reconcile.hpp"
#include "core/turn_resplit.hpp"

namespace {

std::vector<float> LoadWav(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing wav: ") + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

struct CollectingSink : sotto::asr::ITurnSink {
    std::mutex mutex;
    std::vector<sotto::asr::Turn> turns;

    void OnTurn(const sotto::asr::Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }
};

std::vector<sotto::asr::Turn> Transcribe(const sotto::models::ModelStore& store,
                                         sotto::models::OvRuntime& runtime,
                                         sotto::asr::WhisperTranscriber& transcriber,
                                         const std::vector<float>& audio) {
    sotto::audio::SileroVad vad(store, runtime);
    sotto::audio::Endpointer endpointer(vad);
    CollectingSink sink;
    transcriber.Begin(sink);
    for (const auto& window : endpointer.Push(audio)) {
        transcriber.Submit(window.frames, window.first_frame, window.first_new_frame);
    }
    if (const auto tail = endpointer.Flush()) {
        transcriber.Submit(tail->frames, tail->first_frame, tail->first_new_frame);
    }
    transcriber.Finish();
    return sink.turns;
}

std::string Decode(sotto::asr::WhisperTranscriber& transcriber, std::span<const float> clip,
                   std::uint64_t first_frame) {
    CollectingSink sink;
    transcriber.Begin(sink);
    transcriber.Submit(clip, first_frame);
    transcriber.Finish();
    std::string text;
    for (const auto& turn : sink.turns) {
        if (turn.text.empty()) continue;
        if (!text.empty()) text += ' ';
        text += turn.text;
    }
    return text;
}

// --amortise-probe: simulate the incremental worker. Every causal stage -
// VAD hops, seg windows, slice embeddings, straddler re-splits - runs while
// the recording streams, keyed on COMPLETED TURNS (turn boundaries are
// final once the next turn cannot start earlier, so slices below the last
// completed turn never change). Change detection during capture merges
// same-sounding adjacent slices into chains and cuts only across chain
// boundaries. Stop pays the tail, clustering, roles and attribution, and
// splices in the capture-phase re-splits instead of recomputing them
void AmortiseProbe(const sotto::models::ModelStore& store, sotto::models::OvRuntime& runtime,
                   sotto::asr::WhisperTranscriber& whisper, const std::vector<float>& audio,
                   const std::vector<sotto::asr::Turn>& asr_turns,
                   const std::vector<std::uint64_t>& cuts,
                   const std::vector<sotto::diar::LabelledSlice>& batch_slices,
                   const std::vector<sotto::asr::Turn>& batch_resplit) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    constexpr std::uint64_t kStepFrames = 5 * 16000;
    constexpr double kChangeDistance = 0.30;

    sotto::audio::SileroVad vad(store, runtime);
    sotto::diar::Segmenter segmenter(store, runtime);
    sotto::diar::SpeakerEmbedder embedder(store, runtime);
    vad.Reset();

    std::vector<float> probs;
    std::vector<float> hop(sotto::audio::kVadHopFrames, 0.0f);
    std::uint64_t hopped = 0;
    sotto::diar::SegResult seg;
    std::uint64_t seg_done = 0;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<float>> embedded;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<sotto::asr::Turn>> capture_pieces;
    std::size_t capture_embeds = 0, capture_decodes = 0;
    double capture_work = 0.0;

    const auto embed_slice = [&](const sotto::diar::Region& slice) -> const std::vector<float>& {
        auto& slot = embedded[{slice.first_frame, slice.end_frame}];
        if (slot.empty()) {
            const auto ranges = sotto::diar::EmbeddingRanges(slice, seg.overlap_spans);
            std::vector<float> clip;
            for (const auto& r : ranges) {
                clip.insert(clip.end(), audio.begin() + static_cast<std::ptrdiff_t>(r.first_frame),
                            audio.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(
                                                r.end_frame, audio.size())));
            }
            if (clip.size() >= 400) slot = embedder.Embed(clip);
        }
        return slot;
    };
    const auto refine_upto = [&](std::uint64_t settled_to) {
        auto cps = seg.change_points;
        for (auto c : cuts) {
            if (c <= settled_to) cps.push_back(c);
        }
        std::sort(cps.begin(), cps.end());
        // The final hop is zero-padded past the audio's end; the batch path
        // bounds regions by the true length, so bound identically or the last
        // region overshoots the settled line and is dropped whole
        auto regions = sotto::diar::SpeechRegions(
            probs, std::min<std::uint64_t>(hopped * sotto::audio::kVadHopFrames, audio.size()));
        std::erase_if(regions,
                      [&](const sotto::diar::Region& r) { return r.end_frame > settled_to; });
        return sotto::diar::RefineRegions(regions, cps);
    };

    // ---- capture phase
    for (std::uint64_t streamed = 0; streamed < audio.size(); streamed += kStepFrames) {
        const auto t0 = Clock::now();
        const std::uint64_t upto = std::min<std::uint64_t>(streamed + kStepFrames, audio.size());
        while ((hopped + 1) * sotto::audio::kVadHopFrames <= upto) {
            std::copy_n(
                audio.begin() + static_cast<std::ptrdiff_t>(hopped * sotto::audio::kVadHopFrames),
                sotto::audio::kVadHopFrames, hop.begin());
            probs.push_back(vad.SpeechProbability(hop));
            ++hopped;
        }
        while (seg_done + sotto::diar::kSegWindowFrames <= upto) {
            auto part = segmenter.Run(
                std::span<const float>(audio).subspan(seg_done, sotto::diar::kSegWindowFrames));
            for (auto c : part.change_points) seg.change_points.push_back(c + seg_done);
            for (auto s : part.overlap_spans) {
                seg.overlap_spans.push_back({s.first_frame + seg_done, s.end_frame + seg_done});
            }
            seg_done += sotto::diar::kSegWindowFrames;
        }
        // Turn boundaries are final up to the last completed turn; seg change
        // points are final up to seg_done. Below both, slices never change
        std::uint64_t last_turn_end = 0;
        for (const auto& turn : asr_turns) {
            const auto end = turn.first_frame + turn.frame_count;
            if (end <= upto && end > last_turn_end) last_turn_end = end;
        }
        const std::uint64_t settled_to = std::min(seg_done, last_turn_end);
        if (settled_to == 0) {
            capture_work += seconds(t0, Clock::now());
            continue;
        }
        const auto slices = refine_upto(settled_to);
        for (const auto& slice : slices) {
            if (embedded.find({slice.first_frame, slice.end_frame}) == embedded.end()) {
                embed_slice(slice);
                ++capture_embeds;
            }
        }
        // Same-sounding adjacent slices merge into chains; a chain boundary
        // is a speaker-change candidate
        std::vector<std::pair<std::uint64_t, std::uint64_t>> chains;
        const std::vector<float>* prev_emb = nullptr;
        for (const auto& slice : slices) {
            const auto& e = embedded[{slice.first_frame, slice.end_frame}];
            if (e.empty()) continue;
            bool merge = false;
            if (!chains.empty() && prev_emb != nullptr) {
                double dot = 0.0;
                for (std::size_t d = 0; d < e.size(); ++d) dot += (*prev_emb)[d] * e[d];
                merge = 1.0 - dot < kChangeDistance;
                // Short slices carry noisy voiceprints: a break needs both
                // sides substantial, or it stays one chain
                const bool substantial = slice.end_frame - slice.first_frame >= 8000 &&
                                         chains.back().second - chains.back().first >= 8000;
                if (!substantial) merge = true;
            }
            if (merge) {
                chains.back().second = slice.end_frame;
            } else {
                chains.push_back({slice.first_frame, slice.end_frame});
            }
            prev_emb = &e;
        }
        // Re-split completed turns across chain boundaries (batch geometry:
        // each side of a cut keeps the researched minimums)
        for (const auto& turn : asr_turns) {
            const std::uint64_t t1 = turn.first_frame + turn.frame_count;
            if (t1 > settled_to) continue;
            if (capture_pieces.count({turn.first_frame, turn.frame_count}) != 0) continue;
            std::vector<std::uint64_t> change_cuts;
            for (std::size_t i = 1; i < chains.size(); ++i) {
                const std::uint64_t boundary = (chains[i - 1].second + chains[i].first) / 2;
                if (boundary > turn.first_frame + sotto::diar::kResplitMinShareFrames &&
                    boundary + sotto::diar::kResplitMinShareFrames < t1) {
                    change_cuts.push_back(boundary);
                }
            }
            std::vector<sotto::asr::Turn> pieces;
            if (!change_cuts.empty()) {
                bool ok = true;
                std::uint64_t a = turn.first_frame;
                change_cuts.push_back(t1);
                for (const auto b : change_cuts) {
                    if (b <= a || b - a < sotto::diar::kResplitMinClipFrames) {
                        ok = false;
                        break;
                    }
                    sotto::asr::Turn piece;
                    piece.first_frame = a;
                    piece.frame_count = b - a;
                    piece.text =
                        Decode(whisper, std::span<const float>(audio).subspan(a, b - a), a);
                    ++capture_decodes;
                    if (piece.text.empty()) {
                        ok = false;
                        break;
                    }
                    pieces.push_back(std::move(piece));
                    a = b;
                }
                if (!ok) pieces.clear();
            }
            capture_pieces[{turn.first_frame, turn.frame_count}] = std::move(pieces);
        }
        capture_work += seconds(t0, Clock::now());
    }

    // ---- stop
    const auto stop_start = Clock::now();
    while (hopped * sotto::audio::kVadHopFrames < audio.size()) {
        const std::uint64_t at = hopped * sotto::audio::kVadHopFrames;
        const std::size_t have =
            std::min<std::size_t>(sotto::audio::kVadHopFrames, audio.size() - at);
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(at), have, hop.begin());
        std::fill(hop.begin() + static_cast<std::ptrdiff_t>(have), hop.end(), 0.0f);
        probs.push_back(vad.SpeechProbability(hop));
        ++hopped;
    }
    if (seg_done < audio.size()) {
        auto part = segmenter.Run(std::span<const float>(audio).subspan(seg_done));
        for (auto c : part.change_points) seg.change_points.push_back(c + seg_done);
        for (auto s : part.overlap_spans) {
            seg.overlap_spans.push_back({s.first_frame + seg_done, s.end_frame + seg_done});
        }
        seg_done = audio.size();
    }
    const auto final_slices = refine_upto(audio.size());

    std::size_t stop_embeds = 0;
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    std::vector<sotto::diar::Region> kept;
    for (const auto& slice : final_slices) {
        if (embedded.find({slice.first_frame, slice.end_frame}) == embedded.end()) ++stop_embeds;
        const auto& e = embed_slice(slice);
        if (e.empty()) continue;
        embeddings.push_back(e);
        durations.push_back(slice.end_frame - slice.first_frame);
        kept.push_back(slice);
    }
    const auto clusters = sotto::diar::ClusterSpeakers(embeddings, durations);
    std::vector<sotto::diar::LabelledSlice> labelled;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        labelled.push_back({kept[i].first_frame, kept[i].end_frame, clusters.labels[i]});
    }

    // Overlap second-turns, exactly as the batch path emits them
    if (clusters.count >= 2) {
        const std::size_t base = labelled.size();
        for (std::size_t i = 0; i < base; ++i) {
            for (const auto& span : seg.overlap_spans) {
                const auto first = std::max(span.first_frame, labelled[i].first_frame);
                const auto end = std::min(span.end_frame, labelled[i].end_frame);
                if (end <= first || end - first < sotto::diar::kOverlapTurnMinFrames) continue;
                std::vector<float> clip(
                    audio.begin() + static_cast<std::ptrdiff_t>(first),
                    audio.begin() +
                        static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(end, audio.size())));
                const auto embedding = embedder.Embed(clip);
                ++stop_embeds;
                const int primary = labelled[i].cluster;
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
                  [](const sotto::diar::LabelledSlice& a, const sotto::diar::LabelledSlice& b) {
                      return a.first_frame < b.first_frame;
                  });
    }

    // Splice: capture-phase pieces stand; only turns the capture never
    // settled are re-split here, against the final clusters
    std::size_t stop_decodes = 0, spliced = 0;
    std::vector<sotto::asr::Turn> turns;
    std::vector<sotto::asr::Turn> tail;
    for (const auto& turn : asr_turns) {
        const auto it = capture_pieces.find({turn.first_frame, turn.frame_count});
        if (it != capture_pieces.end()) {
            if (it->second.empty()) {
                turns.push_back(turn);
            } else {
                for (const auto& piece : it->second) turns.push_back(piece);
                ++spliced;
            }
        } else {
            tail.push_back(turn);
        }
    }
    tail = sotto::diar::ResplitStraddles(tail, labelled, audio,
                                         [&](std::span<const float> clip, std::uint64_t first) {
                                             ++stop_decodes;
                                             return Decode(whisper, clip, first);
                                         });
    for (const auto& turn : tail) turns.push_back(turn);
    std::sort(turns.begin(), turns.end(), [](const sotto::asr::Turn& a, const sotto::asr::Turn& b) {
        return a.first_frame < b.first_frame;
    });

    const auto texts = sotto::diar::AssignSliceTexts(turns, labelled);
    std::vector<sotto::diar::RoleTurn> role_turns;
    for (std::size_t i = 0; i < labelled.size(); ++i) {
        role_turns.push_back(
            {labelled[i].cluster, labelled[i].end_frame - labelled[i].first_frame, texts[i]});
    }
    const auto named = sotto::diar::NameRoles(role_turns, clusters.count);
    const auto display = sotto::diar::BuildAttributedTurns(labelled, texts, named.role_of_cluster);
    // Production stop also builds the pair's voiceprints (anchor ranking and
    // accrual) - the two long-exposure embeds are a real stop cost
    const auto vp_start = Clock::now();
    for (int c = 0; c < clusters.count && c < 2; ++c) {
        (void)sotto::diar::ClusterVoiceprint(embedder, audio, labelled, c);
    }
    const double vp_s = seconds(vp_start, Clock::now());
    const double stop_s = seconds(stop_start, Clock::now());

    // ---- verification
    std::fprintf(stderr,
                 "amortise probe: capture %.1f s spread over the recording (%zu embeds, %zu "
                 "decodes, %zu turns spliced)\n",
                 capture_work, capture_embeds, capture_decodes, spliced);
    std::fprintf(stderr, "amortise probe: STOP %.2f s (%zu embeds, %zu decodes, %zu tail turns)\n",
                 stop_s, stop_embeds, stop_decodes, tail.size());
    std::ptrdiff_t mismatch = -1;
    if (labelled.size() != batch_slices.size()) {
        std::fprintf(stderr, "amortise probe: SLICE COUNT differs (%zu vs batch %zu)\n",
                     labelled.size(), batch_slices.size());
        bool diverged = false;
        for (std::size_t i = 0; i < std::min(labelled.size(), batch_slices.size()); ++i) {
            if (labelled[i].first_frame != batch_slices[i].first_frame ||
                labelled[i].end_frame != batch_slices[i].end_frame) {
                std::fprintf(
                    stderr, "  first divergence at %zu: probe [%.2f, %.2f) batch [%.2f, %.2f)\n", i,
                    labelled[i].first_frame / 16000.0, labelled[i].end_frame / 16000.0,
                    batch_slices[i].first_frame / 16000.0, batch_slices[i].end_frame / 16000.0);
                diverged = true;
                break;
            }
        }
        if (!diverged) {
            const auto& longer = labelled.size() > batch_slices.size()
                                     ? labelled
                                     : batch_slices;
            const char* who = labelled.size() > batch_slices.size() ? "probe" : "batch";
            for (std::size_t i = std::min(labelled.size(), batch_slices.size());
                 i < longer.size(); ++i) {
                std::fprintf(stderr, "  trailing extra in %s: [%.2f, %.2f) cluster %d\n", who,
                             longer[i].first_frame / 16000.0, longer[i].end_frame / 16000.0,
                             longer[i].cluster);
            }
        }
    } else {
        mismatch = 0;
        for (std::size_t i = 0; i < labelled.size(); ++i) {
            if (labelled[i].first_frame != batch_slices[i].first_frame ||
                labelled[i].end_frame != batch_slices[i].end_frame ||
                labelled[i].cluster != batch_slices[i].cluster) {
                ++mismatch;
            }
        }
        std::fprintf(stderr, "amortise probe: slices vs batch: %td mismatched of %zu\n", mismatch,
                     labelled.size());
    }
    std::size_t text_same = 0;
    std::map<std::string, int> batch_texts;
    for (const auto& t : batch_resplit) ++batch_texts[t.text];
    for (const auto& t : turns) {
        auto it = batch_texts.find(t.text);
        if (it != batch_texts.end() && it->second > 0) {
            --it->second;
            ++text_same;
        }
    }
    std::fprintf(stderr, "amortise probe: turn texts matching batch: %zu of %zu (batch %zu)\n",
                 text_same, turns.size(), batch_resplit.size());
    // One line per consult for the verification driver to parse
    std::fprintf(stderr,
                 "PROBE audio=%.1f capture=%.1f cap_embeds=%zu cap_decodes=%zu spliced=%zu "
                 "stop=%.3f vp=%.3f stop_embeds=%zu stop_decodes=%zu tail=%zu slices=%zu "
                 "batch_slices=%zu mismatch=%td text_match=%zu turns=%zu batch_turns=%zu\n",
                 audio.size() / 16000.0, capture_work, capture_embeds, capture_decodes, spliced,
                 stop_s, vp_s, stop_embeds, stop_decodes, tail.size(), labelled.size(),
                 batch_slices.size(), mismatch, text_same, turns.size(), batch_resplit.size());
    (void)display;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: diar_eval_runner <models-dir> <audio.wav> [--roles] [--turn-cuts]\n");
        return 2;
    }
    bool roles = false, turn_cuts = false, amortise = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--roles") == 0) roles = true;
        if (std::strcmp(argv[i], "--turn-cuts") == 0) turn_cuts = true;
        if (std::strcmp(argv[i], "--amortise-probe") == 0) {
            roles = true;
            amortise = true;
        }
    }
    try {
        const sotto::models::ModelStore store{std::filesystem::path(argv[1])};
        sotto::models::OvRuntime runtime;
        // A throwaway anchor root: evaluation must never touch a real anchor
        const auto anchor_root = std::filesystem::temp_directory_path() / "sotto-diar-eval";
        std::filesystem::create_directories(anchor_root);
        sotto::diar::SpeakerDiariser diariser(store, runtime, anchor_root);
        const auto audio = LoadWav(argv[2]);

        std::vector<sotto::asr::Turn> turns;
        std::unique_ptr<sotto::asr::WhisperTranscriber> whisper;
        if (roles || turn_cuts) {
            whisper = std::make_unique<sotto::asr::WhisperTranscriber>(store, runtime);
            turns = Transcribe(store, runtime, *whisper, audio);
        }

        sotto::diar::ReconcileTurns(turns);
        std::vector<std::uint64_t> cuts;
        if (turn_cuts || roles) {
            for (const auto& turn : turns) {
                cuts.push_back(turn.first_frame);
                cuts.push_back(turn.first_frame + turn.frame_count);
            }
        }
        const auto result = diariser.Diarise(audio, cuts);

        if (roles) {
            const auto before = std::chrono::steady_clock::now();
            const std::size_t was = turns.size();
            turns = sotto::diar::ResplitStraddles(
                turns, result.slices, audio,
                [&whisper](std::span<const float> clip, std::uint64_t first) {
                    return Decode(*whisper, clip, first);
                });
            const auto took =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - before);
            std::fprintf(stderr, "resplit: %zu -> %zu turns in %.1f s\n", was, turns.size(),
                         took.count());
        }

        if (amortise) {
            AmortiseProbe(store, runtime, *whisper, audio, turns, cuts, result.slices, turns);
            return 0;
        }

        if (!roles) {
            for (const auto& slice : result.slices) {
                std::printf("%.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                            static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
            }
            return 0;
        }

        const auto texts = sotto::diar::AssignSliceTexts(turns, result.slices);
        std::vector<sotto::diar::RoleTurn> role_turns;
        for (std::size_t i = 0; i < result.slices.size(); ++i) {
            role_turns.push_back({result.slices[i].cluster,
                                  result.slices[i].end_frame - result.slices[i].first_frame,
                                  texts[i]});
        }
        const auto named = sotto::diar::NameRoles(role_turns, result.cluster_count);

        std::printf("DOCTOR %d\nMARGIN %.4f\n", named.doctor_cluster, named.margin);
        for (std::size_t c = 0; c < named.role_of_cluster.size(); ++c) {
            std::printf("ROLE %zu %s\n", c, named.role_of_cluster[c].c_str());
        }
        for (int c = 0; c < result.cluster_count; ++c) {
            const auto voiceprint =
                sotto::diar::ClusterVoiceprint(diariser.Embedder(), audio, result.slices, c);
            if (voiceprint.empty()) continue;
            std::printf("VP %d", c);
            for (const float x : voiceprint) std::printf(" %.6f", x);
            std::printf("\n");
        }
        for (const auto& slice : result.slices) {
            std::printf("SLICE %.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                        static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
        }
        for (const auto& turn :
             sotto::diar::BuildAttributedTurns(result.slices, texts, named.role_of_cluster)) {
            std::printf("TURN %s\t%s\n", turn.speaker.c_str(), turn.text.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "diar_eval_runner: %s\n", e.what());
        return 1;
    }
}
