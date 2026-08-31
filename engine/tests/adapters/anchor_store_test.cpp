#include "adapters/diarisation/anchor_store.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include "adapters/diarisation/cluster_voiceprint.hpp"

namespace ambient::diar {
namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path =
            std::filesystem::temp_directory_path() /
            ("ambient-anchor-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::vector<float> Unit(float x, float y) {
    const float norm = std::sqrt(x * x + y * y);
    return {x / norm, y / norm};
}

TEST(AnchorStore, StartsEmptyAndAccruesARunningMean) {
    TempDir dir;
    AnchorStore store(dir.path);
    EXPECT_FALSE(store.Anchor().has_value());

    store.Accrue(Unit(1, 0));
    store.Accrue(Unit(0, 1));
    EXPECT_EQ(store.Sessions(), 2u);
    const auto anchor = store.Anchor();
    ASSERT_TRUE(anchor.has_value());
    EXPECT_NEAR((*anchor)[0], (*anchor)[1], 1e-6) << "the mean of two orthogonal units is diagonal";
    EXPECT_NEAR((*anchor)[0] * (*anchor)[0] + (*anchor)[1] * (*anchor)[1], 1.0, 1e-5)
        << "unit norm";
}

TEST(AnchorStore, SurvivesAReload) {
    TempDir dir;
    {
        AnchorStore store(dir.path);
        store.Accrue(Unit(3, 4));
    }
    AnchorStore reloaded(dir.path);
    EXPECT_EQ(reloaded.Sessions(), 1u);
    ASSERT_TRUE(reloaded.Anchor().has_value());
    EXPECT_NEAR((*reloaded.Anchor())[0], 0.6f, 1e-5);
}

TEST(AnchorStore, ClearErasesTheFile) {
    TempDir dir;
    {
        AnchorStore store(dir.path);
        store.Accrue(Unit(1, 0));
        store.Clear();
        EXPECT_FALSE(store.Anchor().has_value());
    }
    EXPECT_FALSE(std::filesystem::exists(dir.path / "anchor.bin"));
    AnchorStore reloaded(dir.path);
    EXPECT_FALSE(reloaded.Anchor().has_value());
}

TEST(AnchorStore, ACorruptFileResetsToEmpty) {
    TempDir dir;
    std::ofstream(dir.path / "anchor.bin", std::ios::binary) << "not a wrapped blob";
    AnchorStore store(dir.path);
    EXPECT_FALSE(store.Anchor().has_value());
    store.Accrue(Unit(1, 0));  // and it can accrue again afterwards
    EXPECT_EQ(store.Sessions(), 1u);
}

TEST(VoiceprintRanges, StopAtTheCapAndRefuseUnderASecond) {
    std::vector<LabelledSlice> slices;
    for (int i = 0; i < 5; ++i) {  // five 40 s slices of cluster 0
        const auto start = static_cast<std::uint64_t>(i) * 700000;
        slices.push_back({start, start + 640000, 0});
    }
    slices.push_back({4000000, 4008000, 1});  // cluster 1: half a second only

    const auto ranges = VoiceprintRanges(slices, 0);
    ASSERT_EQ(ranges.size(), 3u) << "the slice crossing 90 s is kept whole, then stop";
    EXPECT_TRUE(VoiceprintRanges(slices, 1).empty()) << "under a second carries no identity";
}

}  // namespace
}  // namespace ambient::diar
