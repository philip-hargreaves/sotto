#include "adapters/ipc/messages.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

namespace sotto::ipc {
namespace {

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(SOTTO_FIXTURE_DIR) + "/" + name);
    EXPECT_TRUE(in.is_open()) << "missing fixture: " << name;
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

TEST(Messages, ErrorResponseMatchesFixture) {
    const json built = MakeError(
        std::int64_t{7}, Error{kMethodNotFound, "Method not found", json("engine/no-such-method")});
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

}  // namespace
}  // namespace sotto::ipc
