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

// Enumerates per-model manifest.json directories under one root. Parsing is
// eager and fail-closed (a corrupt manifest is a broken install); hashing is
// on demand via Verify, since models run to gigabytes. No OpenVINO here:
// this class deals in files and hashes, the runtime compiles what it clears.
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
