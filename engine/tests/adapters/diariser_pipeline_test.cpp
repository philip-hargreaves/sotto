#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include "adapters/diarisation/speaker_diariser.hpp"

namespace sotto::diar {
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
    const models::ModelStore store{std::filesystem::path(SOTTO_MODELS_DIR)};
    models::OvRuntime runtime;
    SpeakerDiariser diariser(store, runtime);

    const auto start = std::chrono::steady_clock::now();
    const auto slices = diariser.Diarise(audio);
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);

    ASSERT_GT(slices.size(), 20u) << "a full consult yields a real turn structure";
    std::set<int> clusters;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        clusters.insert(slices[i].cluster);
        EXPECT_LT(slices[i].first_frame, slices[i].end_frame);
        EXPECT_LE(slices[i].end_frame, audio.size() + kSegWindowFrames);
        if (i > 0) EXPECT_GE(slices[i].first_frame, slices[i - 1].first_frame) << "time-sorted";
    }
    EXPECT_EQ(clusters.size(), 2u) << "doctor and patient, no phantom third voice";
    std::printf("diarised %zu slices, %zu clusters in %.1f s\n", slices.size(), clusters.size(),
                took.count());
}

}  // namespace
}  // namespace sotto::diar
