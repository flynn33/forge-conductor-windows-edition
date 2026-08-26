-- Immutable Forge Conductor 0.9.0 per-project database fixture.
-- Schema source: ProjectMemoryRepository.swift migrateUnlocked(), lines 674-732.
-- Optional FTS5 objects from lines 735-749 are intentionally absent.
-- The deterministic rows are migration-preservation sentinels, not production defaults.

PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS memory_records(
    id TEXT PRIMARY KEY, project_id TEXT NOT NULL, version INTEGER NOT NULL,
    kind TEXT NOT NULL, title TEXT NOT NULL, summary TEXT NOT NULL, body TEXT,
    importance REAL NOT NULL, confidence REAL NOT NULL, source_kind TEXT NOT NULL,
    source_reference TEXT, session_id TEXT, created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL, last_accessed_at TEXT NOT NULL, expires_at TEXT,
    content_hash TEXT NOT NULL, is_tombstone INTEGER NOT NULL DEFAULT 0,
    schema_version INTEGER NOT NULL, idempotency_key TEXT,
    UNIQUE(project_id,kind,content_hash), UNIQUE(project_id,idempotency_key)
);
CREATE TABLE IF NOT EXISTS memory_tags(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT UNIQUE NOT NULL);
CREATE TABLE IF NOT EXISTS memory_record_tags(record_id TEXT NOT NULL REFERENCES memory_records(id) ON DELETE CASCADE,tag_id INTEGER NOT NULL REFERENCES memory_tags(id),PRIMARY KEY(record_id,tag_id));
CREATE TABLE IF NOT EXISTS memory_links(project_id TEXT NOT NULL,source_id TEXT NOT NULL REFERENCES memory_records(id),target_id TEXT NOT NULL REFERENCES memory_records(id),relation TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(source_id,target_id,relation));
CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,project_id TEXT NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,state TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS handoffs(id TEXT PRIMARY KEY,project_id TEXT NOT NULL,record_id TEXT,created_at TEXT NOT NULL,acknowledged_at TEXT);
CREATE TABLE IF NOT EXISTS artifacts(id TEXT PRIMARY KEY,project_id TEXT NOT NULL,path TEXT NOT NULL,checksum TEXT NOT NULL,created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS project_aliases(project_id TEXT NOT NULL,alias TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY KEY(project_id,alias));
CREATE TABLE IF NOT EXISTS maintenance_state(project_id TEXT PRIMARY KEY,last_run_at TEXT,state_json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS event_journal(id INTEGER PRIMARY KEY AUTOINCREMENT,project_id TEXT NOT NULL,record_id TEXT,action TEXT NOT NULL,detail TEXT,created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS continuity_handoffs(
    handoff_id TEXT PRIMARY KEY,project_id TEXT NOT NULL,operation_id TEXT NOT NULL UNIQUE,
    payload_json TEXT NOT NULL,content_sha256 TEXT NOT NULL,created_at TEXT NOT NULL,
    acknowledged_session_id TEXT,acknowledged_at TEXT
);
CREATE TABLE IF NOT EXISTS rollover_operations(
    operation_id TEXT PRIMARY KEY,project_id TEXT NOT NULL,predecessor_session_id TEXT NOT NULL,
    successor_session_id TEXT,handoff_id TEXT NOT NULL,state TEXT NOT NULL,attempt INTEGER NOT NULL,
    adapter_id TEXT NOT NULL,idempotency_key TEXT NOT NULL,acknowledged_session_id TEXT,
    acknowledged_handoff_id TEXT,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,
    last_error TEXT,retry_at TEXT,state_checksum TEXT NOT NULL,
    UNIQUE(project_id,idempotency_key)
);
CREATE TABLE IF NOT EXISTS rollover_transitions(
    id INTEGER PRIMARY KEY AUTOINCREMENT,operation_id TEXT NOT NULL,project_id TEXT NOT NULL,
    from_state TEXT,to_state TEXT NOT NULL,attempt INTEGER NOT NULL,created_at TEXT NOT NULL,
    adapter_id TEXT NOT NULL,evidence TEXT,state_checksum TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS project_active_sessions(
    project_id TEXT PRIMARY KEY,session_id TEXT NOT NULL,updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_rollover_active_project
    ON rollover_operations(project_id) WHERE state <> 'predecessorSealed';
CREATE INDEX IF NOT EXISTS idx_rollover_project_updated
    ON rollover_operations(project_id,updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_memory_project_recent ON memory_records(project_id,is_tombstone,updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_memory_project_kind ON memory_records(project_id,kind,is_tombstone);
CREATE INDEX IF NOT EXISTS idx_memory_project_session ON memory_records(project_id,session_id,is_tombstone);

INSERT INTO memory_records(
    id, project_id, version, kind, title, summary, body, importance, confidence,
    source_kind, source_reference, session_id, created_at, updated_at, last_accessed_at,
    expires_at, content_hash, is_tombstone, schema_version, idempotency_key
) VALUES (
    '22222222-2222-4222-8222-222222222222',
    '11111111-1111-4111-8111-111111111111',
    3,
    'decision',
    'Legacy decision',
    'project-v1-active-record-sentinel',
    'Preserve this record through migration.',
    0.8,
    0.9,
    'legacy_fixture',
    'legacy-source-a',
    '77777777-7777-4777-8777-777777777777',
    '2025-01-02T03:04:05Z',
    '2025-01-02T03:04:06Z',
    '2025-01-02T03:04:07Z',
    NULL,
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    0,
    1,
    'legacy-idempotency-a'
);

INSERT INTO memory_records(
    id, project_id, version, kind, title, summary, body, importance, confidence,
    source_kind, source_reference, session_id, created_at, updated_at, last_accessed_at,
    expires_at, content_hash, is_tombstone, schema_version, idempotency_key
) VALUES (
    '33333333-3333-4333-8333-333333333333',
    '11111111-1111-4111-8111-111111111111',
    2,
    'fact',
    'Legacy tombstone',
    'project-v1-tombstone-record-sentinel',
    NULL,
    0.25,
    0.5,
    'legacy_fixture',
    NULL,
    NULL,
    '2025-01-02T03:04:08Z',
    '2025-01-02T03:04:09Z',
    '2025-01-02T03:04:10Z',
    '2030-01-02T03:04:10Z',
    'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    1,
    1,
    NULL
);

INSERT INTO memory_tags(id, name) VALUES (1, 'legacy');
INSERT INTO memory_tags(id, name) VALUES (2, 'migration');
INSERT INTO memory_record_tags(record_id, tag_id) VALUES ('22222222-2222-4222-8222-222222222222', 1);
INSERT INTO memory_record_tags(record_id, tag_id) VALUES ('22222222-2222-4222-8222-222222222222', 2);

INSERT INTO memory_links(project_id, source_id, target_id, relation, created_at) VALUES (
    '11111111-1111-4111-8111-111111111111',
    '22222222-2222-4222-8222-222222222222',
    '33333333-3333-4333-8333-333333333333',
    'supersedes',
    '2025-01-02T03:04:11Z'
);

INSERT INTO sessions(id, project_id, created_at, updated_at, state) VALUES (
    '77777777-7777-4777-8777-777777777777',
    '11111111-1111-4111-8111-111111111111',
    '2025-01-02T03:04:12Z',
    '2025-01-02T03:04:13Z',
    '{"phase":"legacy","status":"active"}'
);

INSERT INTO handoffs(id, project_id, record_id, created_at, acknowledged_at) VALUES (
    '44444444-4444-4444-8444-444444444444',
    '11111111-1111-4111-8111-111111111111',
    '22222222-2222-4222-8222-222222222222',
    '2025-01-02T03:04:14Z',
    '2025-01-02T03:04:15Z'
);

INSERT INTO artifacts(id, project_id, path, checksum, created_at) VALUES (
    '88888888-8888-4888-8888-888888888888',
    '11111111-1111-4111-8111-111111111111',
    'artifacts/legacy.txt',
    'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',
    '2025-01-02T03:04:16Z'
);

INSERT INTO project_aliases(project_id, alias, created_at) VALUES (
    '11111111-1111-4111-8111-111111111111',
    'legacy-project',
    '2025-01-02T03:04:17Z'
);

INSERT INTO maintenance_state(project_id, last_run_at, state_json) VALUES (
    '11111111-1111-4111-8111-111111111111',
    '2025-01-02T03:04:18Z',
    '{"last_compaction":"legacy","schema_version":1}'
);

INSERT INTO event_journal(id, project_id, record_id, action, detail, created_at) VALUES (
    9,
    '11111111-1111-4111-8111-111111111111',
    '22222222-2222-4222-8222-222222222222',
    'remembered',
    'project-v1-event-sentinel',
    '2025-01-02T03:04:19Z'
);

INSERT INTO continuity_handoffs(
    handoff_id, project_id, operation_id, payload_json, content_sha256, created_at,
    acknowledged_session_id, acknowledged_at
) VALUES (
    '66666666-6666-4666-8666-666666666666',
    '11111111-1111-4111-8111-111111111111',
    '55555555-5555-4555-8555-555555555555',
    '{"completed_work":[{"summary":"Legacy checkpoint persisted"}],"constraints":["Preserve durable state"],"created_at":"2025-01-02T03:04:15Z","current_work":{"active_files":["README.md"],"phase_id":"P07","summary":"Migrate legacy persistence","work_item_id":"legacy-fixture"},"decisions":[{"decision":"Retain legacy rows"}],"evidence_references":[{"path":"legacy-evidence"}],"handoff_id":"66666666-6666-4666-8666-666666666666","host_state":{"adapter_id":"legacy-adapter","context_budget_source":"legacy-fixture","continuity_state":"checkpointPersisted","retry":{"attempt":2}},"integrity":{"content_sha256":"fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f","redaction_complete":true},"memory_references":[{"record_id":"22222222-2222-4222-8222-222222222222"}],"mission":"Preserve legacy project state","next_actions":[{"action":"Continue migration","command":"","order":1,"success_condition":"Legacy rows remain readable"}],"open_work":[{"summary":"Apply Windows migration"}],"operation_id":"55555555-5555-4555-8555-555555555555","predecessor_session":{"model":"legacy-model","provider_session_id":null,"session_id":"77777777-7777-4777-8777-777777777777"},"project":{"branch":"legacy","commit":"0123456789abcdef","dirty_summary":[],"display_name":"Legacy Project","project_id":"11111111-1111-4111-8111-111111111111","repository_root":"legacy-root"},"schema_version":"1.0","successor_session":null,"validation":{"commands":[],"open_gates":["G07"],"passed_gates":["G06"]}}',
    'fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f',
    '2025-01-02T03:04:15Z',
    NULL,
    NULL
);

INSERT INTO rollover_operations(
    operation_id, project_id, predecessor_session_id, successor_session_id, handoff_id,
    state, attempt, adapter_id, idempotency_key, acknowledged_session_id,
    acknowledged_handoff_id, created_at, updated_at, last_error, retry_at, state_checksum
) VALUES (
    '55555555-5555-4555-8555-555555555555',
    '11111111-1111-4111-8111-111111111111',
    '77777777-7777-4777-8777-777777777777',
    NULL,
    '66666666-6666-4666-8666-666666666666',
    'checkpointPersisted',
    2,
    'legacy-adapter',
    'legacy-rollover-idempotency',
    NULL,
    NULL,
    '2025-01-02T03:04:13Z',
    '2025-01-02T03:04:20Z',
    'legacy retry sentinel',
    '2025-01-02T03:05:20Z',
    '6671b074ded85ec1734239f7fe256051840ff23a7027338ab2bf76482102b298'
);

INSERT INTO rollover_transitions(
    id, operation_id, project_id, from_state, to_state, attempt, created_at,
    adapter_id, evidence, state_checksum
) VALUES (
    11,
    '55555555-5555-4555-8555-555555555555',
    '11111111-1111-4111-8111-111111111111',
    'checkpointPreparing',
    'checkpointPersisted',
    2,
    '2025-01-02T03:04:20Z',
    'legacy-adapter',
    'project-v1-transition-sentinel',
    '6671b074ded85ec1734239f7fe256051840ff23a7027338ab2bf76482102b298'
);

INSERT INTO project_active_sessions(project_id, session_id, updated_at) VALUES (
    '11111111-1111-4111-8111-111111111111',
    '77777777-7777-4777-8777-777777777777',
    '2025-01-02T03:04:20Z'
);

PRAGMA user_version=1;
COMMIT;
