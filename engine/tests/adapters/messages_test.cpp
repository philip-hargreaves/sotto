#include "adapters/ipc/messages.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace sotto::ipc {
namespace {

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(SOTTO_FIXTURE_DIR) + "/" + name);
    // Throwing here names the missing file; parsing a closed stream would not
    if (!in.is_open()) {
        throw std::runtime_error("missing fixture: " + name);
    }
    return json::parse(in);
}

TEST(Messages, ParsesHelloRequestFixture) {
    auto parsed = ParseRequest(LoadFixture("hello-request.json"));
    ASSERT_TRUE(std::holds_alternative<Request>(parsed));
    const auto& req = std::get<Request>(parsed);
    EXPECT_EQ(req.method, "engine/hello");
    EXPECT_EQ(std::get<std::int64_t>(req.id), 1);

    auto peer = PeerInfoFromJson(req.params);
    ASSERT_TRUE(peer.has_value());
    EXPECT_EQ(peer->name, "sotto-shell");
    EXPECT_EQ(peer->version, "0.1.0");
    EXPECT_EQ(peer->protocol_version, 1);
}

TEST(Messages, HelloResponseMatchesFixture) {
    const json built =
        MakeResult(std::int64_t{1}, ToJson(PeerInfo{"sotto", "0.1.0", kProtocolVersion}));
    EXPECT_EQ(built, LoadFixture("hello-response.json"));
}

TEST(Messages, EchoRoundTripsNonAsciiFixture) {
    auto parsed = ParseRequest(LoadFixture("echo-request-nonascii.json"));
    ASSERT_TRUE(std::holds_alternative<Request>(parsed));
    const auto& req = std::get<Request>(parsed);
    EXPECT_EQ(std::get<std::string>(req.id), "e-2");

    const json built = MakeResult(req.id, json{{"payload", req.params["payload"]}});
    EXPECT_EQ(built, LoadFixture("echo-response-nonascii.json"));
}

TEST(Messages, AudioLevelNotificationMatchesFixture) {
    const auto built = MakeNotification("audio.level", {{"level", 0.5}, {"clipped", false}});

    EXPECT_EQ(built, LoadFixture("audio-level.json"));
}

TEST(Messages, TranscriptTurnNotificationMatchesFixture) {
    const auto built =
        MakeNotification("transcript.turn", {{"firstFrame", std::uint64_t{480000}},
                                             {"frameCount", std::uint64_t{48000}},
                                             {"speaker", ""},
                                             {"text", "about three weeks now, mostly mornings"}});

    EXPECT_EQ(built, LoadFixture("transcript-turn.json"));
}

TEST(Messages, SessionInterruptedNotificationMatchesFixture) {
    const auto built =
        MakeNotification("session/interrupted",
                         {{"reason", "deviceLost"}, {"detail", "GetBuffer reported 0x88890004"}});

    EXPECT_EQ(built, LoadFixture("session-interrupted.json"));
}

TEST(Messages, SessionProgressNotificationMatchesFixture) {
    const auto built = MakeNotification("session/progress", {{"stage", "speakers"}});

    EXPECT_EQ(built, LoadFixture("session-progress.json"));
}

TEST(Messages, ErrorResponseMatchesFixture) {
    const json built = MakeError(std::int64_t{7}, Error{kMethodNotFound, "Method not found"});
    EXPECT_EQ(built, LoadFixture("error-method-not-found.json"));
}

TEST(Messages, SerializeReplacesInvalidUtf8) {
    const json j{{"payload", std::string("bad \xFF\xFE bytes")}};
    std::string out;
    EXPECT_NO_THROW(out = Serialize(j));
    EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);  // U+FFFD
}

TEST(Messages, SerializeKeepsValidUtf8Verbatim) {
    const std::string clinical = "naïve café 東京 µg °C";
    const json j{{"payload", clinical}};
    EXPECT_NE(Serialize(j).find("naïve"), std::string::npos);
}

TEST(Messages, RejectsUnknownMember) {
    auto parsed =
        ParseRequest(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}, {"extra", true}});
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
    EXPECT_EQ(std::get<Error>(parsed).code, kInvalidRequest);
}

TEST(Messages, RejectsNullId) {
    auto parsed = ParseRequest(json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
}

TEST(Messages, RejectsMissingJsonrpc) {
    auto parsed = ParseRequest(json{{"id", 1}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
}

TEST(Messages, RejectsBatchArray) {
    auto parsed = ParseRequest(json::array({json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}}}));
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
}

TEST(Messages, RejectsWrongProtocolVersion) {
    EXPECT_EQ(PeerInfoFromJson(json{{"name", "x"}, {"version", "1"}, {"protocolVersion", 2}}),
              std::nullopt);
}

TEST(Messages, StringAndIntegerIdsBothParse) {
    auto with_int = ParseRequest(json{{"jsonrpc", "2.0"}, {"id", 42}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Request>(with_int));
    auto with_str = ParseRequest(json{{"jsonrpc", "2.0"}, {"id", "abc"}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Request>(with_str));
}

TEST(Messages, RejectsOverlongStringId) {
    const std::string long_id(kMaxIdBytes + 1, 'x');
    auto parsed = ParseRequest(json{{"jsonrpc", "2.0"}, {"id", long_id}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
}

TEST(Messages, RejectsIdBeyondInt64) {
    const std::uint64_t too_big =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    auto parsed = ParseRequest(json{{"jsonrpc", "2.0"}, {"id", too_big}, {"method", "m"}});
    ASSERT_TRUE(std::holds_alternative<Error>(parsed));
}

TEST(Messages, RejectsOutOfRangeProtocolVersion) {
    const std::uint64_t huge = static_cast<std::uint64_t>(1) << 33;  // truncates to 0, not 1
    EXPECT_EQ(PeerInfoFromJson(json{{"name", "x"}, {"version", "1"}, {"protocolVersion", huge}}),
              std::nullopt);
}

TEST(Messages, DeeplyNestedParamsDoesNotOverflow) {
    // nlohmann parse and destruction are iterative; only a copy recurses, so
    // ParseRequest must move params out. Depth far past a 1 MB stack's frames.
    constexpr int kDepth = 200000;
    std::string nested;
    nested.reserve(kDepth * 6 + 32);
    for (int i = 0; i < kDepth; ++i) nested += "{\"a\":";
    nested += "1";
    for (int i = 0; i < kDepth; ++i) nested += "}";
    json message{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m"}, {"params", json::object()}};
    message["params"]["deep"] = json::parse(nested);

    auto parsed = ParseRequest(std::move(message));
    EXPECT_TRUE(std::holds_alternative<Request>(parsed));
}

}  // namespace
}  // namespace sotto::ipc
