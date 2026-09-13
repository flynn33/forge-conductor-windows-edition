-- Immutable Forge Conductor released central-database v9 fixture.
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

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
CREATE TABLE client_generation_bindings (
    client_id TEXT PRIMARY KEY,
    generation INTEGER NOT NULL,
    bound_at TEXT NOT NULL
);
CREATE TABLE client_presence (
    client_id TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    deployment_id TEXT,
    process_id INTEGER,
    working_directory TEXT NOT NULL,
    first_seen_at TEXT NOT NULL,
    last_seen_at TEXT NOT NULL
);
CREATE TABLE clu_operations (
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
CREATE TABLE clu_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id TEXT NOT NULL,
    timestamp TEXT NOT NULL,
    event_type TEXT NOT NULL,
    state TEXT NOT NULL,
    bounded_json TEXT NOT NULL DEFAULT '{}',
    FOREIGN KEY(operation_id) REFERENCES clu_operations(operation_id)
);
CREATE TABLE context_handoffs (
    id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source TEXT NOT NULL,
    resume_ready INTEGER NOT NULL DEFAULT 0,
    packet_json TEXT NOT NULL,
    write_sequence INTEGER NOT NULL DEFAULT 0,
    client_id TEXT,
    project_id TEXT,
    session_id TEXT,
    payload_json TEXT,
    content_sha256 TEXT
);
CREATE TABLE memory_notes (
    key TEXT PRIMARY KEY,
    body TEXT NOT NULL,
    tags_json TEXT NOT NULL DEFAULT '[]',
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
CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    identifier TEXT NOT NULL UNIQUE,
    applied_at TEXT NOT NULL,
    content_sha256 TEXT NOT NULL
);
CREATE TABLE schema_version (version INTEGER NOT NULL);
CREATE TABLE store_metadata (
    id INTEGER PRIMARY KEY,
    generation INTEGER NOT NULL,
    maintenance_state TEXT NOT NULL,
    active_reset_id TEXT,
    updated_at TEXT NOT NULL
);
CREATE INDEX idx_agent_sessions_created_id ON agent_sessions(created_at DESC, id DESC);
CREATE INDEX idx_agent_sessions_open_created_id
    ON agent_sessions(created_at DESC, id DESC)
    WHERE status IN ('open','active','running','started');
CREATE UNIQUE INDEX idx_audit_events_event_id
    ON audit_events(event_id) WHERE event_id IS NOT NULL;
CREATE INDEX idx_audit_events_occurred_at ON audit_events(occurred_at DESC);
CREATE INDEX idx_client_presence_last_seen_client
    ON client_presence(last_seen_at DESC, client_id DESC);
CREATE INDEX idx_clu_events_operation ON clu_events(operation_id, event_id);
CREATE INDEX idx_clu_operations_continuity
    ON clu_operations(continuity_id, created_at DESC);
CREATE UNIQUE INDEX idx_clu_operations_handoff_owner
    ON clu_operations(source_client_id, continuity_id);
CREATE INDEX idx_clu_operations_lease ON clu_operations(state, lease_expires_at);
CREATE INDEX idx_clu_operations_ready ON clu_operations(state, next_retry_at, created_at);
CREATE INDEX idx_clu_receipts_operation
    ON clu_provider_receipts(operation_id, phase, attempt);
CREATE INDEX idx_context_handoffs_client_sequence
    ON context_handoffs(client_id, write_sequence DESC);
CREATE INDEX idx_context_handoffs_sequence ON context_handoffs(write_sequence DESC);
CREATE INDEX idx_context_handoffs_updated ON context_handoffs(updated_at DESC);
CREATE INDEX idx_reset_receipts_started ON reset_receipts(started_at DESC);
INSERT INTO schema_version VALUES(9);
INSERT INTO schema_migrations VALUES(1,'C001','2026-08-27T16:08:17Z','6d34b6a07a3d74440b598f2ca8b73ce84b615f99b814911b0f23e517e77c3eeb');
INSERT INTO schema_migrations VALUES(2,'C002','2026-08-27T16:08:17Z','3c6fed9dd5aad4cda6d1bf511c48bfb27e450b68cba7b9446e6ddc9ef0d60315');
INSERT INTO schema_migrations VALUES(3,'C003','2026-08-27T16:08:17Z','600c16d28acd5f54a53a900d20e9ca51392a764e4bc9cdcb0b0b895a335173d9');
INSERT INTO schema_migrations VALUES(4,'C004','2026-08-27T16:08:17Z','653de9cd69b5a570b2269304715742375958e80335fead0a708362a134328936');
INSERT INTO schema_migrations VALUES(5,'C005','2026-08-27T16:08:17Z','e710c085f429574b82013d1bd5d711418147fdb15b91a1de7f74a83e14703cba');
INSERT INTO schema_migrations VALUES(6,'C006','2026-08-27T16:08:17Z','2f4ebc81ba122ca1a471504ce69fad1b11e7cbeecedd972024a521ebc849c427');
INSERT INTO schema_migrations VALUES(7,'C007','2026-09-04T23:21:34Z','e484d351fc622d0664bddeaa17a47b17929213a226341a98a9bed055df0864bd');
INSERT INTO schema_migrations VALUES(8,'C008','2026-09-04T23:21:34Z','d4aff22aa147de43bfb77437762421f55c0c667e14a51f4e229ee1d1df9d5368');
INSERT INTO schema_migrations VALUES(9,'C009','2026-09-04T23:21:34Z','3aadf0efc1844836e10bcab4927532767be92ed06b25cf9a12cb101c0dce6b11');
INSERT INTO store_metadata VALUES(1,1,'idle',NULL,'2026-09-04T23:21:34.277Z');
COMMIT;
