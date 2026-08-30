#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/storage/sqlite_session_store.hpp"

namespace sotto::store {
namespace {

using namespace std::chrono_literals;

// Long enough that the writer thread never fires by itself, so a test that
// wants deterministic chunking gets exactly one chunk from Finalise
constexpr auto kNever = std::chrono::hours(1);

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path = std::filesystem::temp_directory_path() /
               ("sotto-store-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path DbPath() const {
        return path / "sotto.db";
    }

    // The first layout's per-session files, for the import test
    std::filesystem::path SessionFile(const SessionId& id, const char* suffix) const {
        return path / "sessions" / (id + suffix);
    }
};

ChunkCipher CipherOf(const TempRoot& root, const SessionId& id) {
    Db db(root.DbPath());
    Db::Stmt key = db.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
    key.BindText(1, id);
    if (!key.Step()) throw std::runtime_error("no key row for " + id);
    return ChunkCipher::FromWrapped(key.ColumnBlob(0));
}

std::vector<float> Ramp(std::size_t frames) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        audio[i] = static_cast<float>(i % 1000) / 1000.0f - 0.5f;
    }
    return audio;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct StoredChunk {
    std::int64_t first_frame;
    std::int64_t lost_before;
    std::vector<float> frames;
};

// Reads a finalised session the way recovery will: key row, then chunks
std::vector<StoredChunk> DecryptSession(const TempRoot& root, const SessionId& id) {
    const ChunkCipher cipher = CipherOf(root, id);
    Db db(root.DbPath());
    Db::Stmt select = db.Prepare(
        "SELECT seq, first_frame, lost_before, payload FROM chunks WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, id);
    std::vector<StoredChunk> chunks;
    std::int64_t expected_seq = 0;
    while (select.Step()) {
        EXPECT_EQ(select.ColumnInt64(0), expected_seq);
        const std::vector<std::uint8_t> plain = cipher.Open(
            Domain::kAudio, id, static_cast<std::uint64_t>(expected_seq), select.ColumnBlob(3));
        StoredChunk chunk{select.ColumnInt64(1), select.ColumnInt64(2),
                          std::vector<float>(plain.size() / sizeof(float))};
        std::memcpy(chunk.frames.data(), plain.data(), plain.size());
        chunks.push_back(std::move(chunk));
        ++expected_seq;
    }
    return chunks;
}

struct StoredTurn {
    std::int64_t first_frame;
    std::int64_t frame_count;
    std::string speaker;
    std::string text;
};

std::vector<StoredTurn> DecryptTurns(const TempRoot& root, const SessionId& id) {
    const ChunkCipher cipher = CipherOf(root, id);
    Db db(root.DbPath());
    Db::Stmt select = db.Prepare(
        "SELECT seq, first_frame, frame_count, payload FROM turns WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, id);
    std::vector<StoredTurn> turns;
    std::int64_t expected_seq = 0;
    while (select.Step()) {
        EXPECT_EQ(select.ColumnInt64(0), expected_seq);
        const auto plain = cipher.Open(Domain::kTurns, id, static_cast<std::uint64_t>(expected_seq),
                                       select.ColumnBlob(3));
        const auto content = nlohmann::json::parse(plain.begin(), plain.end());
        turns.push_back({select.ColumnInt64(1), select.ColumnInt64(2),
                         content.at("speaker").get<std::string>(),
                         content.at("text").get<std::string>()});
        ++expected_seq;
    }
    return turns;
}

TEST(SessionStore, TurnsRoundTripEncrypted) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.AppendTurn(id, {0, 16000, "", "how long have you had the pain"});
        store.AppendTurn(id, {16000, 8000, "", "about three weeks"});
        store.Finalise(id);
    }

    const auto turns = DecryptTurns(root, id);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].first_frame, 0);
    EXPECT_EQ(turns[0].frame_count, 16000);
    EXPECT_EQ(turns[0].text, "how long have you had the pain");
    EXPECT_EQ(turns[1].first_frame, 16000);
    EXPECT_EQ(turns[1].text, "about three weeks");
}

TEST(SessionStore, StoredAudioReadsBackForResume) {
    TempRoot root;
    SessionId id;
    std::vector<float> audio(16000);
    for (std::size_t i = 0; i < audio.size(); ++i) {
        audio[i] = static_cast<float>(i % 100) / 100.0F;
    }
    {
        SqliteSessionStore store(root.path, std::chrono::milliseconds(10));
        id = store.Begin({16000, "", ""});
        store.Append(id, audio, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }  // destroyed mid-recording, as a crash leaves it

    SqliteSessionStore store(root.path, std::chrono::milliseconds(10));
    EXPECT_EQ(store.ReadAudio(id), audio);
}

TEST(SessionStore, TurnTextIsNotPlaintextAtRest) {
    TempRoot root;
    const std::string sentinel = "SENTINEL-HYPERTENSION-PHRASE";
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.AppendTurn(id, {0, 16000, "doctor", sentinel});
        store.Finalise(id);
    }

    const auto file = ReadFileBytes(root.DbPath());
    const std::vector<std::uint8_t> needle(sentinel.begin(), sentinel.end());
    EXPECT_EQ(std::search(file.begin(), file.end(), needle.begin(), needle.end()), file.end());
}

TEST(SessionStore, ReadTurnsReturnsWhatWasAppended) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.AppendTurn(id, {0, 16000, "doctor", "how long have you had the pain"});
    store.AppendTurn(id, {16000, 8000, "patient", "about three weeks"});
    store.Finalise(id);

    const auto turns = store.ReadTurns(id);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].first_frame, 0u);
    EXPECT_EQ(turns[0].speaker, "doctor");
    EXPECT_EQ(turns[0].text, "how long have you had the pain");
    EXPECT_EQ(turns[1].first_frame, 16000u);
    EXPECT_EQ(turns[1].text, "about three weeks");
}

TEST(SessionStore, TheNoteRoundTripsWithItsOptionsAfterFinalise) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);

    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, "");
    store.SaveDocument(id, DocumentKind::kNote,
                       {.text = "The patient presents with a swollen left elbow.",
                        .style = "soap",
                        .detail = "detailed"});
    Document note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "The patient presents with a swollen left elbow.");
    EXPECT_EQ(note.language, "en");
    EXPECT_EQ(note.style, "soap");
    EXPECT_EQ(note.detail, "detailed");
    EXPECT_FALSE(note.generated_at.empty());
    EXPECT_TRUE(note.edited_at.empty());

    store.SaveDocument(id, DocumentKind::kNote, {.text = "revised", .style = "prose"});
    note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "revised") << "a rewrite replaces the note";
    EXPECT_EQ(note.style, "prose") << "and its options";
}

TEST(SessionStore, AnEditKeepsTheOptionsAndStampsEditedAt) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);
    store.SaveDocument(id, DocumentKind::kNote,
                       {.text = "generated", .style = "soap", .detail = "concise"});
    const std::string generated_at = store.ReadDocument(id, DocumentKind::kNote).generated_at;

    store.EditDocument(id, DocumentKind::kNote, "the clinician's wording");

    const Document note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "the clinician's wording");
    EXPECT_EQ(note.style, "soap");
    EXPECT_EQ(note.detail, "concise");
    EXPECT_EQ(note.generated_at, generated_at);
    EXPECT_FALSE(note.edited_at.empty());

    store.SaveDocument(id, DocumentKind::kNote, {.text = "regenerated", .style = "prose"});
    EXPECT_TRUE(store.ReadDocument(id, DocumentKind::kNote).edited_at.empty())
        << "a generation replaces the edit";
}

TEST(SessionStore, DocumentsAreNotPlaintextAtRest) {
    TempRoot root;
    const std::string note = "SENTINEL-BURSITIS-PHRASE";
    const std::string patient = "SENTINEL-PATIENT-PHRASE";
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.Finalise(id);
        store.SaveDocument(id, DocumentKind::kNote, {.text = note});
        store.SaveDocument(id, DocumentKind::kPatient, {.text = patient});
        EXPECT_EQ(store.ReadDocument(id, DocumentKind::kPatient).text, patient);
        EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, note)
            << "patient and note are separate";
        EXPECT_EQ(store.ReadDocument(id, DocumentKind::kTranslation).text, "");
    }

    const auto file = ReadFileBytes(root.DbPath());
    for (const std::string& sentinel : {note, patient}) {
        const std::vector<std::uint8_t> needle(sentinel.begin(), sentinel.end());
        EXPECT_EQ(std::search(file.begin(), file.end(), needle.begin(), needle.end()), file.end());
    }
}

TEST(SessionStore, EveryDocumentKindRoundTrips) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);
    store.SaveDocument(id, DocumentKind::kTranslation, {.text = "Twój łokieć", .language = "pl"});
    store.SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    const Document translation = store.ReadDocument(id, DocumentKind::kTranslation);
    EXPECT_EQ(translation.text, "Twój łokieć");
    EXPECT_EQ(translation.language, "pl");
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kLabel).text, "Elbow swelling");
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, "");
}

TEST(SessionStore, DocumentsRefuseTheRecordingSessionAndUnknownIds) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    EXPECT_THROW(store.SaveDocument(id, DocumentKind::kNote, {.text = "early"}),
                 std::runtime_error);
    EXPECT_THROW(store.EditDocument(id, DocumentKind::kNote, "early"), std::runtime_error);
    EXPECT_THROW(store.ReadDocument(id, DocumentKind::kNote), std::runtime_error);
    EXPECT_THROW(store.ReadDocument("nope", DocumentKind::kNote), std::runtime_error);
}

// Writes the first layout by hand: main.db catalog, sessions/<id>.db with
// chunks, turns, a sealed note in its own table and the meta bag, and the
// wrapped key beside it
SessionId WritePerSessionFileLayout(const TempRoot& root, const std::vector<float>& audio,
                                    const std::string& turn_text, const std::string& note,
                                    const char* state = "finalised") {
    const SessionId id = "0123456789abcdef0123456789abcdef";
    std::filesystem::create_directories(root.path / "sessions");
    {
        Db catalog(root.path / "main.db");
        catalog.Exec(
            "CREATE TABLE sessions(id TEXT PRIMARY KEY, started_at TEXT NOT NULL, ended_at TEXT,"
            " state TEXT NOT NULL, sample_rate INTEGER NOT NULL, device_id TEXT,"
            " device_name TEXT, lost_frames INTEGER NOT NULL DEFAULT 0)");
        Db::Stmt insert = catalog.Prepare(
            "INSERT INTO sessions VALUES('0123456789abcdef0123456789abcdef',"
            " '2026-08-01T09:00:00Z', '2026-08-01T09:10:00Z', ?, 16000, 'mic-1',"
            " 'Old microphone', 7)");
        insert.BindText(1, state);
        insert.Step();
        catalog.SetUserVersion(1);
    }
    const ChunkCipher cipher = ChunkCipher::Generate();
    {
        const std::vector<std::uint8_t> wrapped = cipher.Wrapped();
        std::ofstream key(root.SessionFile(id, ".key"), std::ios::binary);
        key.write(reinterpret_cast<const char*>(wrapped.data()),
                  static_cast<std::streamsize>(wrapped.size()));
    }
    Db db(root.SessionFile(id, ".db"));
    db.Exec(
        "CREATE TABLE chunks(seq INTEGER PRIMARY KEY, first_frame INTEGER NOT NULL,"
        " frame_count INTEGER NOT NULL, lost_before INTEGER NOT NULL, payload BLOB NOT NULL);"
        "CREATE TABLE turns(seq INTEGER PRIMARY KEY, first_frame INTEGER NOT NULL,"
        " frame_count INTEGER NOT NULL, payload BLOB NOT NULL);"
        "CREATE TABLE note(seq INTEGER PRIMARY KEY, payload BLOB NOT NULL);"
        "CREATE TABLE patient(seq INTEGER PRIMARY KEY, payload BLOB NOT NULL);"
        "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "INSERT INTO meta VALUES('id', '0123456789abcdef0123456789abcdef')");
    {
        const auto sealed = cipher.Seal(
            Domain::kAudio, id, 0,
            {reinterpret_cast<const std::uint8_t*>(audio.data()), audio.size() * sizeof(float)});
        Db::Stmt insert = db.Prepare("INSERT INTO chunks VALUES(0, 0, ?, 7, ?)");
        insert.BindInt64(1, static_cast<std::int64_t>(audio.size()));
        insert.BindBlob(2, sealed);
        insert.Step();
    }
    {
        const std::string content =
            nlohmann::json{{"speaker", "doctor"}, {"text", turn_text}}.dump();
        const auto sealed =
            cipher.Seal(Domain::kTurns, id, 0,
                        {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
        Db::Stmt insert = db.Prepare("INSERT INTO turns VALUES(0, 0, 16000, ?)");
        insert.BindBlob(1, sealed);
        insert.Step();
    }
    {
        const auto sealed =
            cipher.Seal(Domain::kNote, id, 0,
                        {reinterpret_cast<const std::uint8_t*>(note.data()), note.size()});
        Db::Stmt insert = db.Prepare("INSERT INTO note VALUES(0, ?)");
        insert.BindBlob(1, sealed);
        insert.Step();
    }
    db.SetUserVersion(1);
    return id;
}

TEST(SessionStore, ImportsThePerSessionFileLayoutOnFirstOpen) {
    TempRoot root;
    const auto audio = Ramp(16000);
    const SessionId id = WritePerSessionFileLayout(root, audio, "how long have you had the pain",
                                                   "written by the first release");

    SqliteSessionStore store(root.path, kNever);

    const auto sessions = store.ListSessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, id);
    EXPECT_EQ(sessions[0].state, "finalised");
    EXPECT_EQ(sessions[0].ended_at, "2026-08-01T09:10:00Z");
    EXPECT_TRUE(store.ReadAudio(id).empty()) << "finalised: the audio is held to the seal rule";
    const auto turns = store.ReadTurns(id);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "doctor");
    EXPECT_EQ(turns[0].text, "how long have you had the pain");
    const Document note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "written by the first release");
    EXPECT_TRUE(note.style.empty()) << "the first layout stored no options";
    EXPECT_TRUE(note.generated_at.empty());
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kPatient).text, "");

    Db db(root.DbPath());
    Db::Stmt row = db.Prepare("SELECT device_name, lost_frames FROM sessions WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnText(0), "Old microphone");
    EXPECT_EQ(row.ColumnInt64(1), 7);

    EXPECT_FALSE(std::filesystem::exists(root.path / "main.db")) << "the old catalog is gone";
    EXPECT_FALSE(std::filesystem::exists(root.SessionFile(id, ".db")));
    EXPECT_FALSE(std::filesystem::exists(root.SessionFile(id, ".key")));
    EXPECT_FALSE(std::filesystem::exists(root.path / "sessions"));

    store.EditDocument(id, DocumentKind::kNote, "edited on the new build");
    EXPECT_FALSE(store.ReadDocument(id, DocumentKind::kNote).edited_at.empty());
}

TEST(SessionStore, AnOldCrashedSessionImportsWithItsAudioForRecovery) {
    TempRoot root;
    const auto audio = Ramp(16000);
    const SessionId id = WritePerSessionFileLayout(root, audio, "turn", "", "recording");

    SqliteSessionStore store(root.path, kNever);
    const auto recoverable = store.ScanRecoverable();
    ASSERT_EQ(recoverable.size(), 1u);
    EXPECT_EQ(recoverable[0].id, id);
    EXPECT_EQ(store.ReadAudio(id), audio) << "the ciphertext moved unchanged";
}

TEST(SessionStore, TheSweepErasesFinalisedSessionsRecordedWithRetainOff) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    SessionMeta keep{16000, "", ""};
    SessionMeta drop{16000, "", ""};
    drop.retain = false;

    const SessionId kept = store.Begin(keep);
    store.AppendTurn(kept, {0, 16000, "doctor", "kept"});
    store.Finalise(kept);
    const SessionId dropped = store.Begin(drop);
    store.AppendTurn(dropped, {0, 16000, "doctor", "dropped"});
    store.Finalise(dropped);
    ASSERT_EQ(store.ListSessions().size(), 2u) << "readable until the consultation is left";

    store.EraseUnretained();

    const auto sessions = store.ListSessions();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, kept);
    EXPECT_THROW(store.ReadTurns(dropped), std::runtime_error);
    Db db(root.DbPath());
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM session_keys"), 1);
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM turns"), 1);
}

TEST(SessionStore, ACrashedRetainOffSessionWaitsForRecovery) {
    TempRoot root;
    const auto audio = Ramp(16000);
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        SessionMeta drop{16000, "", ""};
        drop.retain = false;
        id = store.Begin(drop);
        store.Append(id, audio, 0);
        store.Abandon(id);
    }
    SqliteSessionStore reopened(root.path, kNever);
    reopened.EraseUnretained();
    ASSERT_EQ(reopened.ScanRecoverable().size(), 1u) << "its audio is still needed";
    EXPECT_EQ(reopened.ReadAudio(id), audio);
}

TEST(SessionStore, AVersionTwoDatabaseGainsTheRetentionFlag) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.Finalise(id);
    }
    {
        // Back to the version-2 shape: the same table without retain
        Db db(root.DbPath());
        db.Exec("PRAGMA foreign_keys=OFF");
        db.Exec(
            "CREATE TABLE sessions_v2(id TEXT PRIMARY KEY, started_at TEXT NOT NULL,"
            " ended_at TEXT, state TEXT NOT NULL, sample_rate INTEGER NOT NULL,"
            " device_id TEXT, device_name TEXT, lost_frames INTEGER NOT NULL DEFAULT 0);"
            "INSERT INTO sessions_v2 SELECT id, started_at, ended_at, state, sample_rate,"
            " device_id, device_name, lost_frames FROM sessions;"
            "DROP TABLE sessions;"
            "ALTER TABLE sessions_v2 RENAME TO sessions");
        db.SetUserVersion(2);
    }

    SqliteSessionStore migrated(root.path, kNever);
    Db db(root.DbPath());
    EXPECT_EQ(db.UserVersion(), 3);
    Db::Stmt row = db.Prepare("SELECT retain FROM sessions WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnInt64(0), 1) << "sessions from before the setting are kept";
    migrated.EraseUnretained();
    EXPECT_EQ(migrated.ListSessions().size(), 1u);
}

TEST(SessionStore, AnUnreadableOldSessionIsLeftInPlace) {
    TempRoot root;
    const SessionId id = WritePerSessionFileLayout(root, Ramp(100), "turn", "note");
    std::filesystem::remove(root.SessionFile(id, ".key"));  // no key: nothing can be read

    SqliteSessionStore store(root.path, kNever);
    EXPECT_TRUE(store.ListSessions().empty());
    EXPECT_TRUE(std::filesystem::exists(root.path / "main.db")) << "kept for a later attempt";
    EXPECT_TRUE(std::filesystem::exists(root.SessionFile(id, ".db")));
}

TEST(SessionStore, AnAbandonedSessionsTurnsAreReadable) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.AppendTurn(id, {0, 16000, "", "kept for recovery"});
    store.Abandon(id);

    const auto turns = store.ReadTurns(id);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].text, "kept for recovery");
}

TEST(SessionStore, ListSessionsIsNewestFirstWithStates) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId first = store.Begin({16000, "", ""});
    store.Finalise(first);
    const SessionId second = store.Begin({16000, "", ""});
    store.Abandon(second);

    const auto sessions = store.ListSessions();
    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[1].id, first);
    EXPECT_EQ(sessions[1].state, "finalised");
    EXPECT_FALSE(sessions[1].ended_at.empty());
    EXPECT_EQ(sessions[0].state, "recording") << "abandoned stays recoverable";
    EXPECT_TRUE(sessions[0].ended_at.empty());
}

TEST(SessionStore, DeleteErasesAFinishedSession) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Append(id, Ramp(16000), 0);
    store.AppendTurn(id, {0, 16000, "", "to be erased"});
    store.Finalise(id);

    store.Delete(id);

    EXPECT_TRUE(store.ListSessions().empty());
    EXPECT_THROW(store.ReadTurns(id), std::runtime_error);
    Db db(root.DbPath());
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM session_keys"), 0);
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM turns"), 0) << "cascade";
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM chunks"), 0) << "cascade";
}

TEST(SessionStore, TheRecordingSessionCannotBeReadOrDeleted) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});

    EXPECT_THROW(store.ReadTurns(id), std::runtime_error);
    EXPECT_THROW(store.Delete(id), std::runtime_error);
    store.Finalise(id);
}

TEST(SessionStore, UnknownIdsAreRefused) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    EXPECT_THROW(store.ReadTurns("nope"), std::runtime_error);
    EXPECT_THROW(store.Delete("nope"), std::runtime_error);
}

TEST(SessionStore, LifecycleSealsTheSessionAndErasesTheAudio) {
    TempRoot root;
    const auto audio = Ramp(40000);  // 2.5 s at 16 kHz
    SessionId id;
    {
        SqliteSessionStore store(root.path, 30ms);
        id = store.Begin({16000, "mic-1", "Test microphone"});
        store.Append(id, std::span(audio).subspan(0, 15000), 0);
        std::this_thread::sleep_for(150ms);  // the first half is on disk
        store.Append(id, std::span(audio).subspan(15000), 0);
        store.AppendTurn(id, {0, 40000, "doctor", "the record"});
        store.Finalise(id);

        EXPECT_TRUE(store.ReadAudio(id).empty()) << "the seal erases the recording";
        ASSERT_EQ(store.ReadTurns(id).size(), 1u) << "and keeps the transcript";
    }
    EXPECT_TRUE(DecryptSession(root, id).empty()) << "committed and pending alike";

    Db catalog(root.DbPath());
    Db::Stmt row = catalog.Prepare(
        "SELECT state, ended_at IS NOT NULL, sample_rate, device_name FROM sessions WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnText(0), "finalised");
    EXPECT_EQ(row.ColumnInt64(1), 1);
    EXPECT_EQ(row.ColumnInt64(2), 16000);
    EXPECT_EQ(row.ColumnText(3), "Test microphone");
}

TEST(SessionStore, OneDatabaseStampedWithTheSchemaVersion) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.Finalise(id);
    }
    EXPECT_FALSE(std::filesystem::exists(root.path / "sessions"));
    EXPECT_FALSE(std::filesystem::exists(root.path / "main.db"));

    Db db(root.DbPath());
    EXPECT_EQ(db.UserVersion(), 3);
    EXPECT_EQ(db.QueryInt64("PRAGMA auto_vacuum"), 2) << "incremental";
    EXPECT_EQ(db.QueryInt64("PRAGMA foreign_keys"), 1);
    Db::Stmt row = db.Prepare("SELECT id, sample_rate FROM sessions");
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnText(0), id);
    EXPECT_EQ(row.ColumnInt64(1), 16000);
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM session_keys"), 1);
}

TEST(SessionStore, AudioAtRestIsNotPlaintext) {
    TempRoot root;
    const auto audio = Ramp(32000);
    SessionId id;
    {
        SqliteSessionStore store(root.path, 30ms);
        id = store.Begin({16000, "", ""});
        store.Append(id, audio, 0);
        std::this_thread::sleep_for(150ms);
        store.Abandon(id);  // recoverable, so the audio is on disk
    }
    ASSERT_FALSE(DecryptSession(root, id).empty());

    // A 64-frame window from the middle of the input must not appear anywhere
    // in the raw session file
    const auto* window = reinterpret_cast<const std::uint8_t*>(audio.data() + 1000);
    const std::vector<std::uint8_t> needle(window, window + 64 * sizeof(float));
    const std::vector<std::uint8_t> file = ReadFileBytes(root.DbPath());
    EXPECT_EQ(std::search(file.begin(), file.end(), needle.begin(), needle.end()), file.end());
}

TEST(SessionStore, CancelRetainsNothing) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Append(id, Ramp(16000), 0);
    store.Cancel(id);

    EXPECT_EQ(store.ScanRecoverable().size(), 0u);
    EXPECT_THROW(store.ReadAudio(id), std::runtime_error) << "the key is gone";

    Db db(root.DbPath());
    for (const char* table : {"sessions", "session_keys", "chunks", "turns"}) {
        EXPECT_EQ(db.QueryInt64((std::string("SELECT COUNT(*) FROM ") + table).c_str()), 0)
            << table;
    }
}

TEST(SessionStore, RecordingWorksAgainAfterCancel) {
    TempRoot root;
    const auto audio = Ramp(16000);
    SessionId second;
    {
        SqliteSessionStore store(root.path, kNever);
        const SessionId first = store.Begin({16000, "", ""});
        store.Append(first, audio, 0);
        store.Cancel(first);

        second = store.Begin({16000, "", ""});
        store.Append(second, audio, 0);
        store.Abandon(second);
    }
    const auto chunks = DecryptSession(root, second);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].frames, audio);
}

TEST(SessionStore, AnUnfinalisedSessionIsRecoverable) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, 30ms);
        id = store.Begin({16000, "", ""});
        store.Append(id, Ramp(16000), 0);
        std::this_thread::sleep_for(150ms);  // let the writer commit
        // Neither finalised nor cancelled: the store dies with the session open
    }

    SqliteSessionStore reopened(root.path, kNever);
    const auto recoverable = reopened.ScanRecoverable();
    ASSERT_EQ(recoverable.size(), 1u);
    EXPECT_EQ(recoverable[0].id, id);
    EXPECT_EQ(recoverable[0].sample_rate, 16000);
    EXPECT_FALSE(DecryptSession(root, id).empty());
}

TEST(SessionStore, AbandonKeepsTheAudioAndFreesTheStore) {
    TempRoot root;
    const auto audio = Ramp(16000);
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Append(id, audio, 0);
    store.Abandon(id);

    // The recording survives, still in the recording state for recovery
    const auto recoverable = store.ScanRecoverable();
    ASSERT_EQ(recoverable.size(), 1u);
    EXPECT_EQ(recoverable[0].id, id);
    const auto chunks = DecryptSession(root, id);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].frames, audio);

    // And the store is free for the next session
    const SessionId next = store.Begin({16000, "", ""});
    store.Finalise(next);
}

TEST(SessionStore, FinalisedAndLiveSessionsAreNotRecoverable) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId done = store.Begin({16000, "", ""});
    store.Finalise(done);

    const SessionId live = store.Begin({16000, "", ""});
    EXPECT_EQ(store.ScanRecoverable().size(), 0u);
    store.Finalise(live);
}

TEST(SessionStore, TheWriterCommitsWhileRecording) {
    TempRoot root;
    const auto audio = Ramp(48000);  // 3 s
    SessionId id;
    {
        SqliteSessionStore store(root.path, 25ms);
        id = store.Begin({16000, "", ""});
        for (std::size_t offset = 0; offset < audio.size(); offset += 8000) {
            store.Append(id, std::span(audio).subspan(offset, 8000), 0);
            std::this_thread::sleep_for(40ms);
        }
        std::this_thread::sleep_for(60ms);  // the last append reaches disk
        store.Abandon(id);
    }

    const auto chunks = DecryptSession(root, id);
    EXPECT_GE(chunks.size(), 2u);
    std::vector<float> joined;
    std::int64_t expected_first = 0;
    for (const auto& chunk : chunks) {
        EXPECT_EQ(chunk.first_frame, expected_first);
        expected_first += static_cast<std::int64_t>(chunk.frames.size());
        joined.insert(joined.end(), chunk.frames.begin(), chunk.frames.end());
    }
    EXPECT_EQ(joined, audio);
}

TEST(SessionStore, LossAccountingReachesTheChunkAndTheCatalog) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        const SessionId abandoned = store.Begin({16000, "", ""});
        store.Append(abandoned, Ramp(8000), 320);
        store.Abandon(abandoned);
        const auto chunks = DecryptSession(root, abandoned);
        ASSERT_EQ(chunks.size(), 1u);
        EXPECT_EQ(chunks[0].lost_before, 320) << "the chunk carries the loss before it";

        id = store.Begin({16000, "", ""});
        store.Append(id, Ramp(8000), 320);
        store.Finalise(id);  // the audio goes; the count does not
    }

    Db catalog(root.DbPath());
    Db::Stmt row = catalog.Prepare("SELECT lost_frames FROM sessions WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnInt64(0), 320);
}

TEST(SessionStore, RefusesASecondSessionAndUnknownIds) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    EXPECT_THROW(store.Begin({16000, "", ""}), std::runtime_error);
    EXPECT_THROW(store.Append("not-it", Ramp(100), 0), std::runtime_error);
    EXPECT_THROW(store.Finalise("not-it"), std::runtime_error);
    EXPECT_THROW(store.Cancel("not-it"), std::runtime_error);
    store.Finalise(id);
    EXPECT_THROW(store.Append(id, Ramp(100), 0), std::runtime_error);
}

TEST(SessionStore, RefusesAStoreFromANewerBuild) {
    TempRoot root;
    {
        SqliteSessionStore store(root.path, kNever);
    }
    {
        Db db(root.DbPath());
        db.SetUserVersion(999);
    }
    EXPECT_THROW(SqliteSessionStore(root.path, kNever), std::runtime_error);
}

}  // namespace
}  // namespace sotto::store
