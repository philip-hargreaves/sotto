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

namespace detail {

namespace {

constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kHeaderV1 = 16;  // version, dims, sessions
constexpr std::size_t kHeaderV2 = 24;  // + enrolled_at

}  // namespace

std::vector<std::uint8_t> SerializeAnchor(const AnchorRecord& record) {
    std::vector<std::uint8_t> plain(kHeaderV2 + record.sum.size() * 4);
    const auto dims = static_cast<std::uint32_t>(record.sum.size());
    std::memcpy(plain.data(), &kVersion, 4);
    std::memcpy(plain.data() + 4, &dims, 4);
    std::memcpy(plain.data() + 8, &record.sessions, 8);
    std::memcpy(plain.data() + 16, &record.enrolled_at, 8);
    std::memcpy(plain.data() + kHeaderV2, record.sum.data(), record.sum.size() * 4);
    return plain;
}

std::optional<AnchorRecord> ParseAnchor(std::span<const std::uint8_t> plain) {
    if (plain.size() < kHeaderV1) return std::nullopt;
    std::uint32_t version = 0, dims = 0;
    std::memcpy(&version, plain.data(), 4);
    std::memcpy(&dims, plain.data() + 4, 4);
    const std::size_t header = version == 1 ? kHeaderV1 : version == kVersion ? kHeaderV2 : 0;
    if (header == 0 || plain.size() != header + static_cast<std::size_t>(dims) * 4) {
        return std::nullopt;
    }
    AnchorRecord record;
    std::memcpy(&record.sessions, plain.data() + 8, 8);
    if (version == kVersion) std::memcpy(&record.enrolled_at, plain.data() + 16, 8);
    record.sum.resize(dims);
    std::memcpy(record.sum.data(), plain.data() + header, static_cast<std::size_t>(dims) * 4);
    return record;
}

}  // namespace detail

AnchorStore::AnchorStore(const std::filesystem::path& root) : path_(root / "anchor.bin") {
    Load();
}

std::optional<std::vector<float>> AnchorStore::Anchor() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (record_.sum.empty()) return std::nullopt;
    double norm = 0.0;
    for (const float x : record_.sum) norm += static_cast<double>(x) * x;
    norm = std::sqrt(norm) + 1e-9;
    std::vector<float> anchor(record_.sum.size());
    for (std::size_t i = 0; i < anchor.size(); ++i) {
        anchor[i] = static_cast<float>(record_.sum[i] / norm);
    }
    return anchor;
}

std::uint64_t AnchorStore::Sessions() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return record_.sessions;
}

AnchorStatus AnchorStore::Status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    AnchorStatus status;
    status.sessions = record_.sessions;
    status.enrolled_at = record_.enrolled_at;
    if (record_.enrolled_at != 0) {
        status.origin = AnchorOrigin::kEnrolled;
    } else if (!record_.sum.empty()) {
        status.origin = AnchorOrigin::kAccrued;
    }
    return status;
}

void AnchorStore::Accrue(std::span<const float> voiceprint) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (voiceprint.empty()) return;
    if (record_.sum.size() != voiceprint.size()) {
        record_ = {};
        record_.sum.assign(voiceprint.size(), 0.0f);
    }
    for (std::size_t i = 0; i < voiceprint.size(); ++i) record_.sum[i] += voiceprint[i];
    record_.sessions += 1;
    Save();
}

void AnchorStore::Replace(std::span<const float> voiceprint, std::uint64_t enrolled_at) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (voiceprint.empty()) return;
    record_ = {};
    record_.sum.assign(voiceprint.begin(), voiceprint.end());
    for (float& x : record_.sum) x *= static_cast<float>(kEnrolWeight);
    record_.enrolled_at = enrolled_at;
    Save();
}

void AnchorStore::Clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    record_ = {};
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
    const auto record = detail::ParseAnchor({blob_out.pbData, blob_out.cbData});
    if (record.has_value()) {
        record_ = *record;
    } else {
        std::fprintf(stderr, "ambient-engine: anchor format mismatch, starting fresh\n");
    }
    SecureZeroMemory(blob_out.pbData, blob_out.cbData);
    LocalFree(blob_out.pbData);
}

void AnchorStore::Save() const {
    std::vector<std::uint8_t> plain = detail::SerializeAnchor(record_);
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
