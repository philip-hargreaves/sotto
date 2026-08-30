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
    std::string label;      // The consultation in a line; empty until a note exists
    std::string edited_at;  // Latest clinician edit to any document; empty when none
};

// The texts a finalised session holds, one of each; a rewrite replaces
enum class DocumentKind { kNote, kPatient, kTranslation, kLabel };

struct Document {
    std::string text;             // Empty when the session has no such document
    std::string language = "en";  // BCP 47
    std::string style;            // Note only: prose | soap
    std::string detail;           // Note only: concise | standard | detailed
    std::string generated_at;     // ISO 8601 UTC; when the model wrote it
    std::string edited_at;        // ISO 8601 UTC; empty until a person changed it
};

// The clinical store for one recording session at a time. Appended audio is
// durable within one second and exists to resume a crash: Finalise seals
// the transcript and erases the audio, Cancel retains nothing; a session
// neither finalised nor cancelled is a crash and stays discoverable with
// its audio. Layout, SQL and encryption live behind this port.
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

    // Documents are written after finalise. Save records a generation
    // (stamps generated_at, clears edited_at); Edit records the clinician's
    // text over it (stamps edited_at, keeps the options). Reads return an
    // empty document when nothing was saved and throw for an unknown session
    virtual void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) = 0;
    virtual void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) = 0;
    virtual Document ReadDocument(const SessionId& id, DocumentKind kind) = 0;

    // Read-back and disposal; all refuse the session currently recording
    virtual std::vector<asr::Turn> ReadTurns(const SessionId& id) = 0;

    // The stored capture in order, the basis for resuming a crashed session
    virtual std::vector<float> ReadAudio(const SessionId& id) = 0;

    virtual void Delete(const SessionId& id) = 0;
};

}  // namespace sotto::store
