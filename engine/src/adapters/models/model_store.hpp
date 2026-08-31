#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sotto::models {

// Streaming SHA-256, lowercase hex; shared by verification and test tooling
std::string Sha256File(const std::filesystem::path& path);

struct ModelInfo {
    std::string id;
    std::string task;     // asr | vad | diarisation | notes
    std::string tier;     // default | accuracy | constrained
    std::string device;   // GPU | CPU | NPU
    std::string licence;  // SPDX id
    std::filesystem::path dir;
    std::map<std::string, std::string> file_hashes;  // filename -> sha256 hex
};

// Per-model manifest.json dirs under one root; parsing fail-closed,
// hashing on demand. No OpenVINO here
class ModelStore {
   public:
    explicit ModelStore(const std::filesystem::path& root);

    const std::vector<ModelInfo>& List() const {
        return models_;
    }

    // The one model serving a role; ambiguity and absence are loud
    const ModelInfo& Resolve(std::string_view task, std::string_view tier) const;

    // Throws naming the first file that is missing or does not match its hash
    void Verify(const ModelInfo& model) const;

   private:
    std::vector<ModelInfo> models_;
};

}  // namespace sotto::models
