#include "adapters/ipc/framing.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sotto::ipc {
namespace {

TEST(Framing, RoundTrip) {
    FrameDecoder d;
    d.Push(EncodeFrame("hello"));
    EXPECT_EQ(d.Next(), "hello");
    EXPECT_EQ(d.Next(), std::nullopt);
}

TEST(Framing, EmptyPayload) {
    FrameDecoder d;
    d.Push(EncodeFrame(""));
    EXPECT_EQ(d.Next(), "");
}

TEST(Framing, ByteAtATimeDelivery) {
    FrameDecoder d;
    const std::string frame = EncodeFrame("split across pushes");
    for (char c : frame) {
        d.Push(std::string_view(&c, 1));
    }
    EXPECT_EQ(d.Next(), "split across pushes");
}

TEST(Framing, TwoFramesInOnePush) {
    FrameDecoder d;
    d.Push(EncodeFrame("first") + EncodeFrame("second"));
    EXPECT_EQ(d.Next(), "first");
    EXPECT_EQ(d.Next(), "second");
    EXPECT_EQ(d.Next(), std::nullopt);
}

TEST(Framing, IncompleteFrameYieldsNothing) {
    FrameDecoder d;
    const std::string frame = EncodeFrame("truncated");
    d.Push(std::string_view(frame).substr(0, frame.size() - 3));
    EXPECT_EQ(d.Next(), std::nullopt);
    EXPECT_FALSE(d.failed());
}

TEST(Framing, OversizeDeclaredLengthPoisonsDecoder) {
    FrameDecoder d;
    const std::uint32_t len = kMaxFrameBytes + 1;
    std::string header;
    for (int shift : {0, 8, 16, 24}) {
        header.push_back(static_cast<char>((len >> shift) & 0xFF));
    }
    d.Push(header);
    EXPECT_EQ(d.Next(), std::nullopt);
    EXPECT_TRUE(d.failed());
    d.Push(EncodeFrame("after failure"));
    EXPECT_EQ(d.Next(), std::nullopt);
}

TEST(Framing, EncodeRejectsOversizePayload) {
    EXPECT_THROW(EncodeFrame(std::string(kMaxFrameBytes + 1, 'x')), std::length_error);
}

TEST(Framing, MaxSizePayloadAccepted) {
    FrameDecoder d;
    d.Push(EncodeFrame(std::string(kMaxFrameBytes, 'x')));
    auto out = d.Next();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->size(), kMaxFrameBytes);
}

}  // namespace
}  // namespace sotto::ipc
