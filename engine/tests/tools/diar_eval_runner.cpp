// Dev evaluation tool, never shipped: runs production diarisation over one
// wav and prints "start end cluster" per slice in seconds, the format the
// research scorer consumes (score_sotto_diar.py)
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "adapters/diarisation/speaker_diariser.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: diar_eval_runner <models-dir> <audio.wav>\n");
        return 2;
    }
    try {
        const sotto::models::ModelStore store{std::filesystem::path(argv[1])};
        sotto::models::OvRuntime runtime;
        // A throwaway anchor root: evaluation must never touch a real anchor
        const auto anchor_root = std::filesystem::temp_directory_path() / "sotto-diar-eval";
        std::filesystem::create_directories(anchor_root);
        sotto::diar::SpeakerDiariser diariser(store, runtime, anchor_root);
        const auto audio = LoadWav(argv[2]);
        for (const auto& slice : diariser.Diarise(audio).slices) {
            std::printf("%.3f %.3f %d\n", static_cast<double>(slice.first_frame) / 16000.0,
                        static_cast<double>(slice.end_frame) / 16000.0, slice.cluster);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "diar_eval_runner: %s\n", e.what());
        return 1;
    }
}
