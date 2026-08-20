#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ports/transcriber.hpp"

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

struct SessionSummary {
    SessionId id;
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;    // Empty while recording or after a crash
    std::string state;       // recording | finalised
    int sample_rate = 0;
};

// The clinical store for one recording session at a time. Appended audio is
// durable within one second. Finalise keeps the recording, Cancel
// retains nothing; a session neither finalised nor cancelled is a crash
// and stays discoverable. Layout, SQL and encryption live behind this port.
class ISessionStore {
   public:
    virtual ~ISessionStore() = default;

    virtual SessionId Begin(const SessionMeta& meta) = 0;

    // lost_frames counts audio that belonged before these frames but never
    // arrived, mirroring the audio port
    virtual void Append(const SessionId& id, std::span<const float> frames,
                        std::uint64_t lost_frames) = 0;

    virtual void AppendTurn(const SessionId& id, const asr::Turn& turn) = 0;

    // The speaker-attributed transcript supersedes the live turns at
    // finalise; one transaction, before the session seals
    virtual void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) = 0;

    virtual void Finalise(const SessionId& id) = 0;
    virtual void Cancel(const SessionId& id) = 0;

    virtual void Abandon(const SessionId& id) = 0;

    virtual std::vector<RecoverableSession> ScanRecoverable() = 0;

    virtual std::vector<SessionSummary> ListSessions() = 0;

    // The clinical note and patient information, written after finalise;
    // reads are empty when nothing was saved and throw for an unknown session
    virtual void SaveNote(const SessionId& id, const std::string& text) = 0;
    virtual std::string ReadNote(const SessionId& id) = 0;
    virtual void SavePatient(const SessionId& id, const std::string& text) = 0;
    virtual std::string ReadPatient(const SessionId& id) = 0;

    // Read-back and disposal; both refuse the session currently recording
    virtual std::vector<asr::Turn> ReadTurns(const SessionId& id) = 0;
    virtual void Delete(const SessionId& id) = 0;
};

}  // namespace sotto::store
