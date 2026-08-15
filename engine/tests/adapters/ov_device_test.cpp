#include <gtest/gtest.h>

#include <algorithm>

#include "openvino/openvino.hpp"

namespace {

TEST(OvDevice, AnIntelGpuIsPresent) {
    ov::Core core;
    std::string listing;
    bool found = false;
    for (const auto& device : core.get_available_devices()) {
        if (device.rfind("GPU", 0) != 0) continue;
        const auto name = core.get_property(device, ov::device::full_name);
        listing += device + "=" + name + " ";
        if (name.find("Intel") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "gpus: " << listing;
}

}  // namespace
