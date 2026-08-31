#include "adapters/diarisation/cluster_voiceprint.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"

namespace ambient::diar {
namespace {

// Real-audio check of the steady-state naming mechanism: voiceprints built
// from one speaker's slices must rank nearer an anchor accrued from that
// speaker than the other speaker's voiceprint does
constexpr const char* kFixtureDir = AMBIENT_DIAR_FIXTURE_DIR;

std::vector<float> LoadWav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("missing dev wav: " + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

float Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;
}

TEST(ClusterVoiceprint, TheAccruedAnchorRanksItsOwnSpeakerNearer) {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "fixtures.json");
    ASSERT_TRUE(in.is_open());
    const auto meta = nlohmann::json::parse(in);
    const auto audio = LoadWav(meta.at("wav"));

    // Fixture slices as labelled slices: doctor cluster 0, patient cluster 1
    std::vector<LabelledSlice> slices;
    for (const auto& entry : meta.at("slices")) {
        slices.push_back({static_cast<std::uint64_t>(entry.at("start_s").get<double>() * 16000.0),
                          static_cast<std::uint64_t>(entry.at("end_s").get<double>() * 16000.0),
                          entry.at("role") == "doctor" ? 0 : 1});
    }

    const models::ModelStore store{std::filesystem::path(AMBIENT_MODELS_DIR)};
    models::OvRuntime runtime;
    SpeakerEmbedder embedder(store, runtime);

    const auto doctor = ClusterVoiceprint(embedder, audio, slices, 0);
    const auto patient = ClusterVoiceprint(embedder, audio, slices, 1);
    ASSERT_EQ(doctor.size(), kEmbeddingDims);
    ASSERT_EQ(patient.size(), kEmbeddingDims);

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "ambient-anchor-rank-test";
    std::filesystem::create_directories(tmp);
    AnchorStore anchors(tmp);
    anchors.Clear();
    anchors.Accrue(doctor);
    const auto anchor = anchors.Anchor();
    ASSERT_TRUE(anchor.has_value());

    EXPECT_GT(Cosine(*anchor, doctor), Cosine(*anchor, patient))
        << "relative match must pick the accrued speaker";
    anchors.Clear();
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

}  // namespace
}  // namespace ambient::diar
