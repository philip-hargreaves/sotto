#include "adapters/models/model_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace ambient::models {
namespace {

// Known SHA-256 digests, so fixtures need no hashing of their own
constexpr const char* kHelloHash =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
constexpr const char* kEmptyHash =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path =
            std::filesystem::temp_directory_path() /
            ("ambient-models-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(path);
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void MakeModel(const std::filesystem::path& root, const std::string& id, const std::string& task,
               const std::string& tier, const std::string& weights_hash = kHelloHash,
               const std::string& manifest_version = "1") {
    const auto dir = root / id;
    std::filesystem::create_directories(dir);
    WriteFile(dir / "weights.bin", "hello");
    WriteFile(dir / "manifest.json",
              "{\"manifestVersion\": " + manifest_version + ", \"id\": \"" + id +
                  "\", \"task\": \"" + task + "\", \"tier\": \"" + tier +
                  "\", \"licence\": \"MIT\", \"runtime\": {\"device\": \"GPU\"},"
                  "\"files\": {\"weights.bin\": \"" +
                  weights_hash + "\"}}");
}

TEST(ModelStore, AMissingOrEmptyRootIsAValidEmptyStore) {
    TempRoot root;
    EXPECT_TRUE(ModelStore(root.path / "nowhere").List().empty());
    EXPECT_TRUE(ModelStore(root.path).List().empty());
}

TEST(ModelStore, EnumeratesStagedManifests) {
    TempRoot root;
    MakeModel(root.path, "whisper-turbo-int8", "asr", "default");
    MakeModel(root.path, "silero-vad", "vad", "default");

    const ModelStore store(root.path);
    ASSERT_EQ(store.List().size(), 2u);
    EXPECT_EQ(store.List()[0].id, "silero-vad");
    EXPECT_EQ(store.List()[1].id, "whisper-turbo-int8");
    EXPECT_EQ(store.List()[1].task, "asr");
    EXPECT_EQ(store.List()[1].device, "GPU");
    EXPECT_EQ(store.List()[1].licence, "MIT");
}

TEST(ModelStore, ADirectoryWithoutAManifestIsInvisible) {
    TempRoot root;
    std::filesystem::create_directories(root.path / "half-staged");
    WriteFile(root.path / "half-staged" / "weights.bin", "hello");
    WriteFile(root.path / "stray.txt", "not a model");

    EXPECT_TRUE(ModelStore(root.path).List().empty());
}

TEST(ModelStore, VerifyPassesWhenEveryHashMatches) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default");
    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
    EXPECT_NO_THROW(store.VerifyHashes(store.List()[0]));
}

// Integrity is established when a model arrives; the load-time check reads
// no bytes, so it is free at any size. The full hash stays for the tools
TEST(ModelStore, TheLoadTimeCheckReadsNoBytesTheFullCheckDoes) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default");
    WriteFile(root.path / "m" / "weights.bin", "jello");  // same size, different bytes

    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
    try {
        store.VerifyHashes(store.List()[0]);
        FAIL() << "the full check passed on changed content";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("weights.bin"), std::string::npos);
    }
}

// A manifest that records sizes makes truncation and replacement loud at load
TEST(ModelStore, AWrongSizeIsRefusedByNameWhenTheManifestRecordsSizes) {
    TempRoot root;
    const auto dir = root.path / "m";
    std::filesystem::create_directories(dir);
    WriteFile(dir / "weights.bin", "hello");
    WriteFile(dir / "manifest.json",
              std::string(R"({"manifestVersion": 1, "id": "m", "task": "asr", "tier": "default",)") +
                  R"( "licence": "MIT", "runtime": {"device": "GPU"}, "files": {"weights.bin": ")" +
                  kHelloHash + R"("}, "bytes": {"weights.bin": 5}})");
    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));

    WriteFile(dir / "weights.bin", "hel");  // truncated
    try {
        store.Verify(store.List()[0]);
        FAIL() << "a truncated file passed the load-time check";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("weights.bin"), std::string::npos);
        EXPECT_NE(what.find("3 bytes"), std::string::npos);
    }
}

TEST(ModelStore, AMissingFileIsRefusedByName) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default");
    std::filesystem::remove(root.path / "m" / "weights.bin");

    const ModelStore store(root.path);
    try {
        store.Verify(store.List()[0]);
        FAIL() << "verification passed with a listed file missing";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("missing file weights.bin"), std::string::npos);
    }
}

TEST(ModelStore, AnUnlistedExtraFileIsTolerated) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default");
    WriteFile(root.path / "m" / "notes.txt", "left by a human");

    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
}

TEST(ModelStore, UppercaseManifestHashesStillVerify) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default",
              "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824");
    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
}

TEST(ModelStore, FilesInSubdirectoriesVerify) {
    TempRoot root;
    std::filesystem::create_directories(root.path / "m" / "sub");
    WriteFile(root.path / "m" / "sub" / "config.json", "");
    WriteFile(root.path / "m" / "manifest.json",
              std::string("{\"manifestVersion\": 1, \"id\": \"m\", \"task\": \"asr\","
                          " \"tier\": \"default\", \"licence\": \"MIT\","
                          " \"runtime\": {\"device\": \"GPU\"},"
                          " \"files\": {\"sub/config.json\": \"") +
                  kEmptyHash + "\"}}");

    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
}

TEST(ModelStore, AnEmptyFileHashesCorrectly) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default", kEmptyHash);
    WriteFile(root.path / "m" / "weights.bin", "");
    const ModelStore store(root.path);
    EXPECT_NO_THROW(store.Verify(store.List()[0]));
}

TEST(ModelStore, ANewerManifestVersionIsRefused) {
    TempRoot root;
    MakeModel(root.path, "m", "asr", "default", kHelloHash, "2");
    EXPECT_THROW(ModelStore{root.path}, std::runtime_error);
}

TEST(ModelStore, AMalformedManifestNamesItsDirectory) {
    TempRoot root;
    std::filesystem::create_directories(root.path / "broken");
    WriteFile(root.path / "broken" / "manifest.json", "{not json");

    try {
        ModelStore store(root.path);
        FAIL() << "a corrupt manifest must be a loud failure";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("broken"), std::string::npos);
    }
}

TEST(ModelStore, AManifestMissingRequiredFieldsIsRefused) {
    TempRoot root;
    std::filesystem::create_directories(root.path / "partial");
    WriteFile(root.path / "partial" / "manifest.json",
              R"({"manifestVersion": 1, "id": "partial", "task": "asr"})");

    EXPECT_THROW(ModelStore{root.path}, std::runtime_error);
}

TEST(ModelStore, ResolvePicksByTaskAndTier) {
    TempRoot root;
    MakeModel(root.path, "whisper-turbo-int8", "asr", "default");
    MakeModel(root.path, "whisper-large-int8", "asr", "accuracy");

    const ModelStore store(root.path);
    EXPECT_EQ(store.Resolve("asr", "default").id, "whisper-turbo-int8");
    EXPECT_EQ(store.Resolve("asr", "accuracy").id, "whisper-large-int8");
}

TEST(ModelStore, AnAmbiguousRoleIsRefusedNamingBoth) {
    TempRoot root;
    MakeModel(root.path, "one", "asr", "default");
    MakeModel(root.path, "two", "asr", "default");

    const ModelStore store(root.path);
    try {
        store.Resolve("asr", "default");
        FAIL() << "two models claiming one role must be refused";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("one"), std::string::npos);
        EXPECT_NE(what.find("two"), std::string::npos);
    }
}

TEST(ModelStore, AnAbsentRoleReportsWhatIsInstalled) {
    TempRoot root;
    MakeModel(root.path, "silero-vad", "vad", "default");

    const ModelStore store(root.path);
    try {
        store.Resolve("asr", "default");
        FAIL() << "an absent role must be refused";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("silero-vad"), std::string::npos);
    }
}

// How a model loads is the manifest's fact: absent means the LLM pipeline
// with no properties, so every manifest written before the fields existed
// reads exactly as it did
TEST(ModelStore, RuntimeFieldsDefaultToTheLlmPipeline) {
    TempRoot root;
    MakeModel(root.path, "qwen3.5-9b-int4", "note", "default");

    const ModelStore store(root.path);
    const auto& info = store.Resolve("note", "default");
    EXPECT_EQ(info.pipeline, "llm");
    EXPECT_TRUE(info.properties.is_object());
    EXPECT_TRUE(info.properties.empty());
}

TEST(ModelStore, RuntimePipelineAndPropertiesAreRead) {
    TempRoot root;
    const auto dir = root.path / "qwen3.6-35b-a3b-int4";
    std::filesystem::create_directories(dir);
    WriteFile(dir / "weights.bin", "hello");
    WriteFile(dir / "manifest.json",
              R"({"manifestVersion": 1, "id": "qwen3.6-35b-a3b-int4", "task": "note",)"
              R"( "tier": "accuracy", "licence": "Apache-2.0",)"
              R"( "runtime": {"device": "GPU", "pipeline": "vlm",)"
              R"(  "properties": {"ACTIVATIONS_SCALE_FACTOR": 32, "KV_CACHE_PRECISION": "u8"}},)"
              R"( "files": {"weights.bin": ")" +
                  std::string(kHelloHash) + R"("}})");

    const ModelStore store(root.path);
    const auto& info = store.Resolve("note", "accuracy");
    EXPECT_EQ(info.pipeline, "vlm");
    EXPECT_EQ(info.properties["ACTIVATIONS_SCALE_FACTOR"], 32);
    EXPECT_EQ(info.properties["KV_CACHE_PRECISION"], "u8");
}

// A pipeline this build cannot construct, or a property it cannot pass, is
// a corrupt manifest for this build: refused at scan, never best-effort
TEST(ModelStore, AnUnknownPipelineOrANonScalarPropertyIsRefused) {
    for (const char* runtime :
         {R"({"device": "GPU", "pipeline": "diffusion"})",
          R"({"device": "GPU", "properties": {"NESTED": {"a": 1}}})",
          R"({"device": "GPU", "properties": [1, 2]})"}) {
        TempRoot root;
        const auto dir = root.path / "broken";
        std::filesystem::create_directories(dir);
        WriteFile(dir / "weights.bin", "hello");
        WriteFile(dir / "manifest.json",
                  std::string(R"({"manifestVersion": 1, "id": "broken", "task": "note",)") +
                      R"( "tier": "default", "licence": "MIT", "runtime": )" + runtime +
                      R"(, "files": {"weights.bin": ")" + kHelloHash + R"("}})");

        EXPECT_THROW(ModelStore{root.path}, std::runtime_error) << runtime;
    }
}

}  // namespace
}  // namespace ambient::models
