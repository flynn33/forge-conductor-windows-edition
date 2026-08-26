-- Immutable Forge Conductor 0.9.0 central-database fixture.
-- Schema source: SQLiteStore.swift migrateLockedDatabase(), lines 143-215.
-- The deterministic rows are migration-preservation sentinels, not production defaults.

PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS memory_notes (
    key TEXT PRIMARY KEY,
    body TEXT NOT NULL,
    tags_json TEXT NOT NULL DEFAULT '[]',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS agent_sessions (
    id TEXT PRIMARY KEY,
    agent_id TEXT NOT NULL,
    client_id TEXT,
    status TEXT NOT NULL,
    summary TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS presence (
    client_id TEXT PRIMARY KEY,
    host_kind TEXT,
    pid INTEGER,
    cwd TEXT,
    last_heartbeat TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS audit_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    client_id TEXT,
    tool TEXT NOT NULL,
    args_digest TEXT,
    args_json TEXT,
    status TEXT,
    duration_ms INTEGER,
    error TEXT
);
CREATE TABLE IF NOT EXISTS context_handoffs (
    id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source TEXT NOT NULL,
    resume_ready INTEGER NOT NULL DEFAULT 0,
    packet_json TEXT NOT NULL,
    client_id TEXT,
    write_sequence INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_context_handoffs_updated
    ON context_handoffs(updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_context_handoffs_sequence
    ON context_handoffs(write_sequence DESC);
CREATE INDEX IF NOT EXISTS idx_context_handoffs_client_sequence
    ON context_handoffs(client_id, write_sequence DESC);

INSERT INTO schema_version(version) VALUES (5);

INSERT INTO memory_notes(key, body, tags_json, created_at, updated_at) VALUES (
    'legacy/note',
    'central-v5-preservation-sentinel',
    '["legacy","migration"]',
    '2025-01-02T03:04:05Z',
    '2025-01-02T03:04:06Z'
);

INSERT INTO agent_sessions(
    id, agent_id, client_id, status, summary, created_at, updated_at
) VALUES (
    'legacy-session-1',
    'legacy-agent-1',
    'legacy-client-1',
    'running',
    'central-v5-agent-session-sentinel',
    '2025-01-02T03:04:07Z',
    '2025-01-02T03:04:08Z'
);

INSERT INTO presence(client_id, host_kind, pid, cwd, last_heartbeat) VALUES (
    'legacy-client-1',
    'cli',
    4242,
    'legacy-root',
    '2025-01-02T03:04:09Z'
);

INSERT INTO audit_events(
    id, timestamp, client_id, tool, args_digest, args_json, status, duration_ms, error
) VALUES (
    7,
    '2025-01-02T03:04:10Z',
    'legacy-client-1',
    'memory_set',
    'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd',
    '{"key":"legacy/note","value":"central-v5-preservation-sentinel"}',
    'ok',
    17,
    NULL
);

INSERT INTO context_handoffs(
    id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence
) VALUES (
    'legacy-handoff-1',
    '2025-01-02T03:04:11Z',
    '2025-01-02T03:04:12Z',
    'model',
    1,
    '{"agents":[],"narrative":"central-v5-handoff-sentinel","resume":{"custom":true,"instructions":[],"seed":"continue legacy migration"},"schema_version":1,"task":{"blockers":[],"cwd":"legacy-root","goal":"Preserve legacy central state","next_actions":["Continue migration"],"project_slug":"legacy-project","status":"in_progress"},"meta":{"client_id":"legacy-client-1","created_at":"2025-01-02T03:04:11Z","id":"legacy-handoff-1","resume_ready":true,"schema_version":1,"source":"model","updated_at":"2025-01-02T03:04:12Z"},"working_set":{"decisions":["Retain legacy rows"],"key_files":["README.md"]}}',
    'legacy-client-1',
    41
);

COMMIT;
