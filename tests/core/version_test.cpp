#include "core/version.hpp"

#include <gtest/gtest.h>

TEST(Version, NameIsSet) {
    EXPECT_STREQ(sotto::kName, "sotto");
}

TEST(Version, MatchesCMakeProjectVersion) {
    EXPECT_STREQ(sotto::kVersion, SOTTO_CMAKE_VERSION);
}
