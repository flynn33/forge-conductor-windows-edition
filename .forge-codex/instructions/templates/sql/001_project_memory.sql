PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS project_metadata (
    project_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    repository_identity TEXT,
    schema_version INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS memory_records (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    title TEXT NOT NULL,
    summary TEXT NOT NULL,
    body TEXT,
    session_id TEXT,
    source TEXT,
    importance REAL NOT NULL,
    confidence REAL NOT NULL,
    version INTEGER NOT NULL,
    is_tombstone INTEGER NOT NULL CHECK (is_tombstone IN (0, 1)),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS memory_records_project_recent
ON memory_records(project_id, is_tombstone, updated_at DESC, id ASC);

CREATE TABLE IF NOT EXISTS memory_tags (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS memory_record_tags (
    record_id TEXT NOT NULL REFERENCES memory_records(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES memory_tags(id) ON DELETE CASCADE,
    PRIMARY KEY(record_id, tag_id)
);

CREATE TABLE IF NOT EXISTS memory_links (
    project_id TEXT NOT NULL,
    source_id TEXT NOT NULL REFERENCES memory_records(id),
    destination_id TEXT NOT NULL REFERENCES memory_records(id),
    relation TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY(project_id, source_id, destination_id, relation)
);

CREATE TABLE IF NOT EXISTS event_journal (
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    entity_id TEXT,
    payload_json TEXT NOT NULL,
    idempotency_key TEXT,
    created_at TEXT NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS event_journal_idempotency
ON event_journal(project_id, idempotency_key)
WHERE idempotency_key IS NOT NULL;

CREATE TABLE IF NOT EXISTS continuity_handoffs (
    handoff_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    operation_id TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    content_sha256 TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS rollover_operations (
    operation_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    predecessor_session_id TEXT NOT NULL,
    successor_session_id TEXT,
    handoff_id TEXT NOT NULL,
    state TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    adapter_id TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    acknowledged_session_id TEXT,
    acknowledged_handoff_id TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    last_error TEXT,
    retry_at TEXT,
    state_checksum TEXT NOT NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS rollover_idempotency
ON rollover_operations(project_id, idempotency_key);

CREATE TABLE IF NOT EXISTS rollover_transitions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id TEXT NOT NULL REFERENCES rollover_operations(operation_id) ON DELETE CASCADE,
    from_state TEXT,
    to_state TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    adapter_id TEXT NOT NULL,
    evidence TEXT,
    state_checksum TEXT NOT NULL,
    created_at TEXT NOT NULL
);
