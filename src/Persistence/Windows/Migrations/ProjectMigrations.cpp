#include "ProjectMigrations.h"

#include <array>

namespace ForgeConductor::Persistence::Windows::Migrations {
namespace {

constexpr std::string_view P001Sql = R"sql(CREATE TABLE memory_records(
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    version INTEGER NOT NULL,
    kind TEXT NOT NULL,
    title TEXT NOT NULL,
    summary TEXT NOT NULL,
    body TEXT,
    importance REAL NOT NULL,
    confidence REAL NOT NULL,
    source_kind TEXT NOT NULL,
    source_reference TEXT,
    session_id TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    last_accessed_at TEXT NOT NULL,
    expires_at TEXT,
    content_hash TEXT NOT NULL,
    is_tombstone INTEGER NOT NULL DEFAULT 0,
    schema_version INTEGER NOT NULL,
    idempotency_key TEXT,
    UNIQUE(project_id, kind, content_hash),
    UNIQUE(project_id, idempotency_key)
);
CREATE TABLE memory_tags(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
);
CREATE TABLE memory_record_tags(
    record_id TEXT NOT NULL REFERENCES memory_records(id) ON DELETE CASCADE,
    tag_id INTEGER NOT NULL REFERENCES memory_tags(id),
    PRIMARY KEY(record_id, tag_id)
);
CREATE TABLE memory_links(
    project_id TEXT NOT NULL,
    source_id TEXT NOT NULL REFERENCES memory_records(id),
    target_id TEXT NOT NULL REFERENCES memory_records(id),
    relation TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY(source_id, target_id, relation)
);
CREATE TABLE sessions(
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    state TEXT NOT NULL
);
CREATE TABLE handoffs(
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    record_id TEXT,
    created_at TEXT NOT NULL,
    acknowledged_at TEXT
);
CREATE TABLE artifacts(
    id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    path TEXT NOT NULL,
    checksum TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE project_aliases(
    project_id TEXT NOT NULL,
    alias TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY(project_id, alias)
);
CREATE TABLE maintenance_state(
    project_id TEXT PRIMARY KEY,
    last_run_at TEXT,
    state_json TEXT NOT NULL
);
CREATE TABLE event_journal(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id TEXT NOT NULL,
    record_id TEXT,
    action TEXT NOT NULL,
    detail TEXT,
    created_at TEXT NOT NULL
);
CREATE TABLE continuity_handoffs(
    handoff_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    operation_id TEXT NOT NULL UNIQUE,
    payload_json TEXT NOT NULL,
    content_sha256 TEXT NOT NULL,
    created_at TEXT NOT NULL,
    acknowledged_session_id TEXT,
    acknowledged_at TEXT
);
CREATE TABLE rollover_operations(
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
    state_checksum TEXT NOT NULL,
    UNIQUE(project_id, idempotency_key)
);
CREATE TABLE rollover_transitions(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    from_state TEXT,
    to_state TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    adapter_id TEXT NOT NULL,
    evidence TEXT,
    state_checksum TEXT NOT NULL
);
CREATE TABLE project_active_sessions(
    project_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX idx_rollover_active_project
    ON rollover_operations(project_id)
    WHERE state <> 'predecessorSealed';
CREATE INDEX idx_rollover_project_updated
    ON rollover_operations(project_id, updated_at DESC);
CREATE INDEX idx_memory_project_recent
    ON memory_records(project_id, is_tombstone, updated_at DESC);
CREATE INDEX idx_memory_project_kind
    ON memory_records(project_id, kind, is_tombstone);
CREATE INDEX idx_memory_project_session
    ON memory_records(project_id, session_id, is_tombstone);
PRAGMA user_version = 1;)sql";

constexpr std::string_view P002Sql = R"sql(CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    identifier TEXT NOT NULL UNIQUE,
    applied_at TEXT NOT NULL,
    content_sha256 TEXT NOT NULL
);
CREATE TABLE project_metadata (
    project_id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    repository_identity TEXT,
    schema_version INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
ALTER TABLE memory_records ADD COLUMN source TEXT;
UPDATE memory_records
SET source = COALESCE(source_reference, source_kind)
WHERE source IS NULL;
ALTER TABLE memory_links ADD COLUMN destination_id TEXT;
UPDATE memory_links
SET destination_id = target_id
WHERE destination_id IS NULL;
ALTER TABLE event_journal ADD COLUMN event_id TEXT;
ALTER TABLE event_journal ADD COLUMN event_type TEXT;
ALTER TABLE event_journal ADD COLUMN entity_id TEXT;
ALTER TABLE event_journal ADD COLUMN payload_json TEXT;
ALTER TABLE event_journal ADD COLUMN idempotency_key TEXT;
UPDATE event_journal
SET event_type = action,
    entity_id = record_id
WHERE event_type IS NULL;
CREATE UNIQUE INDEX idx_event_journal_event_id
    ON event_journal(event_id)
    WHERE event_id IS NOT NULL;
CREATE UNIQUE INDEX idx_event_journal_idempotency
    ON event_journal(project_id, idempotency_key)
    WHERE idempotency_key IS NOT NULL;
CREATE INDEX memory_records_project_recent
    ON memory_records(project_id, is_tombstone, updated_at DESC, id ASC);
PRAGMA user_version = 2;)sql";

constexpr std::string_view P003Sql = R"sql(ALTER TABLE rollover_operations ADD COLUMN retry_resume_state TEXT;
DROP INDEX idx_rollover_active_project;
CREATE UNIQUE INDEX idx_rollover_active_project
    ON rollover_operations(project_id)
    WHERE state NOT IN ('predecessorSealed','completed','cancelled');
PRAGMA user_version = 3;)sql";

constexpr std::string_view FtsSql = R"sql(CREATE VIRTUAL TABLE IF NOT EXISTS memory_records_fts
USING fts5(id UNINDEXED, title, summary, body);
CREATE TRIGGER IF NOT EXISTS memory_fts_insert
AFTER INSERT ON memory_records
WHEN new.is_tombstone = 0
BEGIN
    INSERT INTO memory_records_fts(id, title, summary, body)
    VALUES(new.id, new.title, new.summary, COALESCE(new.body, ''));
END;
CREATE TRIGGER IF NOT EXISTS memory_fts_update
AFTER UPDATE ON memory_records
BEGIN
    DELETE FROM memory_records_fts WHERE id = old.id;
    INSERT INTO memory_records_fts(id, title, summary, body)
    SELECT new.id, new.title, new.summary, COALESCE(new.body, '')
    WHERE new.is_tombstone = 0;
END;
CREATE TRIGGER IF NOT EXISTS memory_fts_delete
AFTER DELETE ON memory_records
BEGIN
    DELETE FROM memory_records_fts WHERE id = old.id;
END;)sql";

constexpr std::array<MigrationStep, 3> Steps{{
    {1, "P001", P001Sql, "9c9d3e635b2c75088da271ca773f4cea18aca862c40d77ad13f3d9ea183a514f"},
    {2, "P002", P002Sql, "69fa2b2c63f84903badba580bc0692804a9569a8b2a50b55b919e0c9881b0c08"},
    {3, "P003", P003Sql, "6a84a8c63e67ed4760ff589cb7ba96bec3ce25140c8e85c849b28b421f25acb9"},
}};

constexpr std::array<SchemaObject, 24> RequiredSchema{{
    {"table", "artifacts"},
    {"table", "continuity_handoffs"},
    {"table", "event_journal"},
    {"table", "handoffs"},
    {"table", "maintenance_state"},
    {"table", "memory_links"},
    {"table", "memory_record_tags"},
    {"table", "memory_records"},
    {"table", "memory_tags"},
    {"table", "project_active_sessions"},
    {"table", "project_aliases"},
    {"table", "project_metadata"},
    {"table", "rollover_operations"},
    {"table", "rollover_transitions"},
    {"table", "schema_migrations"},
    {"table", "sessions"},
    {"index", "idx_event_journal_event_id"},
    {"index", "idx_event_journal_idempotency"},
    {"index", "idx_memory_project_kind"},
    {"index", "idx_memory_project_recent"},
    {"index", "idx_memory_project_session"},
    {"index", "idx_rollover_active_project"},
    {"index", "idx_rollover_project_updated"},
    {"index", "memory_records_project_recent"},
}};

} // namespace

std::span<const MigrationStep> projectMigrationSteps() noexcept
{
    return Steps;
}

std::span<const SchemaObject> projectRequiredSchema() noexcept
{
    return RequiredSchema;
}

std::string_view projectFtsSql() noexcept
{
    return FtsSql;
}

} // namespace ForgeConductor::Persistence::Windows::Migrations
