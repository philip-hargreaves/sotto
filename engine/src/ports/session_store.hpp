#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sotto::store {

using SessionId = std::string;

struct SessionMeta {
    int sample_rate = 0;
    std::string device_id;
    std::string device_name;
};

struct RecoverableSession {
    SessionId id;
    std::string started_at;  // ISO 8601 UTC
    int sample_rate = 0;
};

// The clinical store for one recording session at a time. Appended audio is
// durable within one second (D3); Finalise keeps the recording, Cancel
// retains nothing (D5); a session neither finalised nor cancelled is a crash
// and stays discoverable. Layout, SQL and encryption live behind this port.
class ISessionStore {
   public:
    virtual ~ISessionStore() = default;

    virtual SessionId Begin(const SessionMeta& meta) = 0;

    // lost_frames counts audio that belonged before these frames but never
    // arrived, mirroring the audio port
    virtual void Append(const SessionId& id, std::span<const float> frames,
                        std::uint64_t lost_frames) = 0;

    virtual void Finalise(const SessionId& id) = 0;
    virtual void Cancel(const SessionId& id) = 0;

    // The session ended abnormally: keep every committed chunk, leave the
    // session in the recording state for the recovery scan, free the store
    // for the next Begin
    virtual void Abandon(const SessionId& id) = 0;

    virtual std::vector<RecoverableSession> ScanRecoverable() = 0;
};

}  // namespace sotto::store
