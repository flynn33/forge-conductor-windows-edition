#include "CentralMigrations.h"

#include <array>

namespace ForgeConductor::Persistence::Windows::Migrations {
namespace {

constexpr std::string_view C001Sql = R"sql(CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    identifier TEXT NOT NULL UNIQUE,
    applied_at TEXT NOT NULL,
    content_sha256 TEXT NOT NULL
);)sql";

constexpr std::string_view C002Sql = R"sql(CREATE TABLE schema_version (version INTEGER NOT NULL);
INSERT INTO schema_version(version) VALUES (2);
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
    updated_at TEXT NOT NULL
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
    error TEXT
);)sql";

constexpr std::string_view C003Sql = R"sql(CREATE TABLE context_handoffs (
    id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source TEXT NOT NULL,
    resume_ready INTEGER NOT NULL DEFAULT 0,
    packet_json TEXT NOT NULL
);
CREATE INDEX idx_context_handoffs_updated
    ON context_handoffs(updated_at DESC);
UPDATE schema_version SET version = 3;)sql";

constexpr std::string_view C004Sql = R"sql(ALTER TABLE context_handoffs
    ADD COLUMN write_sequence INTEGER NOT NULL DEFAULT 0;
UPDATE context_handoffs
SET write_sequence = rowid
WHERE write_sequence = 0;
CREATE INDEX idx_context_handoffs_sequence
    ON context_handoffs(write_sequence DESC);
UPDATE schema_version SET version = 4;)sql";

constexpr std::string_view C005Sql = R"sql(ALTER TABLE context_handoffs ADD COLUMN client_id TEXT;
CREATE INDEX idx_context_handoffs_client_sequence
    ON context_handoffs(client_id, write_sequence DESC);
UPDATE schema_version SET version = 5;)sql";

constexpr std::string_view C006Sql = R"sql(CREATE TABLE client_presence (
    client_id TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    deployment_id TEXT,
    process_id INTEGER,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);
INSERT OR IGNORE INTO client_presence(
    client_id, role, process_id, first_seen_at, last_seen_at
)
SELECT client_id, COALESCE(host_kind, 'unknown'), pid, last_heartbeat, last_heartbeat
FROM presence;
ALTER TABLE agent_sessions ADD COLUMN project_id TEXT;
ALTER TABLE agent_sessions ADD COLUMN goal TEXT;
ALTER TABLE agent_sessions ADD COLUMN cwd TEXT;
ALTER TABLE agent_sessions ADD COLUMN report_json TEXT;
ALTER TABLE context_handoffs ADD COLUMN project_id TEXT;
ALTER TABLE context_handoffs ADD COLUMN session_id TEXT;
ALTER TABLE context_handoffs ADD COLUMN payload_json TEXT;
ALTER TABLE context_handoffs ADD COLUMN content_sha256 TEXT;
UPDATE context_handoffs
SET payload_json = packet_json
WHERE payload_json IS NULL;
ALTER TABLE audit_events ADD COLUMN event_id TEXT;
ALTER TABLE audit_events ADD COLUMN occurred_at TEXT;
ALTER TABLE audit_events ADD COLUMN arguments_json TEXT;
ALTER TABLE audit_events ADD COLUMN error_code TEXT;
ALTER TABLE audit_events ADD COLUMN mutating INTEGER CHECK (mutating IN (0, 1));
UPDATE audit_events
SET occurred_at = timestamp,
    arguments_json = args_json
WHERE occurred_at IS NULL;
CREATE UNIQUE INDEX idx_audit_events_event_id
    ON audit_events(event_id)
    WHERE event_id IS NOT NULL;
CREATE INDEX idx_audit_events_occurred_at
    ON audit_events(occurred_at DESC);
UPDATE schema_version SET version = 6;)sql";

constexpr std::string_view C007Sql = R"sql(CREATE TABLE client_presence_v7 (
    client_id TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    deployment_id TEXT,
    process_id INTEGER,
    working_directory TEXT NOT NULL,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);
INSERT INTO client_presence_v7(
    client_id, role, deployment_id, process_id, working_directory,
    first_seen_at, last_seen_at
)
SELECT canonical.client_id, canonical.role, canonical.deployment_id,
       canonical.process_id, legacy.cwd,
       strftime('%Y-%m-%dT%H:%M:%fZ', canonical.first_seen_at),
       strftime('%Y-%m-%dT%H:%M:%fZ', canonical.last_seen_at)
FROM client_presence AS canonical
JOIN presence AS legacy
  ON legacy.client_id = canonical.client_id
 AND COALESCE(legacy.host_kind, 'unknown') = canonical.role
 AND legacy.pid IS canonical.process_id
 AND legacy.last_heartbeat = canonical.first_seen_at
WHERE canonical.deployment_id IS NULL
  AND typeof(legacy.cwd) = 'text'
  AND length(CAST(legacy.cwd AS BLOB)) BETWEEN 1 AND 32768
  AND instr(legacy.cwd, char(0)) = 0;
DROP TABLE client_presence;
ALTER TABLE client_presence_v7 RENAME TO client_presence;
UPDATE agent_sessions
SET created_at = strftime('%Y-%m-%dT%H:%M:%fZ', created_at),
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', updated_at);
CREATE INDEX idx_agent_sessions_created_id
    ON agent_sessions(created_at DESC, id DESC);
CREATE INDEX idx_agent_sessions_open_created_id
    ON agent_sessions(created_at DESC, id DESC)
    WHERE status IN ('open','active','running','started');
CREATE INDEX idx_client_presence_last_seen_client
    ON client_presence(last_seen_at DESC, client_id DESC);
UPDATE schema_version SET version = 7;)sql";

// C008 and C009 preserve the released legacy schema exactly. Their immutable
// ledger checksums are the values written by the original Windows release.
constexpr std::string_view C008Sql = R"sql(CREATE TABLE clu_operations (
    operation_id TEXT PRIMARY KEY NOT NULL,
    continuity_id TEXT NOT NULL,
    source_client_id TEXT NOT NULL,
    idempotency_key TEXT NOT NULL UNIQUE,
    request_fingerprint_sha256 TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL CHECK (state IN (
        'queued','claimed','resolving_handoff','handoff_resolved',
        'bootstrap_intent','bootstrap_response_received','bootstrap_accepted',
        'continuation_intent','continuation_response_received',
        'retry_wait','blocked','cancel_requested',
        'completed','failed','cancelled','quarantined'
    )),
    handoff_write_sequence INTEGER,
    handoff_sha256 TEXT,
    handoff_source TEXT,
    model_id TEXT,
    configuration_fingerprint_sha256 TEXT,
    bootstrap_response_id TEXT,
    continuation_response_id TEXT,
    accepted_provider_phase TEXT,
    worker_id TEXT,
    lease_expires_at TEXT,
    attempt INTEGER NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    next_retry_at TEXT,
    cancellation_reason TEXT,
    cancellation_requested_at TEXT,
    last_error_code TEXT,
    last_error_summary TEXT,
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision >= 1),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    completed_at TEXT,
    CHECK (length(continuity_id) BETWEEN 1 AND 128),
    CHECK (length(idempotency_key) BETWEEN 1 AND 128),
    CHECK (length(request_fingerprint_sha256) = 64),
    CHECK (handoff_sha256 IS NULL OR length(handoff_sha256) = 64),
    CHECK (configuration_fingerprint_sha256 IS NULL OR length(configuration_fingerprint_sha256) = 64)
);
CREATE INDEX idx_clu_operations_ready
    ON clu_operations(state, next_retry_at, created_at);
CREATE INDEX idx_clu_operations_continuity
    ON clu_operations(continuity_id, created_at DESC);
CREATE UNIQUE INDEX idx_clu_operations_handoff_owner
    ON clu_operations(source_client_id, continuity_id);
CREATE INDEX idx_clu_operations_lease
    ON clu_operations(state, lease_expires_at);
CREATE TABLE clu_provider_receipts (
    receipt_id TEXT PRIMARY KEY NOT NULL,
    operation_id TEXT NOT NULL,
    phase TEXT NOT NULL CHECK (phase IN ('bootstrap','continuation')),
    attempt INTEGER NOT NULL CHECK (attempt >= 1),
    request_fingerprint_sha256 TEXT NOT NULL,
    expected_previous_response_id TEXT,
    provider_response_id TEXT,
    model_instance_id TEXT,
    normalized_response_sha256 TEXT,
    disposition TEXT NOT NULL CHECK (disposition IN (
        'intent','received','accepted','quarantined','failed','unknown'
    )),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY(operation_id) REFERENCES clu_operations(operation_id),
    UNIQUE(operation_id, phase, attempt),
    UNIQUE(provider_response_id)
);
CREATE INDEX idx_clu_receipts_operation
    ON clu_provider_receipts(operation_id, phase, attempt);
CREATE TABLE clu_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id TEXT NOT NULL,
    timestamp TEXT NOT NULL,
    event_type TEXT NOT NULL,
    state TEXT NOT NULL,
    bounded_json TEXT NOT NULL DEFAULT '{}',
    FOREIGN KEY(operation_id) REFERENCES clu_operations(operation_id)
);
CREATE INDEX idx_clu_events_operation
    ON clu_events(operation_id, event_id);
UPDATE schema_version SET version = 8;)sql";

constexpr std::string_view C009Sql = R"sql(CREATE TABLE store_metadata (
    id INTEGER PRIMARY KEY,
    generation INTEGER NOT NULL,
    maintenance_state TEXT NOT NULL,
    active_reset_id TEXT,
    updated_at TEXT NOT NULL
);
INSERT INTO store_metadata(id, generation, maintenance_state, updated_at)
VALUES(1, 1, 'idle', strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));
CREATE TABLE client_generation_bindings (
    client_id TEXT PRIMARY KEY,
    generation INTEGER NOT NULL,
    bound_at TEXT NOT NULL
);
CREATE TABLE reset_receipts (
    reset_id TEXT PRIMARY KEY,
    scope_kind TEXT NOT NULL,
    scope_id TEXT NOT NULL,
    display_name TEXT NOT NULL,
    status TEXT NOT NULL,
    started_at TEXT NOT NULL,
    completed_at TEXT,
    old_generation INTEGER NOT NULL,
    new_generation INTEGER,
    targets_json TEXT NOT NULL,
    counts_before_json TEXT NOT NULL DEFAULT '{}',
    counts_deleted_json TEXT NOT NULL DEFAULT '{}',
    backup_json TEXT,
    error_code TEXT,
    recovery_action TEXT,
    application_build TEXT
);
CREATE INDEX idx_reset_receipts_started
    ON reset_receipts(started_at DESC);
UPDATE schema_version SET version = 9;)sql";

constexpr std::string_view C010Sql = R"sql(ALTER TABLE audit_events ADD COLUMN mcp_role TEXT
    CHECK (mcp_role IN ('primary', 'fallback', 'clu'));
ALTER TABLE audit_events ADD COLUMN deployment_id TEXT;
CREATE INDEX idx_audit_events_deployment
    ON audit_events(deployment_id, occurred_at DESC);
UPDATE schema_version SET version = 10;)sql";

constexpr std::string_view C011Sql = R"sql(ALTER TABLE audit_events ADD COLUMN project_id TEXT;
UPDATE schema_version SET version = 11;)sql";

constexpr std::array<MigrationStep, 11> Steps{{
    {1, "C001", C001Sql, "6d34b6a07a3d74440b598f2ca8b73ce84b615f99b814911b0f23e517e77c3eeb"},
    {2, "C002", C002Sql, "3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315"},
    {3, "C003", C003Sql, "600c16d28acd5f54a53a900d20e9ca51392a764e4bc9cdcb0b0b895a335173d9"},
    {4, "C004", C004Sql, "653de9cd69b5a570b2269304715742375958e80335fead0a708362a134328936"},
    {5, "C005", C005Sql, "e710c085f429574b82013d1bd5d711418147fdb15b91a1de7f74a83e14703cba"},
    {6, "C006", C006Sql, "2f4ebc81ba122ca1a471504ce69fad1b11e7cbeecedd972024a521ebc849c427"},
    {7, "C007", C007Sql, "e484d351fc622d0664bddeaa17a47b17929213a226341a98a9bed055df0864bd"},
    {8, "C008", C008Sql, "d4aff22aa147de43bfb77437762421f55c0c667e14a51f4e229ee1d1df9d5368"},
    {9, "C009", C009Sql, "3aadf0efc1844836e10bcab4927532767be92ed06b25cf9a12cb101c0dce6b11"},
    {10, "C010", C010Sql, "9a75c71681d072ecddb28b021f8bd5578cc783307eac6511e13ddf192346a0ab"},
    {11, "C011", C011Sql, "82bda99a53aec3474628ff80d3e23225f753b569ad43b8acccb8b631aec45830"},
}};

constexpr std::array<SchemaObject, 30> RequiredSchema{{
    {"table", "agent_sessions"},
    {"table", "audit_events"},
    {"table", "client_presence"},
    {"table", "context_handoffs"},
    {"table", "memory_notes"},
    {"table", "presence"},
    {"table", "schema_migrations"},
    {"table", "schema_version"},
    {"table", "clu_events"},
    {"table", "clu_operations"},
    {"table", "clu_provider_receipts"},
    {"table", "store_metadata"},
    {"table", "client_generation_bindings"},
    {"table", "reset_receipts"},
    {"index", "idx_audit_events_event_id"},
    {"index", "idx_audit_events_occurred_at"},
    {"index", "idx_audit_events_deployment"},
    {"index", "idx_agent_sessions_created_id"},
    {"index", "idx_agent_sessions_open_created_id"},
    {"index", "idx_client_presence_last_seen_client"},
    {"index", "idx_context_handoffs_client_sequence"},
    {"index", "idx_context_handoffs_sequence"},
    {"index", "idx_context_handoffs_updated"},
    {"index", "idx_clu_events_operation"},
    {"index", "idx_clu_operations_continuity"},
    {"index", "idx_clu_operations_handoff_owner"},
    {"index", "idx_clu_operations_lease"},
    {"index", "idx_clu_operations_ready"},
    {"index", "idx_clu_receipts_operation"},
    {"index", "idx_reset_receipts_started"},
}};

} // namespace

std::span<const MigrationStep> centralMigrationSteps() noexcept
{
    return Steps;
}

std::span<const SchemaObject> centralRequiredSchema() noexcept
{
    return RequiredSchema;
}

} // namespace ForgeConductor::Persistence::Windows::Migrations
