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

// The session store on disk: a catalog (main.db) beside one SQLite file and
// one DPAPI-wrapped key per session under sessions/ (ADR-0017). Appended
// audio buffers in memory and a writer thread seals it into an encrypted
// chunk once per commit interval, so capture never waits on a disk flush;
// the interval is the D3 loss bound and is a parameter only so tests can
// shrink it.
class SqliteSessionStore : public ISessionStore {
   public:
    explicit SqliteSessionStore(
        const std::filesystem::path& root,
        std::chrono::milliseconds commit_interval = std::chrono::seconds(1));
    ~SqliteSessionStore() override;

    SessionId Begin(const SessionMeta& meta) override;
    void Append(const SessionId& id, std::span<const float> frames,
                std::uint64_t lost_frames) override;
    void Finalise(const SessionId& id) override;
    void Cancel(const SessionId& id) override;
    std::vector<RecoverableSession> ScanRecoverable() override;

   private:
    struct Open {
        SessionId id;
        std::optional<Db> db;
        std::optional<ChunkCipher> cipher;
        std::int64_t next_seq = 0;
        std::uint64_t frames_committed = 0;
        std::uint64_t lost_committed = 0;
        std::vector<float> pending;
        std::uint64_t pending_lost = 0;
    };

    Open& RequireOpen(const SessionId& id);
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
