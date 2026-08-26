-- Immutable Forge Conductor released central-database v3 fixture.
-- Exact user-object shape: ContinuityTests.swift lines 985-1022.

PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE schema_version (version INTEGER NOT NULL);
INSERT INTO schema_version(version) VALUES (3);
CREATE TABLE context_handoffs (
    id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    source TEXT NOT NULL,
    resume_ready INTEGER NOT NULL DEFAULT 0,
    packet_json TEXT NOT NULL
);

INSERT INTO context_handoffs(
    id, created_at, updated_at, source, resume_ready, packet_json
) VALUES (
    'legacy-v3-handoff',
    '2026-01-01T00:00:00Z',
    '2026-01-01T00:00:00Z',
    'model',
    1,
    '{"agents":[],"meta":{"created_at":"2026-01-01T00:00:00Z","id":"legacy-v3-handoff","resume_ready":true,"schema_version":1,"source":"model","updated_at":"2026-01-01T00:00:00Z"},"narrative":"central-v3-minimal-handoff-sentinel","resume":{"custom":true,"instructions":[],"seed":"continue legacy v3 migration"},"schema_version":1,"task":{"blockers":[],"goal":"Recover legacy continuity state","next_actions":["Migrate in place"],"status":"in_progress"},"working_set":{"decisions":[],"key_files":[]}}'
);

COMMIT;
