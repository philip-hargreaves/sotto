// Dev evaluation tool, never shipped. Default: production diarisation over
// one wav, "start end cluster" per slice in seconds (the attribution
// scorer's format). --turn-cuts also splits slices at transcribed-turn
// boundaries (the A/B). --roles runs the full finalise flow - ASR, text
// assignment, cold-start naming - and prints roles, margin and per-cluster
// voiceprints for the role-acceptance scorer
#include <algorithm>
#include <cctype>
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
#include "core/role_naming.hpp"
#include "core/speaker_attribution.hpp"
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

std::size_t MaxNgramRepeat(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!word.empty()) words.push_back(word);
    if (words.size() < 5) return 0;
    std::map<std::string, std::size_t> counts;
    std::size_t worst = 0;
    for (std::size_t i = 0; i + 5 <= words.size(); ++i) {
        std::string gram;
        for (std::size_t j = 0; j < 5; ++j) gram += words[i + j] + ' ';
        worst = std::max(worst, ++counts[gram]);
    }
    return worst;
}

// The re-split probe (ported from the research finalise): a transcribed
// turn straddling two different-speaker slices has its AUDIO cut at the
// slice boundary midpoints and each piece re-transcribed, so no word can
// land on the wrong speaker. Pieces under 0.4 s or degenerate decodes
// abort the whole turn back to the original
constexpr std::uint64_t kResplitMinShareFrames = 4800;  // 0.3 s to own a share
constexpr std::uint64_t kResplitMinClipFrames = 6400;   // 0.4 s: shorter clips hallucinate
constexpr std::size_t kResplitMaxRepeat = 4;

std::vector<sotto::asr::Turn> ResplitStraddles(
    sotto::asr::WhisperTranscriber& whisper, const std::vector<float>& audio,
    const std::vector<sotto::asr::Turn>& turns,
    const std::vector<sotto::diar::LabelledSlice>& slices, int& done, int& skipped) {
    std::vector<sotto::asr::Turn> out;
    for (const auto& turn : turns) {
        const std::uint64_t t0 = turn.first_frame;
        const std::uint64_t t1 = turn.first_frame + turn.frame_count;
        std::vector<const sotto::diar::LabelledSlice*> parts;
        for (const auto& slice : slices) {
            const std::uint64_t lo = std::max(slice.first_frame, t0);
            const std::uint64_t hi = std::min(slice.end_frame, t1);
            if (hi <= lo || hi - lo < kResplitMinShareFrames) continue;
            if (!parts.empty()) {
                const auto& prev = *parts.back();
                const std::uint64_t nlo = std::max(prev.first_frame, slice.first_frame);
                const std::uint64_t nhi = std::min(prev.end_frame, slice.end_frame);
                const std::uint64_t dur =
                    std::max<std::uint64_t>(slice.end_frame - slice.first_frame, 1);
                if (nhi > nlo && nhi - nlo > dur / 2) continue;  // nested overlap turn
                if (slice.cluster == prev.cluster) continue;
            }
            parts.push_back(&slice);
        }
        if (parts.size() < 2) {
            out.push_back(turn);
            continue;
        }

        std::vector<sotto::asr::Turn> pieces;
        bool ok = true;
        for (std::size_t p = 0; p < parts.size() && ok; ++p) {
            std::uint64_t a = p == 0 ? t0 : (parts[p - 1]->end_frame + parts[p]->first_frame) / 2;
            std::uint64_t b =
                p + 1 == parts.size() ? t1 : (parts[p]->end_frame + parts[p + 1]->first_frame) / 2;
            a = std::clamp(a, t0, t1);
            b = std::clamp(b, a, t1);
            if (b - a < kResplitMinClipFrames || b > audio.size()) {
                ok = false;
                break;
            }
            const std::string text =
                Decode(whisper, std::span<const float>(audio).subspan(a, b - a), a);
            if (text.empty() || MaxNgramRepeat(text) >= kResplitMaxRepeat) {
                ok = false;
                break;
            }
            sotto::asr::Turn piece;
            piece.first_frame = a;
            piece.frame_count = b - a;
            piece.text = text;
            pieces.push_back(std::move(piece));
        }
        if (ok) {
            for (auto& piece : pieces) out.push_back(std::move(piece));
            ++done;
        } else {
            out.push_back(turn);
            ++skipped;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: diar_eval_runner <models-dir> <audio.wav> [--roles] [--turn-cuts]\n");
        return 2;
    }
    bool roles = false, turn_cuts = false, resplit = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--roles") == 0) roles = true;
        if (std::strcmp(argv[i], "--turn-cuts") == 0) turn_cuts = true;
        if (std::strcmp(argv[i], "--resplit") == 0) resplit = true;
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

        std::vector<std::uint64_t> cuts;
        if (turn_cuts) {
            for (const auto& turn : turns) {
                cuts.push_back(turn.first_frame);
                cuts.push_back(turn.first_frame + turn.frame_count);
            }
        }
        const auto result = diariser.Diarise(audio, cuts);

        if (resplit) {
            sotto::diar::ReconcileTurns(turns);
            int done = 0, skipped = 0;
            turns = ResplitStraddles(*whisper, audio, turns, result.slices, done, skipped);
            std::fprintf(stderr, "resplit: %d re-transcribed, %d skipped\n", done, skipped);
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
