PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL,
    content_sha256 TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS audit_events (
    id TEXT PRIMARY KEY,
    occurred_at TEXT NOT NULL,
    tool TEXT NOT NULL,
    status TEXT NOT NULL,
    client_id TEXT,
    duration_ms INTEGER NOT NULL,
    arguments_json TEXT NOT NULL,
    error_code TEXT,
    mutating INTEGER NOT NULL CHECK (mutating IN (0, 1))
);

CREATE INDEX IF NOT EXISTS audit_events_occurred_at
ON audit_events(occurred_at DESC);

CREATE TABLE IF NOT EXISTS client_presence (
    client_id TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    deployment_id TEXT,
    process_id INTEGER,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS agent_sessions (
    id TEXT PRIMARY KEY,
    project_id TEXT,
    agent_id TEXT NOT NULL,
    goal TEXT NOT NULL,
    cwd TEXT,
    status TEXT NOT NULL,
    report_json TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS memory_notes (
    key TEXT PRIMARY KEY,
    body TEXT NOT NULL,
    tags_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS context_handoffs (
    id TEXT PRIMARY KEY,
    project_id TEXT,
    session_id TEXT,
    payload_json TEXT NOT NULL,
    content_sha256 TEXT NOT NULL,
    resume_ready INTEGER NOT NULL CHECK (resume_ready IN (0, 1)),
    created_at TEXT NOT NULL
);
