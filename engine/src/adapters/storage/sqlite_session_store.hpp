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

namespace sotto::store {

// A catalog (main.db) beside one encrypted db and wrapped key per session.
// Appends buffer in memory; a writer thread commits once per interval.
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
    void SaveNote(const SessionId& id, const std::string& text) override;
    std::string ReadNote(const SessionId& id) override;
    std::vector<asr::Turn> ReadTurns(const SessionId& id) override;
    void Delete(const SessionId& id) override;

   private:
    struct Open {
        SessionId id;
        std::optional<Db> db;
        std::optional<ChunkCipher> cipher;
        std::int64_t next_seq = 0;
        std::int64_t next_turn_seq = 0;
        std::uint64_t frames_committed = 0;
        std::uint64_t lost_committed = 0;
        std::vector<float> pending;
        std::uint64_t pending_lost = 0;
    };

    Open& RequireOpen(const SessionId& id);
    void RequireNotRecording(const SessionId& id);  // caller holds mutex_
    void EraseOnDisk(const SessionId& id);          // key, file, catalog row
    void CommitPending();  // seals pending as one chunk; caller holds mutex_
    void WriterLoop();

    std::filesystem::path root_;
    std::chrono::milliseconds commit_interval_;
    Db catalog_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Open> open_;
    bool stopping_ = false;
    std::thread writer_;
};

}  // namespace sotto::store
