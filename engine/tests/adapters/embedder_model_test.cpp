#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace sotto::models {
namespace {

// Research-generated fixtures: reference fbank features and the embeddings
// the same INT8 IR produced for them (export_sotto_fixtures.py). The staged
// copy must reproduce the embeddings before any pipeline code consumes it
constexpr const char* kFixtureDir = SOTTO_DIAR_FIXTURE_DIR;

struct Slice {
    std::string role;
    std::vector<float> features;  // frames x 80
    std::size_t frames;
    std::vector<float> reference;  // 192, unit norm
};

std::vector<Slice> LoadFixtures() {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "fixtures.json");
    if (!in.is_open()) throw std::runtime_error("missing diarisation fixtures");
    const auto meta = nlohmann::json::parse(in);

    std::vector<Slice> slices;
    for (const auto& entry : meta.at("slices")) {
        Slice slice;
        slice.role = entry.at("role");
        slice.frames = entry.at("frames");
        slice.reference = entry.at("embedding").get<std::vector<float>>();
        slice.features.resize(slice.frames * 80);
        std::ifstream bin(std::filesystem::path(kFixtureDir) / entry.at("file").get<std::string>(),
                          std::ios::binary);
        bin.read(reinterpret_cast<char*>(slice.features.data()),
                 static_cast<std::streamsize>(slice.features.size() * sizeof(float)));
        if (!bin) throw std::runtime_error("short fixture read");
        slices.push_back(std::move(slice));
    }
    return slices;
}

std::vector<float> Normalised(std::vector<float> v) {
    float norm = 0.0f;
    for (const float x : v) norm += x * x;
    norm = std::sqrt(norm);
    for (float& x : v) x /= norm;
    return v;
}

float Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return dot;
}

TEST(EmbedderModel, StagedExportReproducesTheResearchEmbeddings) {
    const ModelStore store{std::filesystem::path(SOTTO_MODELS_DIR)};
    OvRuntime runtime;
    auto loaded = runtime.Load(store, "diarisation", "default", "model.xml");
    EXPECT_EQ(loaded.device, "CPU") << "the embedder's researched placement";
    auto request = loaded.model.create_infer_request();

    const auto slices = LoadFixtures();
    ASSERT_GE(slices.size(), 4u);

    std::vector<std::vector<float>> embeddings;
    for (const auto& slice : slices) {
        ov::Tensor input(ov::element::f32, {1, slice.frames, 80},
                         const_cast<float*>(slice.features.data()));
        request.set_input_tensor(input);
        request.infer();
        const ov::Tensor output = request.get_output_tensor();
        ASSERT_EQ(output.get_size(), 192u);
        std::vector<float> embedding(output.data<float>(), output.data<float>() + 192);
        embedding = Normalised(std::move(embedding));

        EXPECT_GE(Cosine(embedding, slice.reference), 0.999f)
            << "staged IR must match the research reference on " << slice.role;
        embeddings.push_back(std::move(embedding));
    }

    // Functional structure: same-speaker slices embed closer than cross-speaker.
    // Means, not extremes - a mixed track leaks backchannel into some slices
    float same = 0.0f, cross = 0.0f;
    int same_n = 0, cross_n = 0;
    for (std::size_t a = 0; a < slices.size(); ++a) {
        for (std::size_t b = a + 1; b < slices.size(); ++b) {
            const float cos = Cosine(embeddings[a], embeddings[b]);
            if (slices[a].role == slices[b].role) {
                same += cos;
                ++same_n;
            } else {
                cross += cos;
                ++cross_n;
            }
        }
    }
    same /= static_cast<float>(same_n);
    cross /= static_cast<float>(cross_n);
    std::printf("embedder cosines: same-role mean %.4f, cross-role mean %.4f\n",
                static_cast<double>(same), static_cast<double>(cross));
    EXPECT_GT(same, cross + 0.15f) << "speaker identity must dominate the embedding space";
}

}  // namespace
}  // namespace sotto::models
