// Dev evaluation tool. Default: production diarisation over
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
#include "adapters/diarisation/speaker_diariser.hpp"
#include "adapters/transcription/whisper_transcriber.hpp"
#include "adapters/vad/silero_vad.hpp"
#include "core/endpointer.hpp"
#include "core/per_turn.hpp"
#include "core/role_naming.hpp"
#include "core/turn_reconcile.hpp"

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

struct CollectingSink : ambient::asr::ITurnSink {
    std::mutex mutex;
    std::vector<ambient::asr::Turn> turns;

    void OnTurn(const ambient::asr::Turn& turn) override {
        const std::lock_guard<std::mutex> lock(mutex);
        turns.push_back(turn);
    }
};

std::vector<ambient::asr::Turn> Transcribe(const ambient::models::ModelStore& store,
                                           ambient::models::OvRuntime& runtime,
                                           ambient::asr::WhisperTranscriber& transcriber,
                                           const std::vector<float>& audio) {
    ambient::audio::SileroVad vad(store, runtime);
    ambient::audio::Endpointer endpointer(vad);
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

std::string Decode(ambient::asr::WhisperTranscriber& transcriber, std::span<const float> clip,
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

// --amortise-probe: drive the production capture path - a SpeakerDiariser
// fed in five-second steps with the audio and completed turns so far,
// exactly as the session controller feeds it - then time what a stop pays
// and verify the output is bit-identical to the batch pass
void AmortiseProbe(const ambient::models::ModelStore& store, ambient::models::OvRuntime& runtime,
                   ambient::asr::WhisperTranscriber& whisper, const std::vector<float>& audio,
                   const std::vector<ambient::asr::Turn>& reconciled,
                   const std::vector<std::uint64_t>& cuts,
                   const ambient::diar::DiariseResult& batch) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    constexpr std::uint64_t kStepFrames = 5 * 16000;

    const auto anchor_root = std::filesystem::temp_directory_path() / "ambient-diar-eval-amortise";
    std::filesystem::create_directories(anchor_root);
    ambient::diar::SpeakerDiariser fed(store, runtime, anchor_root);
    std::size_t decodes = 0;
    const auto decode = [&whisper, &decodes](std::span<const float> clip, std::uint64_t first) {
        ++decodes;
        return Decode(whisper, clip, first);
    };

    // Capture: the controller's cadence, completed turns only
    double capture_s = 0.0;
    std::size_t ticks = 0;
    for (std::uint64_t upto = kStepFrames; upto < audio.size(); upto += kStepFrames) {
        std::vector<ambient::asr::Turn> so_far;
        for (const auto& turn : reconciled) {
            if (turn.first_frame + turn.frame_count <= upto) so_far.push_back(turn);
        }
        const auto t0 = Clock::now();
        fed.Advance(std::span<const float>(audio).first(upto), so_far, decode);
        capture_s += seconds(t0, Clock::now());
        ++ticks;
    }
    const std::size_t capture_decodes = decodes;

    // Stop: everything production's finalise pays, including the pair's
    // voiceprints for anchor ranking and accrual
    const auto stop_start = Clock::now();
    const auto result = fed.Diarise(audio, cuts);
    const auto cache = fed.TakeTurnTexts();
    const auto turns = ambient::diar::MergeByCluster(result.slices);
    std::size_t hits = 0;
    for (const auto& span : ambient::diar::DecodeSpans(turns, audio.size())) {
        if (cache.contains({span.first_frame, span.end_frame})) ++hits;
    }
    const auto turn_texts = ambient::diar::DecodeTurnTexts(turns, audio, decode, &cache);
    std::vector<ambient::diar::RoleTurn> role_turns;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        role_turns.push_back(
            {turns[i].cluster, turns[i].end_frame - turns[i].first_frame, turn_texts[i]});
    }
    const auto named = ambient::diar::NameRoles(role_turns, result.cluster_count);
    const auto vp_start = Clock::now();
    for (int c = 0; c < result.cluster_count && c < 2; ++c) {
        (void)ambient::diar::ClusterVoiceprint(fed.Embedder(), audio, result.slices, c);
    }
    const double vp_s = seconds(vp_start, Clock::now());
    const double stop_s = seconds(stop_start, Clock::now());
    const std::size_t stop_decodes = decodes - capture_decodes;

    // ---- verification against the batch pass
    std::ptrdiff_t mismatch = -1;
    if (result.slices.size() != batch.slices.size()) {
        std::fprintf(stderr, "amortise probe: SLICE COUNT differs (%zu vs batch %zu)\n",
                     result.slices.size(), batch.slices.size());
        for (std::size_t i = 0; i < std::min(result.slices.size(), batch.slices.size()); ++i) {
            if (result.slices[i].first_frame != batch.slices[i].first_frame ||
                result.slices[i].end_frame != batch.slices[i].end_frame) {
                std::fprintf(
                    stderr, "  first divergence at %zu: fed [%.2f, %.2f) batch [%.2f, %.2f)\n", i,
                    result.slices[i].first_frame / 16000.0, result.slices[i].end_frame / 16000.0,
                    batch.slices[i].first_frame / 16000.0, batch.slices[i].end_frame / 16000.0);
                break;
            }
        }
    } else {
        mismatch = 0;
        for (std::size_t i = 0; i < result.slices.size(); ++i) {
            if (result.slices[i].first_frame != batch.slices[i].first_frame ||
                result.slices[i].end_frame != batch.slices[i].end_frame ||
                result.slices[i].cluster != batch.slices[i].cluster) {
                ++mismatch;
            }
        }
        std::fprintf(stderr, "amortise probe: slices vs batch: %td mismatched of %zu\n", mismatch,
                     result.slices.size());
    }
    std::fprintf(stderr,
                 "amortise probe: capture %.1f s over %zu ticks (%zu decodes); STOP %.2f s "
                 "(vp %.2f, %zu decodes); cache %zu speculated, %zu of %zu turns hit\n",
                 capture_s, ticks, capture_decodes, stop_s, vp_s, stop_decodes, cache.size(), hits,
                 turns.size());
    // One line per consult for the verification driver to parse
    std::fprintf(stderr,
                 "PROBE audio=%.1f capture=%.1f ticks=%zu cap_decodes=%zu stop=%.3f vp=%.3f "
                 "stop_decodes=%zu slices=%zu batch_slices=%zu mismatch=%td turns=%zu "
                 "speculated=%zu hits=%zu\n",
                 audio.size() / 16000.0, capture_s, ticks, capture_decodes, stop_s, vp_s,
                 stop_decodes, result.slices.size(), batch.slices.size(), mismatch, turns.size(),
                 cache.size(), hits);
    // The attributed transcript, for the blinded judge
    for (std::size_t i = 0; i < turns.size(); ++i) {
        if (turn_texts[i].empty()) continue;
        const auto& role = named.role_of_cluster[static_cast<std::size_t>(turns[i].cluster)];
        std::printf("ATURN %s\t%s\n", role.c_str(), turn_texts[i].c_str());
    }
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
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
        const ambient::models::ModelStore store{std::filesystem::path(argv[1])};
        ambient::models::OvRuntime runtime;
        // A throwaway anchor root: evaluation must never touch a real anchor
        const auto anchor_root = std::filesystem::temp_directory_path() / "ambient-diar-eval";
        std::filesystem::create_directories(anchor_root);
        ambient::diar::SpeakerDiariser diariser(store, runtime, anchor_root);
        const auto audio = LoadWav(argv[2]);

        std::vector<ambient::asr::Turn> turns;
        std::unique_ptr<ambient::asr::WhisperTranscriber> whisper;
        if (roles || turn_cuts) {
            whisper = std::make_unique<ambient::asr::WhisperTranscriber>(store, runtime);
            turns = Transcribe(store, runtime, *whisper, audio);
        }

        ambient::diar::ReconcileTurns(turns);
        std::vector<std::uint64_t> cuts;
        if (turn_cuts || roles) {
            for (const auto& turn : turns) {
                cuts.push_back(turn.first_frame);
                cuts.push_back(turn.first_frame + turn.frame_count);
            }
        }
        const std::vector<ambient::asr::Turn> reconciled = turns;
        const auto result = diariser.Diarise(audio, cuts);

        if (amortise) {
            AmortiseProbe(store, runtime, *whisper, audio, reconciled, cuts, result);
            return 0;
        }

        if (!roles) {
            for (const auto& slice : result.slices) {
                std::printf("%.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                            static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
            }
            return 0;
        }

        // The production finalise: each merged turn decodes its own audio
        const auto before = std::chrono::steady_clock::now();
        const auto pturns = ambient::diar::MergeByCluster(result.slices);
        const auto turn_texts = ambient::diar::DecodeTurnTexts(
            pturns, audio, [&whisper](std::span<const float> clip, std::uint64_t first) {
                return Decode(*whisper, clip, first);
            });
        const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - before);
        std::fprintf(stderr, "per-turn: %zu turns decoded in %.1f s\n", pturns.size(),
                     took.count());
        std::vector<ambient::diar::RoleTurn> role_turns;
        for (std::size_t i = 0; i < pturns.size(); ++i) {
            role_turns.push_back(
                {pturns[i].cluster, pturns[i].end_frame - pturns[i].first_frame, turn_texts[i]});
        }
        const auto named = ambient::diar::NameRoles(role_turns, result.cluster_count);

        std::printf("DOCTOR %d\nMARGIN %.4f\n", named.doctor_cluster, named.margin);
        for (std::size_t c = 0; c < named.role_of_cluster.size(); ++c) {
            std::printf("ROLE %zu %s\n", c, named.role_of_cluster[c].c_str());
        }
        for (int c = 0; c < result.cluster_count; ++c) {
            const auto voiceprint =
                ambient::diar::ClusterVoiceprint(diariser.Embedder(), audio, result.slices, c);
            if (voiceprint.empty()) continue;
            std::printf("VP %d", c);
            for (const float x : voiceprint) std::printf(" %.6f", x);
            std::printf("\n");
        }
        for (const auto& slice : result.slices) {
            std::printf("SLICE %.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                        static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
        }
        for (std::size_t i = 0; i < pturns.size(); ++i) {
            if (turn_texts[i].empty()) continue;
            const auto& role = named.role_of_cluster[static_cast<std::size_t>(pturns[i].cluster)];
            std::printf("TURN %s\t%s\n", role.c_str(), turn_texts[i].c_str());
        }

        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "diar_eval_runner: %s\n", e.what());
        return 1;
    }
}
