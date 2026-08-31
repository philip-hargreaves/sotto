#include "adapters/diarisation/anchor_store.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <dpapi.h>
// clang-format on

namespace ambient::diar {

namespace {

constexpr std::uint32_t kVersion = 1;

}  // namespace

AnchorStore::AnchorStore(const std::filesystem::path& root) : path_(root / "anchor.bin") {
    Load();
}

std::optional<std::vector<float>> AnchorStore::Anchor() const {
    if (count_ == 0) return std::nullopt;
    double norm = 0.0;
    for (const float x : sum_) norm += static_cast<double>(x) * x;
    norm = std::sqrt(norm) + 1e-9;
    std::vector<float> anchor(sum_.size());
    for (std::size_t i = 0; i < sum_.size(); ++i) {
        anchor[i] = static_cast<float>(sum_[i] / norm);
    }
    return anchor;
}

std::uint64_t AnchorStore::Sessions() const {
    return count_;
}

void AnchorStore::Accrue(std::span<const float> voiceprint) {
    if (voiceprint.empty()) return;
    if (sum_.size() != voiceprint.size()) {
        sum_.assign(voiceprint.size(), 0.0f);
        count_ = 0;
    }
    for (std::size_t i = 0; i < voiceprint.size(); ++i) sum_[i] += voiceprint[i];
    count_ += 1;
    Save();
}

void AnchorStore::Clear() {
    sum_.clear();
    count_ = 0;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void AnchorStore::Load() {
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return;
    const std::vector<std::uint8_t> wrapped((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
    DATA_BLOB blob_in{static_cast<DWORD>(wrapped.size()),
                      const_cast<std::uint8_t*>(wrapped.data())};
    DATA_BLOB blob_out{};
    if (!CryptUnprotectData(&blob_in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &blob_out)) {
        std::fprintf(stderr, "ambient-engine: anchor unreadable, starting fresh\n");
        return;
    }
    const std::uint8_t* p = blob_out.pbData;
    const std::size_t size = blob_out.cbData;
    std::uint32_t version = 0, dims = 0;
    if (size >= 16) {
        std::memcpy(&version, p, 4);
        std::memcpy(&dims, p + 4, 4);
        std::uint64_t count = 0;
        std::memcpy(&count, p + 8, 8);
        if (version == kVersion && size == 16 + static_cast<std::size_t>(dims) * 4) {
            sum_.resize(dims);
            std::memcpy(sum_.data(), p + 16, static_cast<std::size_t>(dims) * 4);
            count_ = count;
        } else {
            std::fprintf(stderr, "ambient-engine: anchor format mismatch, starting fresh\n");
        }
    }
    SecureZeroMemory(blob_out.pbData, blob_out.cbData);
    LocalFree(blob_out.pbData);
}

void AnchorStore::Save() const {
    std::vector<std::uint8_t> plain(16 + sum_.size() * 4);
    const auto dims = static_cast<std::uint32_t>(sum_.size());
    std::memcpy(plain.data(), &kVersion, 4);
    std::memcpy(plain.data() + 4, &dims, 4);
    std::memcpy(plain.data() + 8, &count_, 8);
    std::memcpy(plain.data() + 16, sum_.data(), sum_.size() * 4);

    DATA_BLOB blob_in{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB blob_out{};
    if (!CryptProtectData(&blob_in, L"ambient clinician anchor", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob_out)) {
        throw std::runtime_error("CryptProtectData failed for the anchor");
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(blob_out.pbData),
              static_cast<std::streamsize>(blob_out.cbData));
    LocalFree(blob_out.pbData);
    if (!out) throw std::runtime_error("anchor write failed");
}

}  // namespace ambient::diar
