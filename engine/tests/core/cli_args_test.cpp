#include "core/cli_args.hpp"

#include <gtest/gtest.h>

namespace ambient {
namespace {

TEST(CliArgs, TakesTheFlagAndItsValue) {
    std::vector<std::string> args{"pipe", "--asr-device", "NPU", "store"};
    EXPECT_EQ(TakeFlag(args, "--asr-device"), "NPU");
    EXPECT_EQ(args, (std::vector<std::string>{"pipe", "store"}));
}

TEST(CliArgs, AbsentFlagLeavesArgsAlone) {
    std::vector<std::string> args{"pipe", "store"};
    EXPECT_EQ(TakeFlag(args, "--asr-device"), "");
    EXPECT_EQ(args.size(), 2u);
}

TEST(CliArgs, FlagWithoutAValueIsIgnored) {
    std::vector<std::string> args{"pipe", "--asr-device"};
    EXPECT_EQ(TakeFlag(args, "--asr-device"), "");
    EXPECT_EQ(args.size(), 2u);
}

}  // namespace
}  // namespace ambient
