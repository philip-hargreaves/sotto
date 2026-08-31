#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include "adapters/diarisation/cluster_voiceprint.hpp"
#include "adapters/diarisation/speaker_diariser.hpp"

namespace ambient::diar {
namespace {

// Shape checks on the assembled chain over a real consultation. Quality is
// deliberately not asserted here: the acceptance is the blinded judge
// protocol, recorded in the provenance addendum
constexpr const char* kWav =
    "C:/dev/intelliscribe/bench/transcription/mixed/day1_consultation01_mixed.wav";

std::vector<float> LoadWav(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error(std::string("missing dev wav: ") + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

TEST(DiariserPipeline, ADoctorPatientConsultDiarisesToTwoSpeakers) {
    const auto audio = LoadWav(kWav);
    const models::ModelStore store{std::filesystem::path(AMBIENT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto anchor_root = std::filesystem::temp_directory_path() / "ambient-diar-pipeline-test";
    std::filesystem::create_directories(anchor_root);
    SpeakerDiariser diariser(store, runtime, anchor_root);

    const auto start = std::chrono::steady_clock::now();
    const auto result = diariser.Diarise(audio);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    const auto& slices = result.slices;

    ASSERT_GT(slices.size(), 20u) << "a full consult yields a real turn structure";
    std::set<int> clusters;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        clusters.insert(slices[i].cluster);
        EXPECT_LT(slices[i].first_frame, slices[i].end_frame);
        EXPECT_LE(slices[i].end_frame, audio.size() + kSegWindowFrames);
        if (i > 0) EXPECT_GE(slices[i].first_frame, slices[i - 1].first_frame) << "time-sorted";
    }
    EXPECT_EQ(clusters.size(), 2u) << "doctor and patient, no phantom third voice";
    EXPECT_EQ(result.cluster_count, 2);
    EXPECT_TRUE(diariser.AnchorSimilarities(audio, slices, result.cluster_count).empty())
        << "no anchor has accrued in a fresh root";
    std::printf("diarised %zu slices, %zu clusters in %.1f s\n", slices.size(), clusters.size(),
                took.count());
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
}

// The voiceprints moved off the Diarise path (they now overlap the GPU turn
// decode) and AccrueDoctor reuses them: the numbers must be the ones the
// reference method produces, and the reuse must actually skip the embed
TEST(DiariserPipeline, AnchorSimilaritiesAreTheReferenceVoiceprintsAndAccrueReusesThem) {
    const auto audio = LoadWav(kWav);
    const models::ModelStore store{std::filesystem::path(AMBIENT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto anchor_root = std::filesystem::temp_directory_path() / "ambient-diar-anchor-test";
    std::filesystem::remove_all(anchor_root);
    std::filesystem::create_directories(anchor_root);
    SpeakerDiariser diariser(store, runtime, anchor_root);

    // Session one teaches the anchor from cluster 0
    const auto first = diariser.Diarise(audio);
    ASSERT_EQ(first.cluster_count, 2);
    diariser.AccrueDoctor(audio, first.slices, 0);

    // Session two: similarities equal the reference voiceprint against the anchor
    const auto second = diariser.Diarise(audio);
    const auto similarity = diariser.AnchorSimilarities(audio, second.slices, second.cluster_count);
    ASSERT_EQ(similarity.size(), 2u);
    AnchorStore anchors(anchor_root);
    const auto anchor = anchors.Anchor();
    ASSERT_TRUE(anchor.has_value());
    for (int c = 0; c < 2; ++c) {
        const auto voiceprint = ClusterVoiceprint(diariser.Embedder(), audio, second.slices, c);
        ASSERT_FALSE(voiceprint.empty());
        double dot = 0.0;
        for (std::size_t d = 0; d < voiceprint.size(); ++d) {
            dot += static_cast<double>(voiceprint[d]) * (*anchor)[d];
        }
        EXPECT_DOUBLE_EQ(similarity[static_cast<std::size_t>(c)], dot);
    }
    EXPECT_GT(similarity[0], similarity[1]) << "the taught cluster ranks nearer";

    // Accrue reuses the voiceprint just computed: no second embed of the cluster
    const auto start = std::chrono::steady_clock::now();
    diariser.AccrueDoctor(audio, second.slices, 0);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    EXPECT_LT(took.count(), 0.05) << "a re-embed of the cluster takes hundreds of ms";
    std::error_code ec;
    std::filesystem::remove_all(anchor_root, ec);
}

// The bit-identity acceptance for capture-fed finalise: feeding the same
// audio incrementally must change nothing about the diarised output
TEST(DiariserPipeline, CaptureFedDiariseMatchesBatchExactly) {
    const auto audio = LoadWav(kWav);
    const models::ModelStore store{std::filesystem::path(AMBIENT_MODELS_DIR)};
    models::OvRuntime runtime;
    const auto root = std::filesystem::temp_directory_path() / "ambient-diar-capture-test";
    std::filesystem::create_directories(root);
    SpeakerDiariser batch(store, runtime, root / "a");
    SpeakerDiariser fed(store, runtime, root / "b");

    // Synthetic reconciled turns, 6 s each; edges double as slice cuts
    std::vector<asr::Turn> turns;
    std::vector<std::uint64_t> boundaries;
    for (std::uint64_t f = 0; f + 96000 <= audio.size(); f += 96000) {
        asr::Turn turn;
        turn.first_frame = f;
        turn.frame_count = 96000;
        turn.text = "spoken words";
        turns.push_back(std::move(turn));
        boundaries.push_back(f);
        boundaries.push_back(f + 96000);
    }
    const DecodeClipFn decode = [](std::span<const float>, std::uint64_t) {
        return std::string("re-decoded");
    };
    for (std::uint64_t fed_to = 80000; fed_to < audio.size(); fed_to += 80000) {  // 5 s steps
        fed.Advance(std::span(audio).first(fed_to), turns, decode);
    }

    const auto want = batch.Diarise(audio, boundaries);
    const auto got = fed.Diarise(audio, boundaries);

    ASSERT_EQ(got.slices.size(), want.slices.size());
    for (std::size_t i = 0; i < got.slices.size(); ++i) {
        EXPECT_EQ(got.slices[i].first_frame, want.slices[i].first_frame);
        EXPECT_EQ(got.slices[i].end_frame, want.slices[i].end_frame);
        EXPECT_EQ(got.slices[i].cluster, want.slices[i].cluster);
    }
    EXPECT_EQ(got.cluster_count, want.cluster_count);
    EXPECT_FALSE(fed.TakeTurnTexts().empty()) << "capture speculated turn texts";
    EXPECT_TRUE(batch.TakeTurnTexts().empty()) << "nothing accumulates without Advance";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

}  // namespace
}  // namespace ambient::diar
