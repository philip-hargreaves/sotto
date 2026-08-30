#include "adapters/ipc/handlers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "adapters/storage/sqlite_session_store.hpp"
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

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(SOTTO_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

TEST(Handlers, ModelsListMatchesTheFixture) {
    const auto root = std::filesystem::temp_directory_path() / "sotto-handlers-models";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "whisper-turbo-int8");
    std::ofstream(root / "whisper-turbo-int8" / "manifest.json")
        << R"({"manifestVersion": 1, "id": "whisper-turbo-int8", "task": "asr",)"
        << R"( "tier": "default", "licence": "MIT", "runtime": {"device": "GPU"},)"
        << R"( "files": {"model.xml": "00"}})";

    const sotto::models::ModelStore store(root);
    const json built = MakeResult(std::int64_t{7}, HandleModels(store));
    EXPECT_EQ(built, LoadFixture("models-list.json"));
    std::filesystem::remove_all(root);
}

struct SessionStoreFixture {
    std::filesystem::path root;
    std::unique_ptr<sotto::store::SqliteSessionStore> store;

    SessionStoreFixture() {
        root = std::filesystem::temp_directory_path() /
               ("sotto-handlers-sessions-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        store = std::make_unique<sotto::store::SqliteSessionStore>(root, std::chrono::hours(1));
    }

    ~SessionStoreFixture() {
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST(Handlers, SessionListAndTranscriptRoundTrip) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->AppendTurn(id, {480000, 48000, "", "about three weeks now, mostly mornings"});
    fixture.store->Finalise(id);

    const json list = HandleSessionList(*fixture.store);
    ASSERT_EQ(list["sessions"].size(), 1u);
    EXPECT_EQ(list["sessions"][0]["id"], id);
    EXPECT_EQ(list["sessions"][0]["state"], "finalised");
    EXPECT_EQ(list["sessions"][0]["sampleRate"], 16000);
    EXPECT_FALSE(list["sessions"][0]["endedAt"].get<std::string>().empty());

    const auto outcome = HandleSessionTranscript(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    // The result payload matches the fixture's shape byte for byte
    const json built = MakeResult(std::int64_t{8}, std::get<json>(outcome));
    EXPECT_EQ(built, LoadFixture("session-transcript.json"));
}

TEST(Handlers, SessionNoteReturnsTheStoredText) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, sotto::store::DocumentKind::kNote,
                                {.text = "The patient presents with a swollen left elbow."});

    const auto outcome = HandleSessionNote(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(std::get<json>(outcome)["text"], "The patient presents with a swollen left elbow.");

    const auto missing = HandleSessionNote(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
}

TEST(Handlers, SessionDeleteRemovesAndUnknownIdsError) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);

    const auto deleted = HandleSessionDelete(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(deleted));
    EXPECT_TRUE(HandleSessionList(*fixture.store)["sessions"].empty());

    const auto missing = HandleSessionDelete(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
    const json built = MakeError(std::int64_t{9}, std::get<Error>(missing));
    EXPECT_EQ(built, LoadFixture("session-error.json"));
}

TEST(Handlers, SessionMethodsRejectAMissingId) {
    SessionStoreFixture fixture;
    for (const json params : {json::object(), json{{"id", 42}}}) {
        const auto transcript = HandleSessionTranscript(*fixture.store, params);
        ASSERT_TRUE(std::holds_alternative<Error>(transcript)) << params.dump();
        EXPECT_EQ(std::get<Error>(transcript).code, kInvalidParams);
        const auto deleted = HandleSessionDelete(*fixture.store, params);
        ASSERT_TRUE(std::holds_alternative<Error>(deleted)) << params.dump();
    }
}

TEST(Handlers, AnEmptyModelStoreListsNothing) {
    const sotto::models::ModelStore store(std::filesystem::temp_directory_path() /
                                          "sotto-no-models");
    const json result = HandleModels(store);
    EXPECT_TRUE(result["models"].is_array());
    EXPECT_TRUE(result["models"].empty());
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
