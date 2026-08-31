#include "core/version.hpp"

#include <gtest/gtest.h>

TEST(Version, NameIsSet) {
    EXPECT_STREQ(ambient::kName, "ambient");
}

TEST(Version, MatchesCMakeProjectVersion) {
    EXPECT_STREQ(ambient::kVersion, AMBIENT_CMAKE_VERSION);
}
