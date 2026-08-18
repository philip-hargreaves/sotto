#include "adapters/storage/sqlite_session_store.hpp"

#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <utility>

namespace sotto::store {

namespace {

constexpr std::int64_t kSchemaVersion = 1;

std::string Iso8601Now() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

std::string RandomId() {
    std::random_device device;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", device());
    return id;
}

std::span<const std::uint8_t> AsBytes(std::span<const float> frames) {
    return {reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size_bytes()};
}

// A file created by a newer build is refused, never best-effort read
void PrepareSchema(Db& db, const char* create_sql) {
    const std::int64_t version = db.UserVersion();
    if (version == 0) {
        Db::Transaction txn(db);
        db.Exec(create_sql);
        txn.Commit();
        db.SetUserVersion(kSchemaVersion);
    } else if (version > kSchemaVersion) {
        throw std::runtime_error("store schema is newer than this build");
    }
}

std::filesystem::path EnsureLayout(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "sessions");
    return root / "main.db";
}

}  // namespace

SqliteSessionStore::SqliteSessionStore(const std::filesystem::path& root,
                                       std::chrono::milliseconds commit_interval)
    : root_(root), commit_interval_(commit_interval), catalog_(EnsureLayout(root)) {
    PrepareSchema(catalog_,
                  "CREATE TABLE sessions("
                  "  id           TEXT PRIMARY KEY,"
                  "  started_at   TEXT NOT NULL,"
                  "  ended_at     TEXT,"
                  "  state        TEXT NOT NULL,"
                  "  sample_rate  INTEGER NOT NULL,"
                  "  device_id    TEXT,"
                  "  device_name  TEXT,"
                  "  lost_frames  INTEGER NOT NULL DEFAULT 0)");
    writer_ = std::thread([this] { WriterLoop(); });
}

SqliteSessionStore::~SqliteSessionStore() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    writer_.join();
    // Destruction is not finalisation; an open session stays recoverable
}

SessionId SqliteSessionStore::Begin(const SessionMeta& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.has_value()) throw std::runtime_error("a session is already recording");

    Open session;
    session.id = RandomId();
    const std::string started_at = Iso8601Now();

    // File and key first, catalog row last: a crash leaves an orphan file,
    // never a row promising audio the file does not hold
    session.db.emplace(root_ / "sessions" / (session.id + ".db"));
    PrepareSchema(*session.db,
                  "CREATE TABLE chunks("
                  "  seq          INTEGER PRIMARY KEY,"
                  "  first_frame  INTEGER NOT NULL,"
                  "  frame_count  INTEGER NOT NULL,"
                  "  lost_before  INTEGER NOT NULL,"
                  "  payload      BLOB NOT NULL);"
                  "CREATE TABLE turns("
                  "  seq          INTEGER PRIMARY KEY,"
                  "  first_frame  INTEGER NOT NULL,"
                  "  frame_count  INTEGER NOT NULL,"
                  "  payload      BLOB NOT NULL);"
                  "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    {
        Db::Transaction txn(*session.db);
        Db::Stmt insert = session.db->Prepare("INSERT INTO meta(key, value) VALUES(?, ?)");
        const std::pair<const char*, std::string> rows[] = {
            {"id", session.id},
            {"started_at", started_at},
            {"sample_rate", std::to_string(meta.sample_rate)},
        };
        for (const auto& [key, value] : rows) {
            insert.BindText(1, key);
            insert.BindText(2, value);
            insert.Step();
            insert.Reset();
        }
        txn.Commit();
    }

    session.cipher.emplace(ChunkCipher::Generate());
    {
        const std::vector<std::uint8_t> wrapped = session.cipher->Wrapped();
        std::ofstream key_file(root_ / "sessions" / (session.id + ".key"), std::ios::binary);
        key_file.write(reinterpret_cast<const char*>(wrapped.data()),
                       static_cast<std::streamsize>(wrapped.size()));
        if (!key_file) throw std::runtime_error("could not persist the session key");
    }

    Db::Stmt insert = catalog_.Prepare(
        "INSERT INTO sessions(id, started_at, state, sample_rate, device_id, device_name)"
        " VALUES(?, ?, 'recording', ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindText(2, started_at);
    insert.BindInt64(3, meta.sample_rate);
    insert.BindText(4, meta.device_id);
    insert.BindText(5, meta.device_name);
    insert.Step();

    open_.emplace(std::move(session));
    return open_->id;
}

void SqliteSessionStore::Append(const SessionId& id, std::span<const float> frames,
                                std::uint64_t lost_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    session.pending_lost += lost_frames;
    session.pending.insert(session.pending.end(), frames.begin(), frames.end());
}

void SqliteSessionStore::AppendTurn(const SessionId& id, const asr::Turn& turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);

    // Timing is queryable shape; speaker and text are content, so encrypted
    const std::string content =
        nlohmann::json{{"speaker", turn.speaker}, {"text", turn.text}}.dump();
    const std::vector<std::uint8_t> sealed = session.cipher->Seal(
        Domain::kTurns, session.id, static_cast<std::uint64_t>(session.next_turn_seq),
        {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});

    Db::Transaction txn(*session.db);
    Db::Stmt insert = session.db->Prepare(
        "INSERT INTO turns(seq, first_frame, frame_count, payload) VALUES(?, ?, ?, ?)");
    insert.BindInt64(1, session.next_turn_seq);
    insert.BindInt64(2, static_cast<std::int64_t>(turn.first_frame));
    insert.BindInt64(3, static_cast<std::int64_t>(turn.frame_count));
    insert.BindBlob(4, sealed);
    insert.Step();
    txn.Commit();

    session.next_turn_seq += 1;
}

void SqliteSessionStore::ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);

    Db::Transaction txn(*session.db);
    session.db->Prepare("DELETE FROM turns").Step();
    for (const asr::Turn& turn : turns) {
        // Sequence numbers continue rather than restart, so every sealed
        // payload's AAD stays unique for the session's lifetime
        const std::string content =
            nlohmann::json{{"speaker", turn.speaker}, {"text", turn.text}}.dump();
        const std::vector<std::uint8_t> sealed = session.cipher->Seal(
            Domain::kTurns, session.id, static_cast<std::uint64_t>(session.next_turn_seq),
            {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()});
        Db::Stmt insert = session.db->Prepare(
            "INSERT INTO turns(seq, first_frame, frame_count, payload) VALUES(?, ?, ?, ?)");
        insert.BindInt64(1, session.next_turn_seq);
        insert.BindInt64(2, static_cast<std::int64_t>(turn.first_frame));
        insert.BindInt64(3, static_cast<std::int64_t>(turn.frame_count));
        insert.BindBlob(4, sealed);
        insert.Step();
        session.next_turn_seq += 1;
    }
    txn.Commit();
}

void SqliteSessionStore::Finalise(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    if (!session.pending.empty() || session.pending_lost != 0) CommitPending();

    Db::Stmt update = catalog_.Prepare(
        "UPDATE sessions SET ended_at = ?, state = 'finalised', lost_frames = ? WHERE id = ?");
    update.BindText(1, Iso8601Now());
    update.BindInt64(2, static_cast<std::int64_t>(session.lost_committed));
    update.BindText(3, session.id);
    update.Step();

    open_.reset();
}

void SqliteSessionStore::Abandon(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    if (!session.pending.empty() || session.pending_lost != 0) CommitPending();
    // No catalog update: the recording state is what marks it recoverable
    open_.reset();
}

void SqliteSessionStore::Cancel(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    session.db.reset();
    EraseOnDisk(session.id);
    open_.reset();
}

void SqliteSessionStore::Delete(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireNotRecording(id);
    EraseOnDisk(id);
}

void SqliteSessionStore::EraseOnDisk(const SessionId& id) {
    // Key first: whatever survives of the file afterwards is unreadable
    const std::filesystem::path base = root_ / "sessions" / id;
    std::error_code ignored;
    std::filesystem::remove(base.string() + ".key", ignored);
    std::filesystem::remove(base.string() + ".db", ignored);
    std::filesystem::remove(base.string() + ".db-wal", ignored);
    std::filesystem::remove(base.string() + ".db-shm", ignored);

    Db::Stmt erase = catalog_.Prepare("DELETE FROM sessions WHERE id = ?");
    erase.BindText(1, id);
    erase.Step();
    if (catalog_.QueryInt64("SELECT changes()") == 0) {
        throw std::runtime_error("no session " + id);
    }
}

std::vector<RecoverableSession> SqliteSessionStore::ScanRecoverable() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoverableSession> found;
    Db::Stmt select = catalog_.Prepare(
        "SELECT id, started_at, sample_rate FROM sessions WHERE state = 'recording'");
    while (select.Step()) {
        RecoverableSession session{select.ColumnText(0), select.ColumnText(1),
                                   static_cast<int>(select.ColumnInt64(2))};
        if (open_.has_value() && session.id == open_->id) continue;  // live, not crashed
        found.push_back(std::move(session));
    }
    return found;
}

std::vector<SessionSummary> SqliteSessionStore::ListSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSummary> sessions;
    Db::Stmt select = catalog_.Prepare(
        "SELECT id, started_at, ended_at, state, sample_rate FROM sessions"
        " ORDER BY started_at DESC, rowid DESC");
    while (select.Step()) {
        sessions.push_back({select.ColumnText(0), select.ColumnText(1), select.ColumnText(2),
                            select.ColumnText(3), static_cast<int>(select.ColumnInt64(4))});
    }
    return sessions;
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurns(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireNotRecording(id);

    const std::filesystem::path base = root_ / "sessions" / id;
    if (!std::filesystem::exists(base.string() + ".db")) {
        throw std::runtime_error("no session " + id);
    }
    std::ifstream key_file(base.string() + ".key", std::ios::binary);
    const std::vector<std::uint8_t> wrapped((std::istreambuf_iterator<char>(key_file)),
                                            std::istreambuf_iterator<char>());
    const ChunkCipher cipher = ChunkCipher::FromWrapped(wrapped);

    Db db(base.string() + ".db");
    Db::Stmt select =
        db.Prepare("SELECT seq, first_frame, frame_count, payload FROM turns ORDER BY seq");
    std::vector<asr::Turn> turns;
    while (select.Step()) {
        const auto plain =
            cipher.Open(Domain::kTurns, id, static_cast<std::uint64_t>(select.ColumnInt64(0)),
                        select.ColumnBlob(3));
        const auto content = nlohmann::json::parse(plain.begin(), plain.end());
        asr::Turn turn;
        turn.first_frame = static_cast<std::uint64_t>(select.ColumnInt64(1));
        turn.frame_count = static_cast<std::uint64_t>(select.ColumnInt64(2));
        turn.speaker = content.at("speaker").get<std::string>();
        turn.text = content.at("text").get<std::string>();
        turns.push_back(std::move(turn));
    }
    return turns;
}

void SqliteSessionStore::RequireNotRecording(const SessionId& id) {
    if (open_.has_value() && open_->id == id) {
        throw std::runtime_error(id + " is still recording");
    }
}

SqliteSessionStore::Open& SqliteSessionStore::RequireOpen(const SessionId& id) {
    if (!open_.has_value() || open_->id != id) {
        throw std::runtime_error("no open session with id " + id);
    }
    return *open_;
}

void SqliteSessionStore::CommitPending() {
    Open& session = *open_;
    const std::vector<std::uint8_t> sealed = session.cipher->Seal(
        Domain::kAudio, session.id, static_cast<std::uint64_t>(session.next_seq),
        AsBytes(session.pending));

    Db::Transaction txn(*session.db);
    Db::Stmt insert = session.db->Prepare(
        "INSERT INTO chunks(seq, first_frame, frame_count, lost_before, payload)"
        " VALUES(?, ?, ?, ?, ?)");
    insert.BindInt64(1, session.next_seq);
    insert.BindInt64(2, static_cast<std::int64_t>(session.frames_committed));
    insert.BindInt64(3, static_cast<std::int64_t>(session.pending.size()));
    insert.BindInt64(4, static_cast<std::int64_t>(session.pending_lost));
    insert.BindBlob(5, sealed);
    insert.Step();
    txn.Commit();

    session.next_seq += 1;
    session.frames_committed += session.pending.size();
    session.lost_committed += session.pending_lost;
    session.pending.clear();
    session.pending_lost = 0;
}

void SqliteSessionStore::WriterLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, commit_interval_, [this] { return stopping_; });
        if (stopping_) break;
        if (open_.has_value() && (!open_->pending.empty() || open_->pending_lost != 0)) {
            CommitPending();
        }
    }
}

}  // namespace sotto::store
