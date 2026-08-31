#include "adapters/host/power_throttling.hpp"

#include <gtest/gtest.h>

using ambient::host::Describe;
using ambient::host::DisableThrottlingOnSelf;
using ambient::host::ReadThrottling;

TEST(PowerThrottling, OptOutReadsBackAsOff) {
    const auto state = DisableThrottlingOnSelf();

    ASSERT_TRUE(state.known) << "the read-back must work on this Windows";
    EXPECT_FALSE(state.defaulted) << "an explicit policy replaces Windows' own";
    EXPECT_FALSE(state.throttled);
    EXPECT_EQ(Describe(state), "off");
    EXPECT_EQ(Describe(ReadThrottling(GetCurrentProcess())), "off") << "the state sticks";
}

TEST(PowerThrottling, DescribeNamesEveryState) {
    EXPECT_EQ(Describe({}), "unknown");
    EXPECT_EQ(Describe({.known = true, .throttled = false, .defaulted = true}), "default");
    EXPECT_EQ(Describe({.known = true, .throttled = true, .defaulted = false}), "on");
    EXPECT_EQ(Describe({.known = true, .throttled = false, .defaulted = false}), "off");
}
