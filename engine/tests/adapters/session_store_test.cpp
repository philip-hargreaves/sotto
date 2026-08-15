#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

    std::filesystem::path SessionFile(const SessionId& id, const char* suffix) const {
        return path / "sessions" / (id + suffix);
    }
};

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

// Reads a finalised session the way recovery will: key file, then chunks
std::vector<StoredChunk> DecryptSession(const TempRoot& root, const SessionId& id) {
    const ChunkCipher cipher =
        ChunkCipher::FromWrapped(ReadFileBytes(root.SessionFile(id, ".key")));
    Db db(root.SessionFile(id, ".db"));
    Db::Stmt select =
        db.Prepare("SELECT seq, first_frame, lost_before, payload FROM chunks ORDER BY seq");
    std::vector<StoredChunk> chunks;
    std::int64_t expected_seq = 0;
    while (select.Step()) {
        EXPECT_EQ(select.ColumnInt64(0), expected_seq);
        const std::vector<std::uint8_t> plain =
            cipher.Open(id, static_cast<std::uint64_t>(expected_seq), select.ColumnBlob(3));
        StoredChunk chunk{select.ColumnInt64(1), select.ColumnInt64(2),
                          std::vector<float>(plain.size() / sizeof(float))};
        std::memcpy(chunk.frames.data(), plain.data(), plain.size());
        chunks.push_back(std::move(chunk));
        ++expected_seq;
    }
    return chunks;
}

TEST(SessionStore, LifecycleRoundTripsTheAudio) {
    TempRoot root;
    const auto audio = Ramp(40000);  // 2.5 s at 16 kHz
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "mic-1", "Test microphone"});
        store.Append(id, std::span(audio).subspan(0, 15000), 0);
        store.Append(id, std::span(audio).subspan(15000), 0);
        store.Finalise(id);
    }

    const auto chunks = DecryptSession(root, id);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].first_frame, 0);
    EXPECT_EQ(chunks[0].frames, audio);

    Db catalog(root.path / "main.db");
    Db::Stmt row = catalog.Prepare(
        "SELECT state, ended_at IS NOT NULL, sample_rate, device_name FROM sessions WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnText(0), "finalised");
    EXPECT_EQ(row.ColumnInt64(1), 1);
    EXPECT_EQ(row.ColumnInt64(2), 16000);
    EXPECT_EQ(row.ColumnText(3), "Test microphone");
}

TEST(SessionStore, StampsTheSchemaVersionAndMeta) {
    TempRoot root;
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.Finalise(id);
    }
    Db catalog(root.path / "main.db");
    EXPECT_EQ(catalog.UserVersion(), 1);

    Db session(root.SessionFile(id, ".db"));
    EXPECT_EQ(session.UserVersion(), 1);
    Db::Stmt meta = session.Prepare("SELECT value FROM meta WHERE key = 'id'");
    ASSERT_TRUE(meta.Step());
    EXPECT_EQ(meta.ColumnText(0), id);
}

TEST(SessionStore, AudioAtRestIsNotPlaintext) {
    TempRoot root;
    const auto audio = Ramp(32000);
    SessionId id;
    {
        SqliteSessionStore store(root.path, kNever);
        id = store.Begin({16000, "", ""});
        store.Append(id, audio, 0);
        store.Finalise(id);
    }

    // A 64-frame window from the middle of the input must not appear anywhere
    // in the raw session file
    const auto* window = reinterpret_cast<const std::uint8_t*>(audio.data() + 1000);
    const std::vector<std::uint8_t> needle(window, window + 64 * sizeof(float));
    const std::vector<std::uint8_t> file = ReadFileBytes(root.SessionFile(id, ".db"));
    EXPECT_EQ(std::search(file.begin(), file.end(), needle.begin(), needle.end()), file.end());
}

TEST(SessionStore, CancelRetainsNothing) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Append(id, Ramp(16000), 0);
    store.Cancel(id);

    EXPECT_FALSE(std::filesystem::exists(root.SessionFile(id, ".db")));
    EXPECT_FALSE(std::filesystem::exists(root.SessionFile(id, ".db-wal")));
    EXPECT_FALSE(std::filesystem::exists(root.SessionFile(id, ".key")));
    EXPECT_EQ(store.ScanRecoverable().size(), 0u);

    Db catalog(root.path / "main.db");
    EXPECT_EQ(catalog.QueryInt64("SELECT COUNT(*) FROM sessions"), 0);
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
        store.Finalise(second);
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
        store.Finalise(id);
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
        id = store.Begin({16000, "", ""});
        store.Append(id, Ramp(8000), 320);
        store.Finalise(id);
    }
    const auto chunks = DecryptSession(root, id);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].lost_before, 320);

    Db catalog(root.path / "main.db");
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
        Db catalog(root.path / "main.db");
        catalog.SetUserVersion(999);
    }
    EXPECT_THROW(SqliteSessionStore(root.path, kNever), std::runtime_error);
}

}  // namespace
}  // namespace sotto::store
