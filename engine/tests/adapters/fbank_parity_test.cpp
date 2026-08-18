#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/diarisation/fbank.hpp"

namespace sotto::diar {
namespace {

// The fixture features are torchaudio output (the research reference), so
// this is a direct torch-vs-C++ front-end parity check on real speech.
// Pass criteria are the research port's own gate: identical frame count,
// p99 abs diff < 1e-4, max abs diff < 1e-2
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

TEST(FbankParity, MatchesTheTorchReferenceOnRealSpeech) {
    std::ifstream in(std::filesystem::path(kFixtureDir) / "fixtures.json");
    ASSERT_TRUE(in.is_open()) << "missing diarisation fixtures";
    const auto meta = nlohmann::json::parse(in);
    const auto audio = LoadWav(meta.at("wav"));

    for (const auto& entry : meta.at("slices")) {
        const auto first = static_cast<std::size_t>(entry.at("start_s").get<double>() * 16000.0);
        const auto last = static_cast<std::size_t>(entry.at("end_s").get<double>() * 16000.0);
        const auto computed =
            EmbedderFbank(std::span<const float>(audio).subspan(first, last - first));

        const std::size_t frames = entry.at("frames");
        ASSERT_EQ(computed.frames, frames) << entry.at("file").get<std::string>();

        std::vector<float> reference(frames * kMelBins);
        std::ifstream bin(std::filesystem::path(kFixtureDir) / entry.at("file").get<std::string>(),
                          std::ios::binary);
        bin.read(reinterpret_cast<char*>(reference.data()),
                 static_cast<std::streamsize>(reference.size() * sizeof(float)));
        ASSERT_TRUE(bin.good());

        std::vector<float> diffs(reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            diffs[i] = std::abs(computed.values[i] - reference[i]);
        }
        std::sort(diffs.begin(), diffs.end());
        const float p99 = diffs[static_cast<std::size_t>(0.99 * static_cast<double>(diffs.size()))];
        const float max = diffs.back();
        EXPECT_LT(p99, 1e-4f) << entry.at("file").get<std::string>();
        EXPECT_LT(max, 1e-2f) << entry.at("file").get<std::string>();
    }
}

}  // namespace
}  // namespace sotto::diar
