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
    EXPECT_EQ(list["sessions"][0]["label"], "") << "no note yet";
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null());
    EXPECT_EQ(list["sessions"][0]["audioSeconds"], 33.0)
        << "the audio's length from the turns, which outlive the audio";
    // The fixture is the shape both languages agree on. Named first: iterating
    // a temporary's sub-object dangles (range-for extends only the top level)
    const json list_fixture = LoadFixture("session-list.json");
    for (const auto& [key, value] : list_fixture["result"]["sessions"][0].items()) {
        EXPECT_TRUE(list["sessions"][0].contains(key)) << key;
    }

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
                                {.text = "The patient presents with a swollen left elbow.",
                                 .style = "soap",
                                 .detail = "concise"});

    const auto outcome = HandleSessionNote(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const json note = std::get<json>(outcome);
    EXPECT_EQ(note["text"], "The patient presents with a swollen left elbow.");
    EXPECT_EQ(note["style"], "soap");
    EXPECT_EQ(note["detail"], "concise");
    EXPECT_TRUE(note["generatedAt"].is_string());
    EXPECT_TRUE(note["editedAt"].is_null());
    const json note_fixture = LoadFixture("session-note.json");
    for (const auto& [key, value] : note_fixture["result"].items()) {
        EXPECT_TRUE(note.contains(key)) << key;
    }

    fixture.store->EditDocument(id, sotto::store::DocumentKind::kNote, "edited");
    const json edited = std::get<json>(HandleSessionNote(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(edited["text"], "edited");
    EXPECT_TRUE(edited["editedAt"].is_string());

    const auto missing = HandleSessionNote(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
}

TEST(Handlers, SessionPatientCarriesTheTranslationWhenStored) {
    using sotto::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "You have bursitis."});

    json patient = std::get<json>(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["text"], "You have bursitis.");
    EXPECT_EQ(patient["language"], "en");
    EXPECT_TRUE(patient["generatedAt"].is_string());
    EXPECT_TRUE(patient["translation"].is_null());

    fixture.store->SaveDocument(id, DocumentKind::kTranslation,
                                {.text = "Masz zapalenie kaletki.", .language = "pl"});
    patient = std::get<json>(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["translation"]["language"], "pl");
    EXPECT_EQ(patient["translation"]["text"], "Masz zapalenie kaletki.");
    const json patient_fixture = LoadFixture("session-patient.json");
    for (const auto& [key, value] : patient_fixture["result"].items()) {
        EXPECT_TRUE(patient.contains(key)) << key;
    }
}

TEST(Handlers, SessionListCarriesTheLabelAndTheLatestEdit) {
    using sotto::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    json list = HandleSessionList(*fixture.store);
    EXPECT_EQ(list["sessions"][0]["label"], "Elbow swelling");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null());

    fixture.store->EditDocument(id, DocumentKind::kLabel, "Left elbow bursitis");
    list = HandleSessionList(*fixture.store);
    EXPECT_EQ(list["sessions"][0]["label"], "Left elbow bursitis");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null())
        << "a retitle is housekeeping, not an edit to the record";

    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "sheet"});
    fixture.store->EditDocument(id, DocumentKind::kPatient, "sheet, edited");
    list = HandleSessionList(*fixture.store);
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_string()) << "any document's edit counts";
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

TEST(Handlers, AudioInputsCarryThePickerFields) {
    const std::vector<sotto::audio::CaptureDevice> devices{
        {"{0.0.1}.{aa}", "Microphone Array (Realtek(R) Audio)", "Microphone Array", true, false},
        {"{0.0.1}.{bb}", "Headset (H800 Hands-Free)", "Headset", false, true},
    };

    const json result = HandleAudioInputs(devices);

    ASSERT_EQ(result["devices"].size(), 2u);
    EXPECT_EQ(result["devices"][0]["id"], "{0.0.1}.{aa}");
    EXPECT_EQ(result["devices"][0]["name"], "Microphone Array (Realtek(R) Audio)");
    EXPECT_EQ(result["devices"][0]["shortName"], "Microphone Array");
    EXPECT_EQ(result["devices"][0]["isDefault"], true);
    EXPECT_EQ(result["devices"][0]["bluetooth"], false);
    EXPECT_EQ(result["devices"][1]["bluetooth"], true);
}

TEST(Handlers, NoMicrophonesIsAnEmptyListNotAnError) {
    const json result = HandleAudioInputs({});
    EXPECT_TRUE(result["devices"].is_array());
    EXPECT_TRUE(result["devices"].empty());
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
