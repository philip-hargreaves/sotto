#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "ports/session_store.hpp"

namespace ambient::store {

// One ambient.db, content sealed per blob under per-session keys; a writer
// thread commits per interval. Layout in schema/ambient.sql
class SqliteSessionStore : public ISessionStore {
   public:
    explicit SqliteSessionStore(
        const std::filesystem::path& root,
        std::chrono::milliseconds commit_interval = std::chrono::seconds(1));
    ~SqliteSessionStore() override;

    SessionId Begin(const SessionMeta& meta) override;
    void Append(const SessionId& id, std::span<const float> frames,
                std::uint64_t lost_frames) override;
    void AppendTurn(const SessionId& id, const asr::Turn& turn) override;
    void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) override;
    void Finalise(const SessionId& id) override;
    void Cancel(const SessionId& id) override;
    void Abandon(const SessionId& id) override;
    std::vector<RecoverableSession> ScanRecoverable() override;
    std::vector<SessionSummary> ListSessions() override;
    void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) override;
    void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) override;
    Document ReadDocument(const SessionId& id, DocumentKind kind) override;
    std::vector<asr::Turn> ReadTurns(const SessionId& id) override;
    std::vector<float> ReadAudio(const SessionId& id) override;
    void Delete(const SessionId& id) override;
    void EraseUnretained() override;

   private:
    struct Open {
        SessionId id;
        std::optional<ChunkCipher> cipher;
        std::int64_t next_seq = 0;
        std::int64_t next_turn_seq = 0;
        std::uint64_t frames_committed = 0;
        std::uint64_t lost_committed = 0;
        std::vector<float> pending;
        std::uint64_t pending_lost = 0;
    };

    // All private members expect mutex_ held
    Open& RequireOpen(const SessionId& id);
    ChunkCipher CipherFor(const SessionId& id);  // a stored session's key; throws otherwise
    void InsertTurn(const SessionId& id, std::int64_t seq, const ChunkCipher& cipher,
                    const asr::Turn& turn);
    void WriteDocument(const SessionId& id, DocumentKind kind, const Document& document);
    Document ReadDocumentLocked(const SessionId& id, DocumentKind kind);
    void Erase(const SessionId& id);  // key row and everything under the session
    void CommitPending();             // seals pending as one chunk
    void WriterLoop();
    void ImportPerSessionFiles(const std::filesystem::path& root);

    std::chrono::milliseconds commit_interval_;
    Db db_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Open> open_;
    bool stopping_ = false;
    std::thread writer_;
};

}  // namespace ambient::store
