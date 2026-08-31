#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace sotto::diar {

// The clinician's accrued voiceprint, DPAPI-protected; a corrupt file
// resets to empty and rebuilds
class AnchorStore {
   public:
    explicit AnchorStore(const std::filesystem::path& root);

    // Unit-norm accrued voiceprint; empty before any session has accrued
    std::optional<std::vector<float>> Anchor() const;
    std::uint64_t Sessions() const;

    void Accrue(std::span<const float> voiceprint);
    void Clear();

   private:
    void Load();
    void Save() const;

    std::filesystem::path path_;
    std::vector<float> sum_;
    std::uint64_t count_ = 0;
};

}  // namespace sotto::diar
