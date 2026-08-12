#include "adapters/ipc/handlers.hpp"

#include <gtest/gtest.h>

#include "core/version.hpp"

namespace sotto::ipc {
namespace {

json HelloParams() {
    return json{
        {"name", "sotto-shell"}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion}};
}

const json& ResultOf(const std::variant<json, Error>& outcome) {
    return std::get<json>(outcome);
}

TEST(Handlers, HelloAnswersWithTheEngineIdentity) {
    const auto outcome = HandleHello(HelloParams());

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const auto& result = ResultOf(outcome);
    EXPECT_EQ(result["name"], sotto::kName);
    EXPECT_EQ(result["version"], sotto::kVersion);
    EXPECT_EQ(result["protocolVersion"], kProtocolVersion);
}

TEST(Handlers, HelloRejectsAPeerItCannotParse) {
    // Each of these fails PeerInfoFromJson for a different reason
    const json cases[] = {
        json::object(),
        json{{"name", "shell"}},
        json{{"name", 1}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion}},
        json{{"name", "shell"}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion + 1}},
        json{{"name", "shell"},
             {"version", "0.1.0"},
             {"protocolVersion", kProtocolVersion},
             {"extra", 1}},
    };

    for (const auto& params : cases) {
        const auto outcome = HandleHello(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }
}

TEST(Handlers, HelloIgnoresWhatThePeerClaimsAboutItself) {
    // The reply describes the engine, never the caller
    const auto outcome = HandleHello(
        json{{"name", "impostor"}, {"version", "9.9.9"}, {"protocolVersion", kProtocolVersion}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["name"], sotto::kName);
    EXPECT_EQ(ResultOf(outcome)["version"], sotto::kVersion);
}

TEST(Handlers, EchoReturnsThePayload) {
    const auto outcome = HandleEcho(json{{"payload", "alive"}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["payload"], "alive");
}

TEST(Handlers, EchoPreservesClinicalNonAscii) {
    const std::string clinical = "naïve café-au-lait 東京 µg °C";

    const auto outcome = HandleEcho(json{{"payload", clinical}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["payload"], clinical);
}

TEST(Handlers, EchoRejectsAMissingOrNonStringPayload) {
    const json cases[] = {
        json::object(),
        json{{"payload", 42}},
        json{{"payload", nullptr}},
        json{{"payload", json::array({"a"})}},
    };

    for (const auto& params : cases) {
        const auto outcome = HandleEcho(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }
}

}  // namespace
}  // namespace sotto::ipc
