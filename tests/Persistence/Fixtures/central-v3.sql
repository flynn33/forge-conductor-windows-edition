-- Immutable enriched central-database v3 lineage fixture.
-- This is the full C002+C003 shape retained for Windows migration compatibility.

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
    packet_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_context_handoffs_updated
    ON context_handoffs(updated_at DESC);

INSERT INTO schema_version(version) VALUES (3);
INSERT INTO context_handoffs(
    id, created_at, updated_at, source, resume_ready, packet_json
) VALUES (
    'legacy-v3-handoff',
    '2026-01-01T00:00:00Z',
    '2026-01-01T00:00:00Z',
    'model',
    1,
    '{"agents":[],"meta":{"created_at":"2026-01-01T00:00:00Z","id":"legacy-v3-handoff","resume_ready":true,"schema_version":1,"source":"model","updated_at":"2026-01-01T00:00:00Z"},"narrative":"central-v3-handoff-sentinel","resume":{"custom":true,"instructions":[],"seed":"continue legacy v3 migration"},"schema_version":1,"task":{"blockers":[],"goal":"Recover legacy continuity state","next_actions":["Migrate in place"],"status":"in_progress"},"working_set":{"decisions":[],"key_files":[]}}'
);

COMMIT;
