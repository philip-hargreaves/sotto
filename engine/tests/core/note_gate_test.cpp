#include "core/note_gate.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace ambient::note {
namespace {

TEST(RefusalReason, ReadsTheSentinelLineAndNothingElse) {
    EXPECT_EQ(RefusalReason("NOT A CONSULTATION: a cooking video, one speaker\n"),
              "a cooking video, one speaker");
    EXPECT_EQ(RefusalReason("  \nNOT A CONSULTATION:   a lecture\nmore text"), "a lecture");
    EXPECT_FALSE(RefusalReason("The patient presented with a sore elbow.").has_value());
    EXPECT_FALSE(RefusalReason("Not a consultation of the usual kind, the patient...").has_value())
        << "case matters: the sentinel is exact";
}

TEST(RefusalFilter, HoldsTheOpeningThenStreamsARealNote) {
    std::vector<std::string> seen;
    RefusalFilter filter([&](const std::string& t) { seen.push_back(t); });
    filter("NOT");
    filter("NOT A CONS");
    EXPECT_TRUE(seen.empty()) << "could still be a refusal";
    filter("NOT A CONSIDERABLE delay, the patient presented");
    ASSERT_EQ(seen.size(), 1u) << "decided: not the sentinel";
    filter("NOT A CONSIDERABLE delay, the patient presented with");
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_FALSE(filter.Refused());
}

TEST(RefusalFilter, SwallowsARefusalEntirely) {
    std::vector<std::string> seen;
    RefusalFilter filter([&](const std::string& t) { seen.push_back(t); });
    filter("NOT A CONSULTATION:");
    filter("NOT A CONSULTATION: a ramen video");
    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(filter.Refused());
}

TEST(RefusalFilter, AnOrdinaryOpeningStreamsAtOnce) {
    std::vector<std::string> seen;
    RefusalFilter filter([&](const std::string& t) { seen.push_back(t); });
    filter("The");
    EXPECT_EQ(seen.size(), 1u);
}

}  // namespace
}  // namespace ambient::note
