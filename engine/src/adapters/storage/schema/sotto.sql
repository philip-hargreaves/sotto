-- Sotto session store. One database, one writer thread.
--
-- Content (audio, transcript text, documents) is AES-256-GCM ciphertext under
-- a per-session key; the cipher authenticates the sealing domain, the session
-- id and the sequence, so a blob cannot be moved between rows or sessions
-- undetected. Shape (timing, state, options) is plaintext so it can be
-- queried without a key. Deleting a session's key row is the crypto-erase.

CREATE TABLE sessions (
    id           TEXT    PRIMARY KEY,             -- random 128-bit hex
    started_at   TEXT    NOT NULL,                -- ISO 8601 UTC
    ended_at     TEXT,                            -- NULL while recording or after a crash
    state        TEXT    NOT NULL
                 CHECK (state IN ('recording', 'finalised')),
    sample_rate  INTEGER NOT NULL,
    device_id    TEXT,                            -- capture device at the time, a snapshot
    device_name  TEXT,
    lost_frames  INTEGER NOT NULL DEFAULT 0,      -- frames the device dropped
    retain       INTEGER NOT NULL DEFAULT 1       -- 0: erased once the consultation is left
);

CREATE TABLE session_keys (
    session_id   TEXT    PRIMARY KEY REFERENCES sessions (id) ON DELETE CASCADE,
    wrapped      BLOB    NOT NULL                 -- AES key, DPAPI-wrapped for the user
);

-- Audio write-ahead log: committed once a second while recording, the basis
-- for resuming a crashed session, erased when the transcript is sealed
CREATE TABLE chunks (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,                -- also the nonce sequence
    first_frame  INTEGER NOT NULL,                -- position on the session timeline
    frame_count  INTEGER NOT NULL,
    lost_before  INTEGER NOT NULL,                -- frames dropped before this chunk
    payload      BLOB    NOT NULL,                -- sealed float32 frames, domain 0
    PRIMARY KEY (session_id, seq)
);

-- The transcript: live turns during capture, replaced in one transaction by
-- the speaker-attributed turns at finalise
CREATE TABLE turns (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    first_frame  INTEGER NOT NULL,
    frame_count  INTEGER NOT NULL,
    payload      BLOB    NOT NULL,                -- sealed {speaker, text}, domain 1
    PRIMARY KEY (session_id, seq)
);

-- One text of each kind per session; a rewrite replaces it
CREATE TABLE documents (
    session_id   TEXT    NOT NULL REFERENCES sessions (id) ON DELETE CASCADE,
    kind         TEXT    NOT NULL
                 CHECK (kind IN ('note', 'patient', 'translation', 'label')),
    language     TEXT    NOT NULL,                -- BCP 47
    payload      BLOB    NOT NULL,                -- sealed text, domain 2..5 by kind
    generated_at TEXT,                            -- when the model wrote it
    edited_at    TEXT,                            -- NULL until a person changed it
    PRIMARY KEY (session_id, kind)
);

-- The options the note was written with: a subtype of documents
CREATE TABLE note_options (
    session_id   TEXT    PRIMARY KEY,
    kind         TEXT    NOT NULL DEFAULT 'note' CHECK (kind = 'note'),
    style        TEXT    NOT NULL,                -- prose | soap
    detail       TEXT    NOT NULL,                -- concise | standard | detailed
    FOREIGN KEY (session_id, kind) REFERENCES documents (session_id, kind) ON DELETE CASCADE
);
