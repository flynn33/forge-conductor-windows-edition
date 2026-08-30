-- Immutable Forge Conductor Windows central-database v6 fixture.
-- The deterministic rows prove C007 recovery and fail-closed pruning behavior.

PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    identifier TEXT NOT NULL UNIQUE,
    applied_at TEXT NOT NULL,
    content_sha256 TEXT NOT NULL
);
CREATE TABLE schema_version (version INTEGER NOT NULL);
CREATE TABLE memory_notes (
    key TEXT PRIMARY KEY,
    body TEXT NOT NULL,
    tags_json TEXT NOT NULL DEFAULT '[]',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE agent_sessions (
    id TEXT PRIMARY KEY,
    agent_id TEXT NOT NULL,
    client_id TEXT,
    status TEXT NOT NULL,
    summary TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    project_id TEXT,
    goal TEXT,
    cwd TEXT,
    report_json TEXT
);
CREATE TABLE presence (
    client_id TEXT PRIMARY KEY,
    host_kind TEXT,
    pid INTEGER,
    cwd TEXT,
    last_heartbeat TEXT NOT NULL
);
CREATE TABLE audit_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    client_id TEXT,
    tool TEXT NOT NULL,
    args_digest TEXT,
    args_json TEXT,
    status TEXT,
    duration_ms INTEGER,
    error TEXT,
    event_id TEXT,
    occurred_at TEXT,
    arguments_json TEXT,
    error_code TEXT,
    mutating INTEGER CHECK (mutating IN (0, 1))
);
CREATE TABLE context_handoffs (
    id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source TEXT NOT NULL,
    resume_ready INTEGER NOT NULL DEFAULT 0,
    packet_json TEXT NOT NULL,
    client_id TEXT,
    write_sequence INTEGER NOT NULL DEFAULT 0,
    project_id TEXT,
    session_id TEXT,
    payload_json TEXT,
    content_sha256 TEXT
);
CREATE TABLE client_presence (
    client_id TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    deployment_id TEXT,
    process_id INTEGER,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);

CREATE INDEX idx_context_handoffs_updated
    ON context_handoffs(updated_at DESC);
CREATE INDEX idx_context_handoffs_sequence
    ON context_handoffs(write_sequence DESC);
CREATE INDEX idx_context_handoffs_client_sequence
    ON context_handoffs(client_id, write_sequence DESC);
CREATE UNIQUE INDEX idx_audit_events_event_id
    ON audit_events(event_id)
    WHERE event_id IS NOT NULL;
CREATE INDEX idx_audit_events_occurred_at
    ON audit_events(occurred_at DESC);

INSERT INTO schema_migrations(
    version, identifier, applied_at, content_sha256
) VALUES
    (1, 'C001', '2026-01-02T03:04:05Z',
     '6d34b6a07a3d74440b598f2ca8b73ce84b615f99b814911b0f23e517e77c3eeb'),
    (2, 'C002', '2026-01-02T03:04:05Z',
     '3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315'),
    (3, 'C003', '2026-01-02T03:04:05Z',
     '600c16d28acd5f54a53a900d20e9ca51392a764e4bc9cdcb0b0b895a335173d9'),
    (4, 'C004', '2026-01-02T03:04:05Z',
     '653de9cd69b5a570b2269304715742375958e80335fead0a708362a134328936'),
    (5, 'C005', '2026-01-02T03:04:05Z',
     'e710c085f429574b82013d1bd5d711418147fdb15b91a1de7f74a83e14703cba'),
    (6, 'C006', '2026-01-02T03:04:05Z',
     '2f4ebc81ba122ca1a471504ce69fad1b11e7cbeecedd972024a521ebc849c427');
INSERT INTO schema_version(version) VALUES (6);

INSERT INTO presence(client_id, host_kind, pid, cwd, last_heartbeat) VALUES
    ('recoverable-client', 'cli', 6101, 'D:/workspaces/recoverable',
     '2026-08-29T12:00:00Z'),
    ('empty-directory-client', 'cli', 6102, '',
     '2026-08-29T12:01:00Z');

INSERT INTO client_presence(
    client_id, role, deployment_id, process_id, first_seen_at, last_seen_at
) VALUES
    ('recoverable-client', 'cli', NULL, 6101,
     '2026-08-29T12:00:00Z', '2026-08-29T12:00:05Z'),
    ('orphan-client', 'primary', 'deployment-orphan', 6199,
     '2026-08-29T12:02:00Z', '2026-08-29T12:02:05Z'),
    ('empty-directory-client', 'cli', NULL, 6102,
     '2026-08-29T12:01:00Z', '2026-08-29T12:01:05Z');

COMMIT;
