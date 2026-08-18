#include "adapters/diarisation/speaker_embedder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sotto::diar {
namespace {

// The full stage against the research reference: our fbank + our runtime on
// raw audio must reproduce the fixture embeddings (research fbank + research
// runtime). A failure here with fbank parity green points at the glue
constexpr const char* kFixtureDir = SOTTO_DIAR_FIXTURE_DIR;

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

TEST(SpeakerEmbedder, ReproducesTheResearchEmbeddingsFromRawAudio) {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "fixtures.json");
    ASSERT_TRUE(in.is_open()) << "missing diarisation fixtures";
    const auto meta = nlohmann::json::parse(in);
    const auto audio = LoadWav(meta.at("wav"));

    const models::ModelStore store{std::filesystem::path(SOTTO_MODELS_DIR)};
    models::OvRuntime runtime;
    SpeakerEmbedder embedder(store, runtime);

    for (const auto& entry : meta.at("slices")) {
        const auto first = static_cast<std::size_t>(entry.at("start_s").get<double>() * 16000.0);
        const auto last = static_cast<std::size_t>(entry.at("end_s").get<double>() * 16000.0);
        const auto embedding =
            embedder.Embed(std::span<const float>(audio).subspan(first, last - first));
        ASSERT_EQ(embedding.size(), kEmbeddingDims);

        const auto reference = entry.at("embedding").get<std::vector<float>>();
        float cosine = 0.0f;
        for (std::size_t i = 0; i < kEmbeddingDims; ++i) cosine += embedding[i] * reference[i];
        EXPECT_GE(cosine, 0.999f) << entry.at("file").get<std::string>();
    }
}

TEST(SpeakerEmbedder, ASliceShorterThanOneFrameIsRefused) {
    const models::ModelStore store{std::filesystem::path(SOTTO_MODELS_DIR)};
    models::OvRuntime runtime;
    SpeakerEmbedder embedder(store, runtime);
    const std::vector<float> silence(399, 0.0f);
    EXPECT_THROW(embedder.Embed(silence), std::invalid_argument);
}

}  // namespace
}  // namespace sotto::diar
