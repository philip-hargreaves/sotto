#include "adapters/diarisation/anchor_store.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
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

TEST(AnchorStore, AnEnrolmentSeedsThePrintAndConsultationsRefineIt) {
    TempDir dir;
    AnchorStore store(dir.path);
    store.Accrue(Unit(0, 1));  // an earlier accrual is discarded by the enrolment
    store.Replace(Unit(1, 0), 1'757'000'000);
    auto status = store.Status();
    EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled);
    EXPECT_EQ(status.sessions, 0u);
    EXPECT_EQ(status.enrolled_at, 1'757'000'000u);
    EXPECT_NEAR((*store.Anchor())[0], 1.0f, 1e-5);

    store.Accrue(Unit(0, 1));
    status = store.Status();
    EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled) << "refined, still enrolled";
    EXPECT_EQ(status.sessions, 1u);
    const auto anchor = *store.Anchor();
    EXPECT_GT(anchor[0], anchor[1]) << "the enrolment outweighs one consultation";
    EXPECT_GT(anchor[1], 0.0f) << "but the consultation moved it";
}

TEST(AnchorStore, StatusNamesTheOrigin) {
    TempDir dir;
    AnchorStore store(dir.path);
    EXPECT_EQ(store.Status().origin, AnchorOrigin::kNone);
    store.Accrue(Unit(1, 0));
    EXPECT_EQ(store.Status().origin, AnchorOrigin::kAccrued);
    EXPECT_EQ(store.Status().sessions, 1u);
    store.Clear();
    EXPECT_EQ(store.Status().origin, AnchorOrigin::kNone);
    EXPECT_EQ(store.Status().enrolled_at, 0u);
}

TEST(AnchorStore, EnrolmentSurvivesAReload) {
    TempDir dir;
    {
        AnchorStore store(dir.path);
        store.Replace(Unit(3, 4), 42);
        store.Accrue(Unit(3, 4));
    }
    AnchorStore reloaded(dir.path);
    const auto status = reloaded.Status();
    EXPECT_EQ(status.origin, AnchorOrigin::kEnrolled);
    EXPECT_EQ(status.sessions, 1u);
    EXPECT_EQ(status.enrolled_at, 42u);
}

TEST(AnchorRecord, AVersionOneRecordStillParses) {
    // version 1: version, dims, sessions, then the sum; no enrolment fields
    std::vector<std::uint8_t> v1(16 + 8);
    const std::uint32_t version = 1, dims = 2;
    const std::uint64_t sessions = 5;
    const float sum[2] = {0.6f, 0.8f};
    std::memcpy(v1.data(), &version, 4);
    std::memcpy(v1.data() + 4, &dims, 4);
    std::memcpy(v1.data() + 8, &sessions, 8);
    std::memcpy(v1.data() + 16, sum, 8);
    const auto record = detail::ParseAnchor(v1);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->sessions, 5u);
    EXPECT_EQ(record->enrolled_at, 0u);
    EXPECT_FLOAT_EQ(record->sum[1], 0.8f);

    const auto again = detail::ParseAnchor(detail::SerializeAnchor(*record));
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->sum, record->sum);
    EXPECT_FALSE(detail::ParseAnchor(std::vector<std::uint8_t>(10)).has_value()) << "too short";
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
