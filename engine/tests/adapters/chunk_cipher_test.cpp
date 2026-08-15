#include "adapters/storage/chunk_cipher.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace sotto::store {
namespace {

std::vector<std::uint8_t> SampleChunk() {
    std::vector<std::uint8_t> plain(64 * 1024);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<std::uint8_t>(i * 31);
    }
    return plain;
}

TEST(ChunkCipher, RoundTrips) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto plain = SampleChunk();
    const auto sealed = cipher.Seal("session-a", 7, plain);
    EXPECT_EQ(sealed.size(), plain.size() + 16);
    EXPECT_EQ(cipher.Open("session-a", 7, sealed), plain);
}

TEST(ChunkCipher, CiphertextIsNotThePlaintext) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto plain = SampleChunk();
    const auto sealed = cipher.Seal("session-a", 0, plain);
    EXPECT_FALSE(std::equal(plain.begin(), plain.end(), sealed.begin()));
}

TEST(ChunkCipher, RoundTripsAnEmptyChunk) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto sealed = cipher.Seal("session-a", 0, {});
    EXPECT_EQ(sealed.size(), 16u);
    EXPECT_TRUE(cipher.Open("session-a", 0, sealed).empty());
}

TEST(ChunkCipher, TamperedCiphertextFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    auto sealed = cipher.Seal("session-a", 7, SampleChunk());
    sealed[100] ^= 0x01;
    EXPECT_THROW(cipher.Open("session-a", 7, sealed), std::runtime_error);
}

TEST(ChunkCipher, TamperedTagFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    auto sealed = cipher.Seal("session-a", 7, SampleChunk());
    sealed.back() ^= 0x01;
    EXPECT_THROW(cipher.Open("session-a", 7, sealed), std::runtime_error);
}

TEST(ChunkCipher, RenumberedChunkFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto sealed = cipher.Seal("session-a", 7, SampleChunk());
    EXPECT_THROW(cipher.Open("session-a", 8, sealed), std::runtime_error);
}

TEST(ChunkCipher, ChunkMovedBetweenSessionsFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto sealed = cipher.Seal("session-a", 7, SampleChunk());
    EXPECT_THROW(cipher.Open("session-b", 7, sealed), std::runtime_error);
}

TEST(ChunkCipher, WrongKeyFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const ChunkCipher other = ChunkCipher::Generate();
    const auto sealed = cipher.Seal("session-a", 7, SampleChunk());
    EXPECT_THROW(other.Open("session-a", 7, sealed), std::runtime_error);
}

TEST(ChunkCipher, TruncatedPayloadFailsAuthentication) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    EXPECT_THROW(cipher.Open("session-a", 0, std::vector<std::uint8_t>(15)), std::runtime_error);
}

TEST(ChunkCipher, WrappedKeySurvivesTheRoundTrip) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const auto plain = SampleChunk();
    const auto sealed = cipher.Seal("session-a", 7, plain);

    const ChunkCipher recovered = ChunkCipher::FromWrapped(cipher.Wrapped());
    EXPECT_EQ(recovered.Open("session-a", 7, sealed), plain);
}

TEST(ChunkCipher, GarbageWrappedKeyThrows) {
    const std::vector<std::uint8_t> garbage(64, 0xAB);
    EXPECT_THROW(ChunkCipher::FromWrapped(garbage), std::runtime_error);
}

}  // namespace
}  // namespace sotto::store
