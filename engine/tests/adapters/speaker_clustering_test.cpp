#include "adapters/diarisation/speaker_clustering.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace ambient::diar {
namespace {

constexpr std::uint64_t kLong = kFitMinFrames;
constexpr std::uint64_t kShort = 8000;

// A unit vector near one of three well-separated directions, with a small
// deterministic offset so no two members are identical
std::vector<float> Voice(int direction, int member) {
    std::vector<float> v(8, 0.0f);
    v[static_cast<std::size_t>(direction)] = 1.0f;
    v[7] = 0.05f * static_cast<float>(member + 1);
    float norm = 0.0f;
    for (const float x : v) norm += x * x;
    norm = std::sqrt(norm);
    for (float& x : v) x /= norm;
    return v;
}

TEST(SpeakerClustering, TwoVoicesAreFoundAndEverySliceIsLabelled) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    for (int i = 0; i < 4; ++i) {
        embeddings.push_back(Voice(0, i));
        durations.push_back(kLong);
    }
    for (int i = 0; i < 4; ++i) {
        embeddings.push_back(Voice(1, i));
        durations.push_back(kLong);
    }
    embeddings.push_back(Voice(0, 9));  // short slice, same voice as the first pile
    durations.push_back(kShort);

    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 2);
    for (int i = 1; i < 4; ++i) EXPECT_EQ(result.labels[i], result.labels[0]);
    for (int i = 5; i < 8; ++i) EXPECT_EQ(result.labels[i], result.labels[4]);
    EXPECT_NE(result.labels[0], result.labels[4]);
    EXPECT_EQ(result.labels[8], result.labels[0]) << "a short slice joins its nearest centroid";
}

TEST(SpeakerClustering, AThirdLongVoiceRaisesTheCount) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    for (int voice = 0; voice < 3; ++voice) {
        for (int i = 0; i < 4; ++i) {
            embeddings.push_back(Voice(voice, i));
            durations.push_back(kLong);
        }
    }
    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 3);
}

TEST(SpeakerClustering, ShortSlicesCannotChangeTheCount) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    for (int voice = 0; voice < 2; ++voice) {
        for (int i = 0; i < 4; ++i) {
            embeddings.push_back(Voice(voice, i));
            durations.push_back(kLong);
        }
    }
    for (int i = 0; i < 5; ++i) {  // a chorus of short slices from a third voice
        embeddings.push_back(Voice(2, i));
        durations.push_back(kShort);
    }
    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 2) << "only long slices vote on the count";
}

TEST(SpeakerClustering, CentroidsAreUnitNorm) {
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    for (int voice = 0; voice < 2; ++voice) {
        for (int i = 0; i < 3; ++i) {
            embeddings.push_back(Voice(voice, i));
            durations.push_back(kLong);
        }
    }
    const auto result = ClusterSpeakers(embeddings, durations);
    ASSERT_EQ(result.centroids.size(), 2u);
    for (const auto& centroid : result.centroids) {
        double norm = 0.0;
        for (const float x : centroid) norm += static_cast<double>(x) * x;
        EXPECT_NEAR(norm, 1.0, 1e-5);
    }
}

TEST(SpeakerClustering, TooFewLongSlicesCollapsesToOneCluster) {
    std::vector<std::vector<float>> embeddings{Voice(0, 0), Voice(1, 0)};
    std::vector<std::uint64_t> durations{kLong, kLong};
    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 1);
    EXPECT_EQ(result.labels, (std::vector<int>{0, 0}));
    EXPECT_TRUE(result.centroids.empty());
}

TEST(SpeakerClustering, DegenerateInputsAreSafe) {
    EXPECT_TRUE(ClusterSpeakers({}, {}).labels.empty());
    const auto one = ClusterSpeakers({Voice(0, 0)}, {kLong});
    EXPECT_EQ(one.labels, (std::vector<int>{0}));
    EXPECT_EQ(one.count, 1);
}

TEST(SpeakerClustering, AllShortSlicesStillCluster) {
    // No slice reaches the fit threshold: the fit falls back to everything
    std::vector<std::vector<float>> embeddings;
    std::vector<std::uint64_t> durations;
    for (int voice = 0; voice < 2; ++voice) {
        for (int i = 0; i < 4; ++i) {
            embeddings.push_back(Voice(voice, i));
            durations.push_back(kShort);
        }
    }
    const auto result = ClusterSpeakers(embeddings, durations);
    EXPECT_EQ(result.count, 2);
}

}  // namespace
}  // namespace ambient::diar
