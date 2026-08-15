#include <gtest/gtest.h>

#include <string>

#include "openvino/genai/version.hpp"
#include "openvino/openvino.hpp"

namespace {

// Proves the pinned toolchain links and its DLLs load, on any machine
TEST(OvSmoke, RuntimeIsThePinnedRelease) {
    const std::string runtime = ov::get_openvino_version().buildNumber;
    EXPECT_NE(runtime.find("2026.3"), std::string::npos) << runtime;

    const std::string genai = ov::genai::get_version().buildNumber;
    EXPECT_NE(genai.find("2026.3"), std::string::npos) << genai;
}

}  // namespace
