#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

#include "adapters/models/model_store.hpp"
#include "adapters/models/ov_runtime.hpp"

namespace sotto::models {
namespace {

// Staged on dev machines by:
//   tools/stage-model.ps1 -Id whisper-turbo-int8 -Task asr -Tier default -Licence MIT
//       -Source C:\dev\intelliscribe\ml-models\asr\whisper-large-v3-turbo-int8
TEST(AsrModel, StagedVerifiedAndCompilesOnTheGpu) {
    const ModelStore store(std::filesystem::path(SOTTO_MODELS_DIR));

    const ModelInfo& info = store.Resolve("asr", "default");
    EXPECT_EQ(info.id, "whisper-turbo-int8");
    EXPECT_EQ(info.device, "GPU");

    OvRuntime runtime;
    const LoadedModel encoder = runtime.Load(store, "asr", "default", "openvino_encoder_model.xml");
    EXPECT_EQ(encoder.device.rfind("GPU", 0), 0u) << encoder.device;
    std::printf("encoder: verified and compiled on %s in %lld ms\n", encoder.device.c_str(),
                static_cast<long long>(encoder.load_time.count()));
}

}  // namespace
}  // namespace sotto::models
