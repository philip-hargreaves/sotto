#include "adapters/models/model_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace ambient::models {

namespace {

constexpr std::int64_t kManifestVersion = 1;

[[noreturn]] void Broken(const std::filesystem::path& dir, const std::string& why) {
    throw std::runtime_error("model manifest " + dir.string() + ": " + why);
}

}  // namespace

std::string Sha256File(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<char> chunk(1 << 20);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()),
                       static_cast<ULONG>(in.gcount()), 0);
    }

    unsigned char digest[32];
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    std::string hex;
    for (const unsigned char byte : digest) {
        constexpr char kDigits[] = "0123456789abcdef";
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 0xF];
    }
    return hex;
}

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

ModelInfo ParseManifest(const std::filesystem::path& dir) {
    std::ifstream in(dir / "manifest.json");
    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(in);
    } catch (const nlohmann::json::exception& e) {
        Broken(dir, e.what());
    }

    const std::int64_t version = manifest.value("manifestVersion", std::int64_t{0});
    if (version > kManifestVersion) Broken(dir, "manifest is newer than this build");
    if (version < 1) Broken(dir, "manifestVersion missing or invalid");

    ModelInfo info;
    info.dir = dir;
    try {
        info.id = manifest.at("id").get<std::string>();
        info.name = manifest.value("name", info.id);
        info.task = manifest.at("task").get<std::string>();
        info.tier = manifest.at("tier").get<std::string>();
        info.licence = manifest.at("licence").get<std::string>();
        const auto& runtime = manifest.at("runtime");
        info.device = runtime.at("device").get<std::string>();
        // Optional: absent means the LLM pipeline with no properties
        info.pipeline = runtime.value("pipeline", "llm");
        info.properties = runtime.value("properties", nlohmann::json::object());
        for (const auto& [name, hash] : manifest.at("files").items()) {
            info.file_hashes[name] = Lower(hash.get<std::string>());
        }
        // Optional: without sizes the load check is presence only. Named local:
        // iterating the items of a temporary json dangles
        const nlohmann::json sizes = manifest.value("bytes", nlohmann::json::object());
        for (const auto& [name, bytes] : sizes.items()) {
            info.file_bytes[name] = bytes.get<std::uintmax_t>();
        }
    } catch (const nlohmann::json::exception& e) {
        Broken(dir, e.what());
    }
    if (info.file_hashes.empty()) Broken(dir, "no files listed");
    // A pipeline this build cannot construct is a corrupt manifest for this build
    if (info.pipeline != "llm" && info.pipeline != "vlm") {
        Broken(dir, "unknown pipeline: " + info.pipeline);
    }
    if (!info.properties.is_object()) Broken(dir, "runtime.properties must be an object");
    for (const auto& [key, value] : info.properties.items()) {
        if (!value.is_primitive() || value.is_null()) {
            Broken(dir, "runtime.properties." + key + " must be a number, string or bool");
        }
    }
    return info;
}

}  // namespace

ModelStore::ModelStore(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) return;  // valid empty store
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        if (!std::filesystem::exists(entry.path() / "manifest.json")) continue;
        models_.push_back(ParseManifest(entry.path()));
    }
    std::sort(models_.begin(), models_.end(),
              [](const ModelInfo& a, const ModelInfo& b) { return a.id < b.id; });
}

const ModelInfo& ModelStore::Resolve(std::string_view task, std::string_view tier) const {
    const ModelInfo* found = nullptr;
    for (const auto& model : models_) {
        if (model.task != task || model.tier != tier) continue;
        if (found != nullptr) {
            throw std::runtime_error("both " + found->id + " and " + model.id + " claim " +
                                     std::string(task) + "/" + std::string(tier));
        }
        found = &model;
    }
    if (found == nullptr) {
        std::string installed;
        for (const auto& model : models_) {
            installed += " " + model.id + "(" + model.task + "/" + model.tier + ")";
        }
        throw std::runtime_error("no model for " + std::string(task) + "/" + std::string(tier) +
                                 "; installed:" + (installed.empty() ? " none" : installed));
    }
    return *found;
}

void ModelStore::Verify(const ModelInfo& model) const {
    for (const auto& [name, expected] : model.file_hashes) {
        const std::filesystem::path path = model.dir / name;
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error(model.id + ": missing file " + name);
        }
        const auto bytes = model.file_bytes.find(name);
        if (bytes != model.file_bytes.end() && std::filesystem::file_size(path) != bytes->second) {
            throw std::runtime_error(model.id + ": " + name + " is " +
                                     std::to_string(std::filesystem::file_size(path)) +
                                     " bytes, the manifest says " + std::to_string(bytes->second));
        }
    }
}

void ModelStore::VerifyHashes(const ModelInfo& model) const {
    Verify(model);
    for (const auto& [name, expected] : model.file_hashes) {
        if (Sha256File(model.dir / name) != expected) {
            throw std::runtime_error(model.id + ": " + name + " does not match its manifest hash");
        }
    }
}

}  // namespace ambient::models
