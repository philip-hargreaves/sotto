#include "adapters/audio/capture_devices.hpp"

#include <gtest/gtest.h>

namespace {

// Real enumeration; a runner with no microphone legitimately lists nothing
TEST(CaptureDevices, EveryListedDeviceIsWellFormed) {
    const auto devices = ambient::audio::ListCaptureDevices();
    int defaults = 0;
    for (const auto& device : devices) {
        EXPECT_FALSE(device.id.empty());
        EXPECT_FALSE(device.name.empty());
        EXPECT_FALSE(device.short_name.empty());
        defaults += device.is_default ? 1 : 0;
        std::printf("  %s%s%s | %s\n", device.short_name.c_str(),
                    device.is_default ? " (default)" : "", device.bluetooth ? " [BT]" : "",
                    device.name.c_str());
    }
    EXPECT_LE(defaults, 1) << "at most one communications default";
    if (!devices.empty()) {
        EXPECT_EQ(defaults, 1) << "a non-empty list names its default";
    }
}

TEST(CaptureDevices, ResolveFindsTheRequestedDevice) {
    const std::vector<ambient::audio::CaptureDevice> devices{
        {"{aa}", "Array", "Array", true, false},
        {"{bb}", "Headset", "Headset", false, true},
    };
    EXPECT_EQ(ambient::audio::ResolveMicrophone(devices, "{bb}").id, "{bb}");
}

TEST(CaptureDevices, AGoneChoiceResolvesToTheDefault) {
    const std::vector<ambient::audio::CaptureDevice> devices{
        {"{aa}", "USB Mic", "USB Mic", false, false},
        {"{bb}", "Array", "Array", true, false},
    };
    const auto resolved = ambient::audio::ResolveMicrophone(devices, "{unplugged}");
    EXPECT_EQ(resolved.id, "{bb}");
    EXPECT_EQ(resolved.name, "Array");
}

TEST(CaptureDevices, NoDefaultFallsToTheFirstAndEmptyToNothing) {
    const std::vector<ambient::audio::CaptureDevice> devices{
        {"{aa}", "USB Mic", "USB Mic", false, false},
    };
    EXPECT_EQ(ambient::audio::ResolveMicrophone(devices, "").id, "{aa}");
    EXPECT_EQ(ambient::audio::ResolveMicrophone({}, "{any}").id, "");
}

TEST(CaptureDevices, WideIdRoundTripsAscii) {
    EXPECT_EQ(ambient::audio::WideId("{0.0.1.00000000}.{abc}"), L"{0.0.1.00000000}.{abc}");
    EXPECT_TRUE(ambient::audio::WideId("").empty());
}

}  // namespace
