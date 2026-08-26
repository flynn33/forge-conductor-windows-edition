#include "SchemaMigrator.h"

#include "CentralMigrations.h"
#include "ProjectMigrations.h"
#include "../Detail/WinsqliteConnection.h"
#include "../Detail/WinsqliteStatement.h"
#include "../Detail/WinsqliteTransaction.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows::Migrations
{
namespace
{

constexpr std::size_t MaximumSchemaTextBytes = 64U * 1024U;
constexpr std::size_t MaximumSchemaIdentifierBytes = 128U;
constexpr std::size_t MaximumColumnTypeBytes = 128U;
constexpr std::size_t MaximumDefaultValueBytes = 512U;
constexpr std::size_t MaximumAppliedAtBytes = 64U;
constexpr std::size_t MaximumBackupEvidenceIdBytes = 128U;
constexpr std::size_t MaximumUserObjects = 128U;
constexpr std::size_t MaximumTableColumns = 128U;
constexpr std::size_t MaximumIndexColumns = 32U;

struct ColumnSpec final
{
    std::string_view name;
    std::string_view type;
    bool notNull{};
    bool hasDefault{};
    std::string_view defaultValue;
    int primaryKeyOrder{};
};

struct TableSpec final
{
    std::string_view name;
    std::span<const ColumnSpec> columns;
    std::span<const ColumnSpec> alternateColumns;
};

struct IndexColumnSpec final
{
    std::string_view name;
    bool descending{};
};

struct IndexSpec final
{
    std::string_view name;
    std::string_view tableName;
    bool unique{};
    bool partial{};
    std::span<const IndexColumnSpec> columns;
    std::string_view partialPredicate;
};

struct LayoutSpec final
{
    DatabaseKind databaseKind{};
    SchemaLayout layout{};
    int sourceVersion{};
    int targetVersion{};
    bool requiresOnlineBackup{};
    bool requiresLedger{};
    std::span<const TableSpec> tables;
    std::span<const IndexSpec> indexes;
};

struct ObservedObject final
{
    std::string type;
    std::string name;
    std::string tableName;
};

struct ObservedColumn final
{
    int columnId{};
    std::string name;
    std::string type;
    bool notNull{};
    std::optional<std::string> defaultValue;
    int primaryKeyOrder{};
    int hidden{};
};

struct ObservedIndexColumn final
{
    int sequence{};
    int columnId{};
    std::string name;
    bool descending{};
    std::string collation;
};

struct UniqueConstraintSpec final
{
    std::span<const std::string_view> columns;
};

struct ForeignKeySpec final
{
    std::string_view referencedTable;
    std::string_view fromColumn;
    std::string_view toColumn;
    std::string_view onUpdate;
    std::string_view onDelete;
    std::string_view match;
};

struct ObservedConstraintIndex final
{
    std::string name;
    std::string origin;
    bool unique{};
    bool partial{};
    std::vector<std::string> columns;
};

constexpr ColumnSpec column(const std::string_view name, const std::string_view type,
                            const bool notNull = false, const int primaryKeyOrder = 0) noexcept
{
    return {name, type, notNull, false, {}, primaryKeyOrder};
}

constexpr ColumnSpec columnWithDefault(const std::string_view name, const std::string_view type,
                                       const bool notNull, const std::string_view defaultValue,
                                       const int primaryKeyOrder = 0) noexcept
{
    return {name, type, notNull, true, defaultValue, primaryKeyOrder};
}

constexpr std::array SchemaMigrationColumns{
    column("version", "INTEGER", false, 1),
    column("identifier", "TEXT", true),
    column("applied_at", "TEXT", true),
    column("content_sha256", "TEXT", true),
};

constexpr std::array SchemaVersionColumns{
    column("version", "INTEGER", true),
};

constexpr std::array MemoryNoteColumns{
    column("key", "TEXT", false, 1),
    column("body", "TEXT", true),
    columnWithDefault("tags_json", "TEXT", true, "'[]'"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
};

constexpr std::array CentralAgentSessionVersion5Columns{
    column("id", "TEXT", false, 1),     column("agent_id", "TEXT", true),
    column("client_id", "TEXT"),        column("status", "TEXT", true),
    column("summary", "TEXT"),          column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
};

constexpr std::array CentralAgentSessionVersion6Columns{
    column("id", "TEXT", false, 1),
    column("agent_id", "TEXT", true),
    column("client_id", "TEXT"),
    column("status", "TEXT", true),
    column("summary", "TEXT"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("project_id", "TEXT"),
    column("goal", "TEXT"),
    column("cwd", "TEXT"),
    column("report_json", "TEXT"),
};

constexpr std::array PresenceColumns{
    column("client_id", "TEXT", false, 1),
    column("host_kind", "TEXT"),
    column("pid", "INTEGER"),
    column("cwd", "TEXT"),
    column("last_heartbeat", "TEXT", true),
};

constexpr std::array AuditEventVersion5Columns{
    column("id", "INTEGER", false, 1), column("timestamp", "TEXT", true),
    column("client_id", "TEXT"),       column("tool", "TEXT", true),
    column("args_digest", "TEXT"),     column("args_json", "TEXT"),
    column("status", "TEXT"),          column("duration_ms", "INTEGER"),
    column("error", "TEXT"),
};

constexpr std::array AuditEventVersion6Columns{
    column("id", "INTEGER", false, 1), column("timestamp", "TEXT", true),
    column("client_id", "TEXT"),       column("tool", "TEXT", true),
    column("args_digest", "TEXT"),     column("args_json", "TEXT"),
    column("status", "TEXT"),          column("duration_ms", "INTEGER"),
    column("error", "TEXT"),           column("event_id", "TEXT"),
    column("occurred_at", "TEXT"),     column("arguments_json", "TEXT"),
    column("error_code", "TEXT"),      column("mutating", "INTEGER"),
};

constexpr std::array ContextHandoffVersion3Columns{
    column("id", "TEXT", false, 1),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("source", "TEXT", true),
    columnWithDefault("resume_ready", "INTEGER", true, "0"),
    column("packet_json", "TEXT", true),
};

constexpr std::array ContextHandoffVersion5FixtureColumns{
    column("id", "TEXT", false, 1),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("source", "TEXT", true),
    columnWithDefault("resume_ready", "INTEGER", true, "0"),
    column("packet_json", "TEXT", true),
    column("client_id", "TEXT"),
    columnWithDefault("write_sequence", "INTEGER", true, "0"),
};

constexpr std::array ContextHandoffVersion5ManifestColumns{
    column("id", "TEXT", false, 1),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("source", "TEXT", true),
    columnWithDefault("resume_ready", "INTEGER", true, "0"),
    column("packet_json", "TEXT", true),
    columnWithDefault("write_sequence", "INTEGER", true, "0"),
    column("client_id", "TEXT"),
};

constexpr std::array ContextHandoffVersion6FixtureColumns{
    column("id", "TEXT", false, 1),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("source", "TEXT", true),
    columnWithDefault("resume_ready", "INTEGER", true, "0"),
    column("packet_json", "TEXT", true),
    column("client_id", "TEXT"),
    columnWithDefault("write_sequence", "INTEGER", true, "0"),
    column("project_id", "TEXT"),
    column("session_id", "TEXT"),
    column("payload_json", "TEXT"),
    column("content_sha256", "TEXT"),
};

constexpr std::array ContextHandoffVersion6ManifestColumns{
    column("id", "TEXT", false, 1),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("source", "TEXT", true),
    columnWithDefault("resume_ready", "INTEGER", true, "0"),
    column("packet_json", "TEXT", true),
    columnWithDefault("write_sequence", "INTEGER", true, "0"),
    column("client_id", "TEXT"),
    column("project_id", "TEXT"),
    column("session_id", "TEXT"),
    column("payload_json", "TEXT"),
    column("content_sha256", "TEXT"),
};

constexpr std::array ClientPresenceColumns{
    column("client_id", "TEXT", false, 1), column("role", "TEXT", true),
    column("deployment_id", "TEXT"),       column("process_id", "INTEGER"),
    column("first_seen_at", "TEXT", true), column("last_seen_at", "TEXT", true),
};

constexpr std::array MemoryRecordVersion1Columns{
    column("id", "TEXT", false, 1),
    column("project_id", "TEXT", true),
    column("version", "INTEGER", true),
    column("kind", "TEXT", true),
    column("title", "TEXT", true),
    column("summary", "TEXT", true),
    column("body", "TEXT"),
    column("importance", "REAL", true),
    column("confidence", "REAL", true),
    column("source_kind", "TEXT", true),
    column("source_reference", "TEXT"),
    column("session_id", "TEXT"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("last_accessed_at", "TEXT", true),
    column("expires_at", "TEXT"),
    column("content_hash", "TEXT", true),
    columnWithDefault("is_tombstone", "INTEGER", true, "0"),
    column("schema_version", "INTEGER", true),
    column("idempotency_key", "TEXT"),
};

constexpr std::array MemoryRecordVersion2Columns{
    column("id", "TEXT", false, 1),
    column("project_id", "TEXT", true),
    column("version", "INTEGER", true),
    column("kind", "TEXT", true),
    column("title", "TEXT", true),
    column("summary", "TEXT", true),
    column("body", "TEXT"),
    column("importance", "REAL", true),
    column("confidence", "REAL", true),
    column("source_kind", "TEXT", true),
    column("source_reference", "TEXT"),
    column("session_id", "TEXT"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("last_accessed_at", "TEXT", true),
    column("expires_at", "TEXT"),
    column("content_hash", "TEXT", true),
    columnWithDefault("is_tombstone", "INTEGER", true, "0"),
    column("schema_version", "INTEGER", true),
    column("idempotency_key", "TEXT"),
    column("source", "TEXT"),
};

constexpr std::array MemoryTagColumns{
    column("id", "INTEGER", false, 1),
    column("name", "TEXT", true),
};

constexpr std::array MemoryRecordTagColumns{
    column("record_id", "TEXT", true, 1),
    column("tag_id", "INTEGER", true, 2),
};

constexpr std::array MemoryLinkVersion1Columns{
    column("project_id", "TEXT", true),   column("source_id", "TEXT", true, 1),
    column("target_id", "TEXT", true, 2), column("relation", "TEXT", true, 3),
    column("created_at", "TEXT", true),
};

constexpr std::array MemoryLinkVersion2Columns{
    column("project_id", "TEXT", true),   column("source_id", "TEXT", true, 1),
    column("target_id", "TEXT", true, 2), column("relation", "TEXT", true, 3),
    column("created_at", "TEXT", true),   column("destination_id", "TEXT"),
};

constexpr std::array SessionColumns{
    column("id", "TEXT", false, 1),     column("project_id", "TEXT", true),
    column("created_at", "TEXT", true), column("updated_at", "TEXT", true),
    column("state", "TEXT", true),
};

constexpr std::array HandoffColumns{
    column("id", "TEXT", false, 1),    column("project_id", "TEXT", true),
    column("record_id", "TEXT"),       column("created_at", "TEXT", true),
    column("acknowledged_at", "TEXT"),
};

constexpr std::array ArtifactColumns{
    column("id", "TEXT", false, 1),     column("project_id", "TEXT", true),
    column("path", "TEXT", true),       column("checksum", "TEXT", true),
    column("created_at", "TEXT", true),
};

constexpr std::array ProjectAliasColumns{
    column("project_id", "TEXT", true, 1),
    column("alias", "TEXT", true, 2),
    column("created_at", "TEXT", true),
};

constexpr std::array MaintenanceStateColumns{
    column("project_id", "TEXT", false, 1),
    column("last_run_at", "TEXT"),
    column("state_json", "TEXT", true),
};

constexpr std::array EventJournalVersion1Columns{
    column("id", "INTEGER", false, 1), column("project_id", "TEXT", true),
    column("record_id", "TEXT"),       column("action", "TEXT", true),
    column("detail", "TEXT"),          column("created_at", "TEXT", true),
};

constexpr std::array EventJournalVersion2Columns{
    column("id", "INTEGER", false, 1), column("project_id", "TEXT", true),
    column("record_id", "TEXT"),       column("action", "TEXT", true),
    column("detail", "TEXT"),          column("created_at", "TEXT", true),
    column("event_id", "TEXT"),        column("event_type", "TEXT"),
    column("entity_id", "TEXT"),       column("payload_json", "TEXT"),
    column("idempotency_key", "TEXT"),
};

constexpr std::array ContinuityHandoffColumns{
    column("handoff_id", "TEXT", false, 1),    column("project_id", "TEXT", true),
    column("operation_id", "TEXT", true),      column("payload_json", "TEXT", true),
    column("content_sha256", "TEXT", true),    column("created_at", "TEXT", true),
    column("acknowledged_session_id", "TEXT"), column("acknowledged_at", "TEXT"),
};

constexpr std::array RolloverOperationVersion2Columns{
    column("operation_id", "TEXT", false, 1),
    column("project_id", "TEXT", true),
    column("predecessor_session_id", "TEXT", true),
    column("successor_session_id", "TEXT"),
    column("handoff_id", "TEXT", true),
    column("state", "TEXT", true),
    column("attempt", "INTEGER", true),
    column("adapter_id", "TEXT", true),
    column("idempotency_key", "TEXT", true),
    column("acknowledged_session_id", "TEXT"),
    column("acknowledged_handoff_id", "TEXT"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("last_error", "TEXT"),
    column("retry_at", "TEXT"),
    column("state_checksum", "TEXT", true),
};

constexpr std::array RolloverOperationVersion3Columns{
    column("operation_id", "TEXT", false, 1),
    column("project_id", "TEXT", true),
    column("predecessor_session_id", "TEXT", true),
    column("successor_session_id", "TEXT"),
    column("handoff_id", "TEXT", true),
    column("state", "TEXT", true),
    column("attempt", "INTEGER", true),
    column("adapter_id", "TEXT", true),
    column("idempotency_key", "TEXT", true),
    column("acknowledged_session_id", "TEXT"),
    column("acknowledged_handoff_id", "TEXT"),
    column("created_at", "TEXT", true),
    column("updated_at", "TEXT", true),
    column("last_error", "TEXT"),
    column("retry_at", "TEXT"),
    column("state_checksum", "TEXT", true),
    column("retry_resume_state", "TEXT"),
};

constexpr std::array RolloverTransitionColumns{
    column("id", "INTEGER", false, 1),  column("operation_id", "TEXT", true),
    column("project_id", "TEXT", true), column("from_state", "TEXT"),
    column("to_state", "TEXT", true),   column("attempt", "INTEGER", true),
    column("created_at", "TEXT", true), column("adapter_id", "TEXT", true),
    column("evidence", "TEXT"),         column("state_checksum", "TEXT", true),
};

constexpr std::array ProjectActiveSessionColumns{
    column("project_id", "TEXT", false, 1),
    column("session_id", "TEXT", true),
    column("updated_at", "TEXT", true),
};

constexpr std::array ProjectMetadataColumns{
    column("project_id", "TEXT", false, 1), column("display_name", "TEXT", true),
    column("repository_identity", "TEXT"),  column("schema_version", "INTEGER", true),
    column("created_at", "TEXT", true),     column("updated_at", "TEXT", true),
};

constexpr std::array<std::string_view, 1> IdentifierUniqueColumns{"identifier"};
constexpr std::array<std::string_view, 3> MemoryContentUniqueColumns{"project_id", "kind",
                                                                     "content_hash"};
constexpr std::array<std::string_view, 2> MemoryIdempotencyUniqueColumns{"project_id",
                                                                         "idempotency_key"};
constexpr std::array<std::string_view, 1> NameUniqueColumns{"name"};
constexpr std::array<std::string_view, 1> OperationIdUniqueColumns{"operation_id"};
constexpr std::array<std::string_view, 2> RolloverIdempotencyUniqueColumns{"project_id",
                                                                           "idempotency_key"};

constexpr std::array SchemaMigrationUniqueConstraints{
    UniqueConstraintSpec{IdentifierUniqueColumns},
};
constexpr std::array MemoryRecordUniqueConstraints{
    UniqueConstraintSpec{MemoryContentUniqueColumns},
    UniqueConstraintSpec{MemoryIdempotencyUniqueColumns},
};
constexpr std::array MemoryTagUniqueConstraints{
    UniqueConstraintSpec{NameUniqueColumns},
};
constexpr std::array ContinuityHandoffUniqueConstraints{
    UniqueConstraintSpec{OperationIdUniqueColumns},
};
constexpr std::array RolloverOperationUniqueConstraints{
    UniqueConstraintSpec{RolloverIdempotencyUniqueColumns},
};

constexpr std::array MemoryRecordTagForeignKeys{
    ForeignKeySpec{"memory_records", "record_id", "id", "NO ACTION", "CASCADE", "NONE"},
    ForeignKeySpec{"memory_tags", "tag_id", "id", "NO ACTION", "NO ACTION", "NONE"},
};
constexpr std::array MemoryLinkForeignKeys{
    ForeignKeySpec{"memory_records", "source_id", "id", "NO ACTION", "NO ACTION", "NONE"},
    ForeignKeySpec{"memory_records", "target_id", "id", "NO ACTION", "NO ACTION", "NONE"},
};

constexpr std::array UpdatedAtDescendingIndexColumns{
    IndexColumnSpec{"updated_at", true},
};

constexpr std::array WriteSequenceDescendingIndexColumns{
    IndexColumnSpec{"write_sequence", true},
};

constexpr std::array ClientWriteSequenceIndexColumns{
    IndexColumnSpec{"client_id", false},
    IndexColumnSpec{"write_sequence", true},
};

constexpr std::array EventIdIndexColumns{
    IndexColumnSpec{"event_id", false},
};

constexpr std::array OccurredAtDescendingIndexColumns{
    IndexColumnSpec{"occurred_at", true},
};

constexpr std::array ProjectIdIndexColumns{
    IndexColumnSpec{"project_id", false},
};

constexpr std::array ProjectUpdatedIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"updated_at", true},
};

constexpr std::array ProjectRecentIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"is_tombstone", false},
    IndexColumnSpec{"updated_at", true},
};

constexpr std::array ProjectKindIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"kind", false},
    IndexColumnSpec{"is_tombstone", false},
};

constexpr std::array ProjectSessionIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"session_id", false},
    IndexColumnSpec{"is_tombstone", false},
};

constexpr std::array ProjectIdempotencyIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"idempotency_key", false},
};

constexpr std::array ProjectRecentStableIndexColumns{
    IndexColumnSpec{"project_id", false},
    IndexColumnSpec{"is_tombstone", false},
    IndexColumnSpec{"updated_at", true},
    IndexColumnSpec{"id", false},
};

constexpr std::array CentralVersion3MinimalTables{
    TableSpec{"context_handoffs", ContextHandoffVersion3Columns, {}},
    TableSpec{"schema_version", SchemaVersionColumns, {}},
};

constexpr std::array<IndexSpec, 0> CentralVersion3MinimalIndexes{};

constexpr std::array CentralVersion3Tables{
    TableSpec{"agent_sessions", CentralAgentSessionVersion5Columns, {}},
    TableSpec{"audit_events", AuditEventVersion5Columns, {}},
    TableSpec{"context_handoffs", ContextHandoffVersion3Columns, {}},
    TableSpec{"memory_notes", MemoryNoteColumns, {}},
    TableSpec{"presence", PresenceColumns, {}},
    TableSpec{"schema_version", SchemaVersionColumns, {}},
};

constexpr std::array CentralVersion3Indexes{
    IndexSpec{"idx_context_handoffs_updated",
              "context_handoffs",
              false,
              false,
              UpdatedAtDescendingIndexColumns,
              {}},
};

constexpr std::array CentralVersion5Tables{
    TableSpec{"agent_sessions", CentralAgentSessionVersion5Columns, {}},
    TableSpec{"audit_events", AuditEventVersion5Columns, {}},
    TableSpec{"context_handoffs", ContextHandoffVersion5FixtureColumns, {}},
    TableSpec{"memory_notes", MemoryNoteColumns, {}},
    TableSpec{"presence", PresenceColumns, {}},
    TableSpec{"schema_version", SchemaVersionColumns, {}},
};

constexpr std::array CentralVersion5Indexes{
    IndexSpec{"idx_context_handoffs_client_sequence",
              "context_handoffs",
              false,
              false,
              ClientWriteSequenceIndexColumns,
              {}},
    IndexSpec{"idx_context_handoffs_sequence",
              "context_handoffs",
              false,
              false,
              WriteSequenceDescendingIndexColumns,
              {}},
    IndexSpec{"idx_context_handoffs_updated",
              "context_handoffs",
              false,
              false,
              UpdatedAtDescendingIndexColumns,
              {}},
};

constexpr std::array CentralVersion6Tables{
    TableSpec{"agent_sessions", CentralAgentSessionVersion6Columns, {}},
    TableSpec{"audit_events", AuditEventVersion6Columns, {}},
    TableSpec{"client_presence", ClientPresenceColumns, {}},
    TableSpec{"context_handoffs", ContextHandoffVersion6FixtureColumns,
              ContextHandoffVersion6ManifestColumns},
    TableSpec{"memory_notes", MemoryNoteColumns, {}},
    TableSpec{"presence", PresenceColumns, {}},
    TableSpec{"schema_migrations", SchemaMigrationColumns, {}},
    TableSpec{"schema_version", SchemaVersionColumns, {}},
};

constexpr std::array CentralVersion6Indexes{
    IndexSpec{"idx_audit_events_event_id", "audit_events", true, true, EventIdIndexColumns,
              "event_id IS NOT NULL"},
    IndexSpec{"idx_audit_events_occurred_at",
              "audit_events",
              false,
              false,
              OccurredAtDescendingIndexColumns,
              {}},
    IndexSpec{"idx_context_handoffs_client_sequence",
              "context_handoffs",
              false,
              false,
              ClientWriteSequenceIndexColumns,
              {}},
    IndexSpec{"idx_context_handoffs_sequence",
              "context_handoffs",
              false,
              false,
              WriteSequenceDescendingIndexColumns,
              {}},
    IndexSpec{"idx_context_handoffs_updated",
              "context_handoffs",
              false,
              false,
              UpdatedAtDescendingIndexColumns,
              {}},
};

constexpr std::array ProjectVersion1Tables{
    TableSpec{"artifacts", ArtifactColumns, {}},
    TableSpec{"continuity_handoffs", ContinuityHandoffColumns, {}},
    TableSpec{"event_journal", EventJournalVersion1Columns, {}},
    TableSpec{"handoffs", HandoffColumns, {}},
    TableSpec{"maintenance_state", MaintenanceStateColumns, {}},
    TableSpec{"memory_links", MemoryLinkVersion1Columns, {}},
    TableSpec{"memory_record_tags", MemoryRecordTagColumns, {}},
    TableSpec{"memory_records", MemoryRecordVersion1Columns, {}},
    TableSpec{"memory_tags", MemoryTagColumns, {}},
    TableSpec{"project_active_sessions", ProjectActiveSessionColumns, {}},
    TableSpec{"project_aliases", ProjectAliasColumns, {}},
    TableSpec{"rollover_operations", RolloverOperationVersion2Columns, {}},
    TableSpec{"rollover_transitions", RolloverTransitionColumns, {}},
    TableSpec{"sessions", SessionColumns, {}},
};

constexpr std::array ProjectVersion1Indexes{
    IndexSpec{
        "idx_memory_project_kind", "memory_records", false, false, ProjectKindIndexColumns, {}},
    IndexSpec{
        "idx_memory_project_recent", "memory_records", false, false, ProjectRecentIndexColumns, {}},
    IndexSpec{"idx_memory_project_session",
              "memory_records",
              false,
              false,
              ProjectSessionIndexColumns,
              {}},
    IndexSpec{"idx_rollover_active_project", "rollover_operations", true, true,
              ProjectIdIndexColumns, "state <> 'predecessorSealed'"},
    IndexSpec{"idx_rollover_project_updated",
              "rollover_operations",
              false,
              false,
              ProjectUpdatedIndexColumns,
              {}},
};

constexpr std::array ProjectVersion2Tables{
    TableSpec{"artifacts", ArtifactColumns, {}},
    TableSpec{"continuity_handoffs", ContinuityHandoffColumns, {}},
    TableSpec{"event_journal", EventJournalVersion2Columns, {}},
    TableSpec{"handoffs", HandoffColumns, {}},
    TableSpec{"maintenance_state", MaintenanceStateColumns, {}},
    TableSpec{"memory_links", MemoryLinkVersion2Columns, {}},
    TableSpec{"memory_record_tags", MemoryRecordTagColumns, {}},
    TableSpec{"memory_records", MemoryRecordVersion2Columns, {}},
    TableSpec{"memory_tags", MemoryTagColumns, {}},
    TableSpec{"project_active_sessions", ProjectActiveSessionColumns, {}},
    TableSpec{"project_aliases", ProjectAliasColumns, {}},
    TableSpec{"project_metadata", ProjectMetadataColumns, {}},
    TableSpec{"rollover_operations", RolloverOperationVersion2Columns, {}},
    TableSpec{"rollover_transitions", RolloverTransitionColumns, {}},
    TableSpec{"schema_migrations", SchemaMigrationColumns, {}},
    TableSpec{"sessions", SessionColumns, {}},
};

constexpr std::array ProjectVersion2Indexes{
    IndexSpec{"idx_event_journal_event_id", "event_journal", true, true, EventIdIndexColumns,
              "event_id IS NOT NULL"},
    IndexSpec{"idx_event_journal_idempotency", "event_journal", true, true,
              ProjectIdempotencyIndexColumns, "idempotency_key IS NOT NULL"},
    IndexSpec{
        "idx_memory_project_kind", "memory_records", false, false, ProjectKindIndexColumns, {}},
    IndexSpec{
        "idx_memory_project_recent", "memory_records", false, false, ProjectRecentIndexColumns, {}},
    IndexSpec{"idx_memory_project_session",
              "memory_records",
              false,
              false,
              ProjectSessionIndexColumns,
              {}},
    IndexSpec{"idx_rollover_active_project", "rollover_operations", true, true,
              ProjectIdIndexColumns, "state <> 'predecessorSealed'"},
    IndexSpec{"idx_rollover_project_updated",
              "rollover_operations",
              false,
              false,
              ProjectUpdatedIndexColumns,
              {}},
    IndexSpec{"memory_records_project_recent",
              "memory_records",
              false,
              false,
              ProjectRecentStableIndexColumns,
              {}},
};

constexpr std::array ProjectVersion3Tables{
    TableSpec{"artifacts", ArtifactColumns, {}},
    TableSpec{"continuity_handoffs", ContinuityHandoffColumns, {}},
    TableSpec{"event_journal", EventJournalVersion2Columns, {}},
    TableSpec{"handoffs", HandoffColumns, {}},
    TableSpec{"maintenance_state", MaintenanceStateColumns, {}},
    TableSpec{"memory_links", MemoryLinkVersion2Columns, {}},
    TableSpec{"memory_record_tags", MemoryRecordTagColumns, {}},
    TableSpec{"memory_records", MemoryRecordVersion2Columns, {}},
    TableSpec{"memory_tags", MemoryTagColumns, {}},
    TableSpec{"project_active_sessions", ProjectActiveSessionColumns, {}},
    TableSpec{"project_aliases", ProjectAliasColumns, {}},
    TableSpec{"project_metadata", ProjectMetadataColumns, {}},
    TableSpec{"rollover_operations", RolloverOperationVersion3Columns, {}},
    TableSpec{"rollover_transitions", RolloverTransitionColumns, {}},
    TableSpec{"schema_migrations", SchemaMigrationColumns, {}},
    TableSpec{"sessions", SessionColumns, {}},
};

constexpr std::array ProjectVersion3Indexes{
    IndexSpec{"idx_event_journal_event_id", "event_journal", true, true, EventIdIndexColumns,
              "event_id IS NOT NULL"},
    IndexSpec{"idx_event_journal_idempotency", "event_journal", true, true,
              ProjectIdempotencyIndexColumns, "idempotency_key IS NOT NULL"},
    IndexSpec{
        "idx_memory_project_kind", "memory_records", false, false, ProjectKindIndexColumns, {}},
    IndexSpec{
        "idx_memory_project_recent", "memory_records", false, false, ProjectRecentIndexColumns, {}},
    IndexSpec{"idx_memory_project_session",
              "memory_records",
              false,
              false,
              ProjectSessionIndexColumns,
              {}},
    IndexSpec{"idx_rollover_active_project", "rollover_operations", true, true,
              ProjectIdIndexColumns,
              "state NOT IN ('predecessorSealed','completed','cancelled')"},
    IndexSpec{"idx_rollover_project_updated",
              "rollover_operations",
              false,
              false,
              ProjectUpdatedIndexColumns,
              {}},
    IndexSpec{"memory_records_project_recent",
              "memory_records",
              false,
              false,
              ProjectRecentStableIndexColumns,
              {}},
};

constexpr LayoutSpec CentralVersion3Layout{
    DatabaseKind::Central, SchemaLayout::CentralVersion3, 3, CentralPhysicalVersion, true, false,
    CentralVersion3Tables, CentralVersion3Indexes,
};

constexpr LayoutSpec CentralVersion3MinimalLayout{
    DatabaseKind::Central, SchemaLayout::CentralVersion3Minimal, 3, CentralPhysicalVersion, true,
    false, CentralVersion3MinimalTables, CentralVersion3MinimalIndexes,
};

constexpr LayoutSpec CentralVersion5Layout{
    DatabaseKind::Central, SchemaLayout::CentralVersion5, 5, CentralPhysicalVersion, true, false,
    CentralVersion5Tables, CentralVersion5Indexes,
};

constexpr LayoutSpec CentralVersion6Layout{
    DatabaseKind::Central, SchemaLayout::CentralVersion6, 6, CentralPhysicalVersion, false, true,
    CentralVersion6Tables, CentralVersion6Indexes,
};

constexpr LayoutSpec ProjectVersion1Layout{
    DatabaseKind::Project, SchemaLayout::ProjectVersion1, 1, ProjectPhysicalVersion, true, false,
    ProjectVersion1Tables, ProjectVersion1Indexes,
};

constexpr LayoutSpec ProjectVersion2Layout{
    DatabaseKind::Project, SchemaLayout::ProjectVersion2, 2, ProjectPhysicalVersion, true, true,
    ProjectVersion2Tables, ProjectVersion2Indexes,
};

constexpr LayoutSpec ProjectVersion3Layout{
    DatabaseKind::Project, SchemaLayout::ProjectVersion3, 3, ProjectPhysicalVersion, false, true,
    ProjectVersion3Tables, ProjectVersion3Indexes,
};

class ConnectionReader final
{
  public:
    ConnectionReader(Detail::WinsqliteConnection &connection,
                     const Domain::OperationContext &context) noexcept
        : connection_{connection}, context_{context}
    {
    }

    [[nodiscard]] Domain::Result<Detail::WinsqliteStatement> prepare(
        const std::string_view sql) noexcept
    {
        return connection_.prepare(sql, context_);
    }

  private:
    Detail::WinsqliteConnection &connection_;
    const Domain::OperationContext &context_;
};

class TransactionReader final
{
  public:
    explicit TransactionReader(Detail::WinsqliteTransaction &transaction) noexcept
        : transaction_{transaction}
    {
    }

    [[nodiscard]] Domain::Result<Detail::WinsqliteStatement> prepare(
        const std::string_view sql) noexcept
    {
        return transaction_.prepare(sql);
    }

  private:
    Detail::WinsqliteTransaction &transaction_;
};

template <typename T>
[[nodiscard]] Domain::Result<T> migrationFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::MigrationFailed, std::string{message}));
}

template <typename T>
[[nodiscard]] Domain::Result<T> unsupportedVersion(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::UnsupportedVersion, std::string{message}));
}

template <typename T>
[[nodiscard]] Domain::Result<T> integrityFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure, std::string{message}));
}

[[nodiscard]] bool preserveOperationalError(const std::string_view code) noexcept
{
    return code == Domain::ErrorCodes::Cancelled || code == Domain::ErrorCodes::DeadlineExceeded ||
           code == Domain::ErrorCodes::DatabaseBusy || code == Domain::ErrorCodes::StorageFull;
}

[[nodiscard]] bool isBoundedPrintableText(const std::string_view value,
                                          const std::size_t maximumBytes) noexcept
{
    if (value.empty() || value.size() > maximumBytes)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7eU;
    });
}

[[nodiscard]] bool isBoundedEvidenceId(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > MaximumBackupEvidenceIdBytes)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '.' || character == '_' ||
               character == ':' || character == '-';
    });
}

[[nodiscard]] bool validDatabaseKind(const DatabaseKind databaseKind) noexcept
{
    return databaseKind == DatabaseKind::Central || databaseKind == DatabaseKind::Project;
}

[[nodiscard]] bool validLayoutForKind(const DatabaseKind databaseKind,
                                      const SchemaLayout layout) noexcept
{
    if (layout == SchemaLayout::Empty)
    {
        return true;
    }
    if (databaseKind == DatabaseKind::Central)
    {
        return layout == SchemaLayout::CentralVersion3Minimal ||
               layout == SchemaLayout::CentralVersion3 || layout == SchemaLayout::CentralVersion5 ||
               layout == SchemaLayout::CentralVersion6;
    }
    if (databaseKind == DatabaseKind::Project)
    {
        return layout == SchemaLayout::ProjectVersion1 || layout == SchemaLayout::ProjectVersion2 ||
               layout == SchemaLayout::ProjectVersion3;
    }
    return false;
}

[[nodiscard]] SchemaAssessment assessmentFor(const LayoutSpec &spec) noexcept
{
    return {
        spec.databaseKind,         spec.layout, spec.sourceVersion, spec.targetVersion,
        spec.requiresOnlineBackup,
    };
}

[[nodiscard]] SchemaAssessment emptyAssessment(const DatabaseKind databaseKind) noexcept
{
    return {
        databaseKind,
        SchemaLayout::Empty,
        0,
        databaseKind == DatabaseKind::Central ? CentralPhysicalVersion : ProjectPhysicalVersion,
        false,
    };
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::vector<ObservedObject>> readUserObjects(Reader &reader)
{
    auto prepared = reader.prepare("SELECT type, name, tbl_name "
                                   "FROM main.sqlite_schema "
                                   "WHERE type IN ('table', 'index', 'trigger', 'view') "
                                   "AND substr(name, 1, 7) <> 'sqlite_' "
                                   "ORDER BY type, name;");
    if (!prepared)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(prepared).error());
    }

    auto statement = std::move(prepared).value();
    std::vector<ObservedObject> objects;
    objects.reserve(32U);
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<std::vector<ObservedObject>>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        if (objects.size() >= MaximumUserObjects)
        {
            return migrationFailure<std::vector<ObservedObject>>(
                "The database has too many user-defined schema objects.");
        }

        auto type = statement.columnText(0, MaximumSchemaIdentifierBytes);
        auto name = statement.columnText(1, MaximumSchemaIdentifierBytes);
        auto tableName = statement.columnText(2, MaximumSchemaIdentifierBytes);
        if (!type)
        {
            return Domain::Result<std::vector<ObservedObject>>::failure(std::move(type).error());
        }
        if (!name)
        {
            return Domain::Result<std::vector<ObservedObject>>::failure(std::move(name).error());
        }
        if (!tableName)
        {
            return Domain::Result<std::vector<ObservedObject>>::failure(
                std::move(tableName).error());
        }
        if (!type.value().has_value() || !name.value().has_value() ||
            !tableName.value().has_value())
        {
            return migrationFailure<std::vector<ObservedObject>>(
                "The database schema contains a null object identity.");
        }
        objects.push_back({std::move(type.value().value()), std::move(name.value().value()),
                           std::move(tableName.value().value())});
    }
    return Domain::Result<std::vector<ObservedObject>>::success(std::move(objects));
}

template <typename Reader> [[nodiscard]] Domain::Result<int> readUserVersion(Reader &reader)
{
    auto prepared = reader.prepare("PRAGMA main.user_version;");
    if (!prepared)
    {
        return Domain::Result<int>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<int>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Row)
    {
        return migrationFailure<int>("PRAGMA user_version did not return one row.");
    }
    auto value = statement.columnInt64(0);
    if (!value)
    {
        return Domain::Result<int>::failure(std::move(value).error());
    }
    if (value.value() < 0 || value.value() > static_cast<std::int64_t>(INT_MAX))
    {
        return unsupportedVersion<int>("The database user_version is outside the supported range.");
    }
    stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<int>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Done)
    {
        return migrationFailure<int>("PRAGMA user_version returned multiple rows.");
    }
    return Domain::Result<int>::success(static_cast<int>(value.value()));
}

[[nodiscard]] bool hasObject(const std::span<const ObservedObject> objects,
                             const std::string_view type, const std::string_view name) noexcept
{
    return std::any_of(objects.begin(), objects.end(), [&](const ObservedObject &object) {
        return object.type == type && object.name == name;
    });
}

template <typename Reader> [[nodiscard]] Domain::Result<int> readCentralVersion(Reader &reader)
{
    auto prepared =
        reader.prepare("SELECT version, typeof(version) FROM schema_version ORDER BY rowid;");
    if (!prepared)
    {
        if (preserveOperationalError(prepared.error().code))
        {
            return Domain::Result<int>::failure(std::move(prepared).error());
        }
        return migrationFailure<int>("The central schema-version ledger is unreadable.");
    }
    auto statement = std::move(prepared).value();
    auto stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<int>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Row)
    {
        return migrationFailure<int>(
            "The central schema-version ledger must contain exactly one row.");
    }
    auto value = statement.columnInt64(0);
    auto type = statement.columnText(1, MaximumColumnTypeBytes);
    if (!value)
    {
        return migrationFailure<int>("The central schema version is not an integer.");
    }
    if (!type)
    {
        return Domain::Result<int>::failure(std::move(type).error());
    }
    if (!type.value().has_value() || *type.value() != "integer")
    {
        return migrationFailure<int>("The central schema version has the wrong storage type.");
    }
    stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<int>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Done)
    {
        return migrationFailure<int>(
            "The central schema-version ledger must contain exactly one row.");
    }
    if (value.value() < 0 || value.value() > static_cast<std::int64_t>(INT_MAX))
    {
        return unsupportedVersion<int>(
            "The central schema version is outside the supported range.");
    }
    return Domain::Result<int>::success(static_cast<int>(value.value()));
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::vector<ObservedColumn>> readTableColumns(
    Reader &reader, const std::string_view tableName)
{
    auto prepared = reader.prepare("SELECT cid, name, type, \"notnull\", dflt_value, pk, hidden "
                                   "FROM pragma_table_xinfo(?1) ORDER BY cid;");
    if (!prepared)
    {
        return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, tableName);
    if (!bound)
    {
        return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(bound).error());
    }

    std::vector<ObservedColumn> columns;
    columns.reserve(16U);
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        if (columns.size() >= MaximumTableColumns)
        {
            return migrationFailure<std::vector<ObservedColumn>>(
                "A database table has too many columns.");
        }

        auto columnId = statement.columnInt64(0);
        auto name = statement.columnText(1, MaximumSchemaIdentifierBytes);
        auto type = statement.columnText(2, MaximumColumnTypeBytes);
        auto notNull = statement.columnInt64(3);
        auto defaultValue = statement.columnText(4, MaximumDefaultValueBytes);
        auto primaryKeyOrder = statement.columnInt64(5);
        auto hidden = statement.columnInt64(6);
        if (!columnId)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(
                std::move(columnId).error());
        }
        if (!name)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(name).error());
        }
        if (!type)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(type).error());
        }
        if (!notNull)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(notNull).error());
        }
        if (!defaultValue)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(
                std::move(defaultValue).error());
        }
        if (!primaryKeyOrder)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(
                std::move(primaryKeyOrder).error());
        }
        if (!hidden)
        {
            return Domain::Result<std::vector<ObservedColumn>>::failure(std::move(hidden).error());
        }
        if (!name.value().has_value() || !type.value().has_value() || columnId.value() < 0 ||
            columnId.value() > static_cast<std::int64_t>(INT_MAX) ||
            (notNull.value() != 0 && notNull.value() != 1) || primaryKeyOrder.value() < 0 ||
            primaryKeyOrder.value() > static_cast<std::int64_t>(INT_MAX) || hidden.value() < 0 ||
            hidden.value() > static_cast<std::int64_t>(INT_MAX))
        {
            return migrationFailure<std::vector<ObservedColumn>>(
                "A database table has malformed column metadata.");
        }
        columns.push_back({
            static_cast<int>(columnId.value()),
            std::move(name.value().value()),
            std::move(type.value().value()),
            notNull.value() == 1,
            std::move(defaultValue).value(),
            static_cast<int>(primaryKeyOrder.value()),
            static_cast<int>(hidden.value()),
        });
    }
    return Domain::Result<std::vector<ObservedColumn>>::success(std::move(columns));
}

[[nodiscard]] bool columnsMatch(const std::span<const ObservedColumn> observed,
                                const std::span<const ColumnSpec> expected) noexcept
{
    if (observed.size() != expected.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto &actual = observed[index];
        const auto &wanted = expected[index];
        if (actual.columnId != static_cast<int>(index) || actual.name != wanted.name ||
            actual.type != wanted.type || actual.notNull != wanted.notNull ||
            actual.primaryKeyOrder != wanted.primaryKeyOrder || actual.hidden != 0)
        {
            return false;
        }
        if (actual.defaultValue.has_value() != wanted.hasDefault)
        {
            return false;
        }
        if (wanted.hasDefault && *actual.defaultValue != wanted.defaultValue)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool equalsAsciiCaseInsensitive(const std::string_view left,
                                              const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto leftByte = static_cast<unsigned char>(left[index]);
        const auto rightByte = static_cast<unsigned char>(right[index]);
        if (std::toupper(leftByte) != std::toupper(rightByte))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool identifierCharacter(const char character) noexcept
{
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_';
}

[[nodiscard]] std::optional<std::string_view> findWherePredicate(
    const std::string_view sql) noexcept
{
    char quote{};
    for (std::size_t index = 0; index < sql.size(); ++index)
    {
        const char character = sql[index];
        if (quote != '\0')
        {
            if (quote == '[')
            {
                if (character == ']')
                {
                    quote = '\0';
                }
            }
            else if (character == quote)
            {
                if (index + 1U < sql.size() && sql[index + 1U] == quote)
                {
                    ++index;
                }
                else
                {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '\'' || character == '"' || character == '`' || character == '[')
        {
            quote = character;
            continue;
        }
        constexpr std::string_view Keyword = "WHERE";
        if (index + Keyword.size() <= sql.size() &&
            equalsAsciiCaseInsensitive(sql.substr(index, Keyword.size()), Keyword))
        {
            const bool validLeft = index == 0U || !identifierCharacter(sql[index - 1U]);
            const std::size_t after = index + Keyword.size();
            const bool validRight = after == sql.size() || !identifierCharacter(sql[after]);
            if (validLeft && validRight)
            {
                return sql.substr(after);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string canonicalSqlFragment(const std::string_view fragment)
{
    std::string canonical;
    canonical.reserve(fragment.size());
    char quote{};
    for (std::size_t index = 0; index < fragment.size(); ++index)
    {
        const char character = fragment[index];
        if (quote != '\0')
        {
            canonical.push_back(character);
            if (quote == '[')
            {
                if (character == ']')
                {
                    quote = '\0';
                }
            }
            else if (character == quote)
            {
                if (index + 1U < fragment.size() && fragment[index + 1U] == quote)
                {
                    canonical.push_back(fragment[++index]);
                }
                else
                {
                    quote = '\0';
                }
            }
            continue;
        }
        if (character == '-' && index + 1U < fragment.size() &&
            fragment[index + 1U] == '-')
        {
            index += 2U;
            while (index < fragment.size() && fragment[index] != '\r' &&
                   fragment[index] != '\n')
            {
                ++index;
            }
            if (index >= fragment.size())
            {
                break;
            }
            continue;
        }
        if (character == '/' && index + 1U < fragment.size() &&
            fragment[index + 1U] == '*')
        {
            index += 2U;
            while (index + 1U < fragment.size() &&
                   !(fragment[index] == '*' && fragment[index + 1U] == '/'))
            {
                ++index;
            }
            if (index + 1U >= fragment.size())
            {
                break;
            }
            ++index;
            continue;
        }
        if (character == '\'' || character == '"' || character == '`' || character == '[')
        {
            quote = character;
            canonical.push_back(character);
        }
        else if (std::isspace(static_cast<unsigned char>(character)) == 0 && character != ';')
        {
            canonical.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
        }
    }
    return canonical;
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::string> readIndexSql(Reader &reader,
                                                       const std::string_view indexName)
{
    auto prepared =
        reader.prepare("SELECT sql FROM main.sqlite_schema WHERE type = 'index' AND name = ?1;");
    if (!prepared)
    {
        return Domain::Result<std::string>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, indexName);
    if (!bound)
    {
        return Domain::Result<std::string>::failure(std::move(bound).error());
    }
    auto stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<std::string>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Row)
    {
        return migrationFailure<std::string>("A required database index has no SQL definition.");
    }
    auto sql = statement.columnText(0, MaximumSchemaTextBytes);
    if (!sql)
    {
        return Domain::Result<std::string>::failure(std::move(sql).error());
    }
    if (!sql.value().has_value())
    {
        return migrationFailure<std::string>("A required database index has no SQL definition.");
    }
    stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<std::string>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Done)
    {
        return migrationFailure<std::string>("A database index name is ambiguous.");
    }
    return Domain::Result<std::string>::success(std::move(sql.value().value()));
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateIndex(Reader &reader, const IndexSpec &expected)
{
    std::vector<ObservedIndexColumn> columns;
    columns.reserve(expected.columns.size());
    {
        auto propertiesPrepared = reader.prepare("SELECT \"unique\", origin, partial "
                                                 "FROM pragma_index_list(?1) WHERE name = ?2;");
        if (!propertiesPrepared)
        {
            return Domain::Result<void>::failure(std::move(propertiesPrepared).error());
        }
        auto properties = std::move(propertiesPrepared).value();
        auto bound = properties.bindText(1, expected.tableName);
        if (!bound)
        {
            return Domain::Result<void>::failure(std::move(bound).error());
        }
        bound = properties.bindText(2, expected.name);
        if (!bound)
        {
            return Domain::Result<void>::failure(std::move(bound).error());
        }
        auto stepped = properties.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Row)
        {
            return migrationFailure<void>("A required database index is missing.");
        }
        auto unique = properties.columnInt64(0);
        auto origin = properties.columnText(1, MaximumColumnTypeBytes);
        auto partial = properties.columnInt64(2);
        if (!unique || !origin || !partial)
        {
            return migrationFailure<void>("A database index has unreadable properties.");
        }
        if ((unique.value() != 0 && unique.value() != 1) ||
            (partial.value() != 0 && partial.value() != 1) || !origin.value().has_value() ||
            *origin.value() != "c" || (unique.value() == 1) != expected.unique ||
            (partial.value() == 1) != expected.partial)
        {
            return migrationFailure<void>("A database index has unexpected properties.");
        }
        stepped = properties.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Done)
        {
            return migrationFailure<void>("A database index name is ambiguous.");
        }
    }
    {
        auto columnsPrepared =
            reader.prepare("SELECT seqno, cid, name, \"desc\", coll "
                           "FROM pragma_index_xinfo(?1) WHERE \"key\" = 1 ORDER BY seqno;");
        if (!columnsPrepared)
        {
            return Domain::Result<void>::failure(std::move(columnsPrepared).error());
        }
        auto columnsStatement = std::move(columnsPrepared).value();
        auto bound = columnsStatement.bindText(1, expected.name);
        if (!bound)
        {
            return Domain::Result<void>::failure(std::move(bound).error());
        }
        for (;;)
        {
            auto stepped = columnsStatement.step();
            if (!stepped)
            {
                return Domain::Result<void>::failure(std::move(stepped).error());
            }
            if (stepped.value() == Detail::WinsqliteStepResult::Done)
            {
                break;
            }
            if (columns.size() >= MaximumIndexColumns)
            {
                return migrationFailure<void>("A database index has too many key columns.");
            }
            auto sequence = columnsStatement.columnInt64(0);
            auto columnId = columnsStatement.columnInt64(1);
            auto name = columnsStatement.columnText(2, MaximumSchemaIdentifierBytes);
            auto descending = columnsStatement.columnInt64(3);
            auto collation = columnsStatement.columnText(4, MaximumColumnTypeBytes);
            if (!sequence || !columnId || !name || !descending || !collation)
            {
                return migrationFailure<void>("A database index has unreadable key metadata.");
            }
            if (sequence.value() < 0 || sequence.value() > static_cast<std::int64_t>(INT_MAX) ||
                columnId.value() < 0 || columnId.value() > static_cast<std::int64_t>(INT_MAX) ||
                !name.value().has_value() || (descending.value() != 0 && descending.value() != 1) ||
                !collation.value().has_value())
            {
                return migrationFailure<void>("A database index has malformed key metadata.");
            }
            columns.push_back({
                static_cast<int>(sequence.value()),
                static_cast<int>(columnId.value()),
                std::move(name.value().value()),
                descending.value() == 1,
                std::move(collation.value().value()),
            });
        }
    }
    if (columns.size() != expected.columns.size())
    {
        return migrationFailure<void>("A database index has the wrong key-column count.");
    }
    for (std::size_t index = 0; index < expected.columns.size(); ++index)
    {
        if (columns[index].sequence != static_cast<int>(index) || columns[index].columnId < 0 ||
            columns[index].name != expected.columns[index].name ||
            columns[index].descending != expected.columns[index].descending ||
            columns[index].collation != "BINARY")
        {
            return migrationFailure<void>("A database index has the wrong key-column layout.");
        }
    }

    auto sql = readIndexSql(reader, expected.name);
    if (!sql)
    {
        return Domain::Result<void>::failure(std::move(sql).error());
    }
    const auto predicate = findWherePredicate(sql.value());
    if (predicate.has_value() != expected.partial)
    {
        return migrationFailure<void>("A database index has the wrong partial-index predicate.");
    }
    if (expected.partial &&
        canonicalSqlFragment(*predicate) != canonicalSqlFragment(expected.partialPredicate))
    {
        return migrationFailure<void>("A database index has the wrong partial-index predicate.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool isFtsBundleObjectName(const std::string_view name) noexcept
{
    constexpr std::array<std::string_view, 9> Names{
        "memory_fts_delete",       "memory_fts_insert",          "memory_fts_update",
        "memory_records_fts",      "memory_records_fts_config",  "memory_records_fts_content",
        "memory_records_fts_data", "memory_records_fts_docsize", "memory_records_fts_idx",
    };
    return std::find(Names.begin(), Names.end(), name) != Names.end();
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::string> readSchemaObjectSql(Reader &reader,
                                                              const std::string_view type,
                                                              const std::string_view name)
{
    auto prepared =
        reader.prepare("SELECT sql FROM main.sqlite_schema WHERE type = ?1 AND name = ?2;");
    if (!prepared)
    {
        return Domain::Result<std::string>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, type);
    if (!bound)
    {
        return Domain::Result<std::string>::failure(std::move(bound).error());
    }
    bound = statement.bindText(2, name);
    if (!bound)
    {
        return Domain::Result<std::string>::failure(std::move(bound).error());
    }
    auto stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<std::string>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Row)
    {
        return migrationFailure<std::string>("A required schema object is missing.");
    }
    auto sql = statement.columnText(0, MaximumSchemaTextBytes);
    if (!sql)
    {
        return Domain::Result<std::string>::failure(std::move(sql).error());
    }
    if (!sql.value().has_value())
    {
        return migrationFailure<std::string>("A required schema object has no SQL definition.");
    }
    stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<std::string>::failure(std::move(stepped).error());
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Done)
    {
        return migrationFailure<std::string>("A schema object name is ambiguous.");
    }
    return Domain::Result<std::string>::success(std::move(sql.value().value()));
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateCanonicalSchemaSql(
    Reader &reader, const std::string_view type, const std::string_view name,
    const std::string_view expectedWithIfNotExists,
    const std::string_view expectedWithoutIfNotExists)
{
    auto observed = readSchemaObjectSql(reader, type, name);
    if (!observed)
    {
        return Domain::Result<void>::failure(std::move(observed).error());
    }
    const auto canonical = canonicalSqlFragment(observed.value());
    if (canonical != canonicalSqlFragment(expectedWithIfNotExists) &&
        canonical != canonicalSqlFragment(expectedWithoutIfNotExists))
    {
        return migrationFailure<void>(
            "An optional project FTS schema object has an unexpected definition.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateFtsVirtualColumns(Reader &reader)
{
    auto prepared = reader.prepare("SELECT name, hidden FROM pragma_table_xinfo(?1) ORDER BY cid;");
    if (!prepared)
    {
        return Domain::Result<void>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, "memory_records_fts");
    if (!bound)
    {
        return Domain::Result<void>::failure(std::move(bound).error());
    }
    constexpr std::array<std::pair<std::string_view, int>, 6> Expected{
        std::pair<std::string_view, int>{"id", 0},
        std::pair<std::string_view, int>{"title", 0},
        std::pair<std::string_view, int>{"summary", 0},
        std::pair<std::string_view, int>{"body", 0},
        std::pair<std::string_view, int>{"memory_records_fts", 1},
        std::pair<std::string_view, int>{"rank", 1},
    };
    std::size_t index{};
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        if (index >= Expected.size())
        {
            return migrationFailure<void>("The optional project FTS table has unexpected columns.");
        }
        auto name = statement.columnText(0, MaximumSchemaIdentifierBytes);
        auto hidden = statement.columnInt64(1);
        if (!name || !hidden || !name.value().has_value() ||
            *name.value() != Expected[index].first || hidden.value() != Expected[index].second)
        {
            return migrationFailure<void>("The optional project FTS table has unexpected columns.");
        }
        ++index;
    }
    if (index != Expected.size())
    {
        return migrationFailure<void>("The optional project FTS table has unexpected columns.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::vector<ObservedObject>> removeVerifiedProjectFtsBundle(
    Reader &reader, const std::span<const ObservedObject> objects)
{
    const auto ftsObjectCount = static_cast<std::size_t>(
        std::count_if(objects.begin(), objects.end(),
                      [](const auto &object) { return isFtsBundleObjectName(object.name); }));
    if (ftsObjectCount == 0U)
    {
        return Domain::Result<std::vector<ObservedObject>>::success(
            std::vector<ObservedObject>{objects.begin(), objects.end()});
    }
    if (ftsObjectCount != 9U)
    {
        return migrationFailure<std::vector<ObservedObject>>(
            "The optional project FTS schema bundle is incomplete.");
    }

    constexpr std::array<std::string_view, 6> FtsTables{
        "memory_records_fts",      "memory_records_fts_config",  "memory_records_fts_content",
        "memory_records_fts_data", "memory_records_fts_docsize", "memory_records_fts_idx",
    };
    constexpr std::array<std::string_view, 3> FtsTriggers{
        "memory_fts_delete",
        "memory_fts_insert",
        "memory_fts_update",
    };
    for (const auto tableName : FtsTables)
    {
        const bool found = std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
            return object.type == "table" && object.name == tableName &&
                   object.tableName == tableName;
        });
        if (!found)
        {
            return migrationFailure<std::vector<ObservedObject>>(
                "The optional project FTS table bundle is malformed.");
        }
    }
    for (const auto triggerName : FtsTriggers)
    {
        const bool found = std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
            return object.type == "trigger" && object.name == triggerName &&
                   object.tableName == "memory_records";
        });
        if (!found)
        {
            return migrationFailure<std::vector<ObservedObject>>(
                "The optional project FTS trigger bundle is malformed.");
        }
    }

    auto validated =
        validateCanonicalSchemaSql(reader, "table", "memory_records_fts",
                                   "CREATE VIRTUAL TABLE IF NOT EXISTS memory_records_fts "
                                   "USING fts5(id UNINDEXED, title, summary, body)",
                                   "CREATE VIRTUAL TABLE memory_records_fts "
                                   "USING fts5(id UNINDEXED, title, summary, body)");
    if (!validated)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(validated).error());
    }
    validated = validateFtsVirtualColumns(reader);
    if (!validated)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(validated).error());
    }
    validated = validateCanonicalSchemaSql(
        reader, "trigger", "memory_fts_insert",
        "CREATE TRIGGER IF NOT EXISTS memory_fts_insert "
        "AFTER INSERT ON memory_records WHEN new.is_tombstone = 0 BEGIN "
        "INSERT INTO memory_records_fts(id, title, summary, body) "
        "VALUES(new.id, new.title, new.summary, COALESCE(new.body, '')); END",
        "CREATE TRIGGER memory_fts_insert "
        "AFTER INSERT ON memory_records WHEN new.is_tombstone = 0 BEGIN "
        "INSERT INTO memory_records_fts(id, title, summary, body) "
        "VALUES(new.id, new.title, new.summary, COALESCE(new.body, '')); END");
    if (!validated)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(validated).error());
    }
    validated =
        validateCanonicalSchemaSql(reader, "trigger", "memory_fts_update",
                                   "CREATE TRIGGER IF NOT EXISTS memory_fts_update "
                                   "AFTER UPDATE ON memory_records BEGIN "
                                   "DELETE FROM memory_records_fts WHERE id = old.id; "
                                   "INSERT INTO memory_records_fts(id, title, summary, body) "
                                   "SELECT new.id, new.title, new.summary, COALESCE(new.body, '') "
                                   "WHERE new.is_tombstone = 0; END",
                                   "CREATE TRIGGER memory_fts_update "
                                   "AFTER UPDATE ON memory_records BEGIN "
                                   "DELETE FROM memory_records_fts WHERE id = old.id; "
                                   "INSERT INTO memory_records_fts(id, title, summary, body) "
                                   "SELECT new.id, new.title, new.summary, COALESCE(new.body, '') "
                                   "WHERE new.is_tombstone = 0; END");
    if (!validated)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(validated).error());
    }
    validated = validateCanonicalSchemaSql(reader, "trigger", "memory_fts_delete",
                                           "CREATE TRIGGER IF NOT EXISTS memory_fts_delete "
                                           "AFTER DELETE ON memory_records BEGIN "
                                           "DELETE FROM memory_records_fts WHERE id = old.id; END",
                                           "CREATE TRIGGER memory_fts_delete "
                                           "AFTER DELETE ON memory_records BEGIN "
                                           "DELETE FROM memory_records_fts WHERE id = old.id; END");
    if (!validated)
    {
        return Domain::Result<std::vector<ObservedObject>>::failure(std::move(validated).error());
    }

    std::vector<ObservedObject> baseObjects;
    baseObjects.reserve(objects.size() - ftsObjectCount);
    for (const auto &object : objects)
    {
        if (!isFtsBundleObjectName(object.name))
        {
            baseObjects.push_back(object);
        }
    }
    return Domain::Result<std::vector<ObservedObject>>::success(std::move(baseObjects));
}

[[nodiscard]] std::span<const UniqueConstraintSpec> expectedUniqueConstraints(
    const std::string_view tableName) noexcept
{
    if (tableName == "schema_migrations")
    {
        return SchemaMigrationUniqueConstraints;
    }
    if (tableName == "memory_records")
    {
        return MemoryRecordUniqueConstraints;
    }
    if (tableName == "memory_tags")
    {
        return MemoryTagUniqueConstraints;
    }
    if (tableName == "continuity_handoffs")
    {
        return ContinuityHandoffUniqueConstraints;
    }
    if (tableName == "rollover_operations")
    {
        return RolloverOperationUniqueConstraints;
    }
    return {};
}

[[nodiscard]] std::span<const ForeignKeySpec> expectedForeignKeys(
    const std::string_view tableName) noexcept
{
    if (tableName == "memory_record_tags")
    {
        return MemoryRecordTagForeignKeys;
    }
    if (tableName == "memory_links")
    {
        return MemoryLinkForeignKeys;
    }
    return {};
}

[[nodiscard]] bool expectsAutoincrement(const std::string_view tableName) noexcept
{
    return tableName == "audit_events" || tableName == "memory_tags" ||
           tableName == "event_journal" || tableName == "rollover_transitions";
}

[[nodiscard]] std::size_t countCanonicalToken(const std::string_view text,
                                              const std::string_view token) noexcept
{
    std::size_t count{};
    std::size_t offset{};
    while (offset < text.size())
    {
        const auto found = text.find(token, offset);
        if (found == std::string_view::npos)
        {
            break;
        }
        ++count;
        offset = found + token.size();
    }
    return count;
}

template <typename Reader>
[[nodiscard]] Domain::Result<std::vector<std::string>> readConstraintIndexColumns(
    Reader &reader, const std::string_view indexName)
{
    auto prepared = reader.prepare("SELECT seqno, cid, name, \"desc\", coll "
                                   "FROM pragma_index_xinfo(?1) WHERE \"key\" = 1 ORDER BY seqno;");
    if (!prepared)
    {
        return Domain::Result<std::vector<std::string>>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, indexName);
    if (!bound)
    {
        return Domain::Result<std::vector<std::string>>::failure(std::move(bound).error());
    }
    std::vector<std::string> columns;
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<std::vector<std::string>>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        if (columns.size() >= MaximumIndexColumns)
        {
            return migrationFailure<std::vector<std::string>>(
                "A constraint index has too many key columns.");
        }
        auto sequence = statement.columnInt64(0);
        auto columnId = statement.columnInt64(1);
        auto name = statement.columnText(2, MaximumSchemaIdentifierBytes);
        auto descending = statement.columnInt64(3);
        auto collation = statement.columnText(4, MaximumColumnTypeBytes);
        if (!sequence || !columnId || !name || !descending || !collation)
        {
            return migrationFailure<std::vector<std::string>>(
                "A constraint index has unreadable key metadata.");
        }
        if (sequence.value() != static_cast<std::int64_t>(columns.size()) || columnId.value() < 0 ||
            !name.value().has_value() || descending.value() != 0 ||
            !collation.value().has_value() || *collation.value() != "BINARY")
        {
            return migrationFailure<std::vector<std::string>>(
                "A constraint index has unexpected key metadata.");
        }
        columns.push_back(std::move(name.value().value()));
    }
    return Domain::Result<std::vector<std::string>>::success(std::move(columns));
}

[[nodiscard]] bool stringColumnsEqual(const std::span<const std::string> actual,
                                      const std::span<const std::string_view> expected) noexcept
{
    if (actual.size() != expected.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (actual[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateTableConstraintIndexes(Reader &reader,
                                                                  const TableSpec &table,
                                                                  const LayoutSpec &layout)
{
    std::vector<ObservedConstraintIndex> indexes;
    {
        auto prepared = reader.prepare("SELECT name, \"unique\", origin, partial "
                                       "FROM pragma_index_list(?1) ORDER BY seq;");
        if (!prepared)
        {
            return Domain::Result<void>::failure(std::move(prepared).error());
        }
        auto statement = std::move(prepared).value();
        auto bound = statement.bindText(1, table.name);
        if (!bound)
        {
            return Domain::Result<void>::failure(std::move(bound).error());
        }
        for (;;)
        {
            auto stepped = statement.step();
            if (!stepped)
            {
                return Domain::Result<void>::failure(std::move(stepped).error());
            }
            if (stepped.value() == Detail::WinsqliteStepResult::Done)
            {
                break;
            }
            if (indexes.size() >= MaximumIndexColumns)
            {
                return migrationFailure<void>("A database table has too many indexes.");
            }
            auto name = statement.columnText(0, MaximumSchemaIdentifierBytes);
            auto unique = statement.columnInt64(1);
            auto origin = statement.columnText(2, MaximumColumnTypeBytes);
            auto partial = statement.columnInt64(3);
            if (!name || !unique || !origin || !partial || !name.value().has_value() ||
                !origin.value().has_value() || (unique.value() != 0 && unique.value() != 1) ||
                (partial.value() != 0 && partial.value() != 1))
            {
                return migrationFailure<void>("A database table has malformed index metadata.");
            }
            indexes.push_back({
                std::move(name.value().value()),
                std::move(origin.value().value()),
                unique.value() == 1,
                partial.value() == 1,
                {},
            });
        }
    }
    for (auto &observed : indexes)
    {
        if (observed.origin == "pk" || observed.origin == "u")
        {
            auto columns = readConstraintIndexColumns(reader, observed.name);
            if (!columns)
            {
                return Domain::Result<void>::failure(std::move(columns).error());
            }
            observed.columns = std::move(columns).value();
        }
    }

    std::size_t expectedCreatedIndexCount{};
    for (const auto &index : layout.indexes)
    {
        if (index.tableName == table.name)
        {
            ++expectedCreatedIndexCount;
            const bool found = std::any_of(indexes.begin(), indexes.end(), [&](const auto &item) {
                return item.origin == "c" && item.name == index.name;
            });
            if (!found)
            {
                return migrationFailure<void>(
                    "A table is missing a required explicitly-created index.");
            }
        }
    }
    if (static_cast<std::size_t>(std::count_if(
            indexes.begin(), indexes.end(), [](const auto &item) { return item.origin == "c"; })) !=
        expectedCreatedIndexCount)
    {
        return migrationFailure<void>("A table has an unexpected explicitly-created index.");
    }

    std::vector<std::string_view> primaryKeyColumns;
    for (int order = 1;; ++order)
    {
        const auto found =
            std::find_if(table.columns.begin(), table.columns.end(),
                         [&](const auto &item) { return item.primaryKeyOrder == order; });
        if (found == table.columns.end())
        {
            break;
        }
        primaryKeyColumns.push_back(found->name);
    }
    const bool rowIdPrimaryKey =
        primaryKeyColumns.size() == 1U &&
        std::any_of(table.columns.begin(), table.columns.end(), [&](const auto &item) {
            return item.name == primaryKeyColumns.front() && item.type == "INTEGER";
        });
    const std::size_t expectedPrimaryKeyIndexes =
        primaryKeyColumns.empty() || rowIdPrimaryKey ? 0U : 1U;
    const auto observedPrimaryKeyCount = static_cast<std::size_t>(std::count_if(
        indexes.begin(), indexes.end(), [](const auto &item) { return item.origin == "pk"; }));
    if (observedPrimaryKeyCount != expectedPrimaryKeyIndexes)
    {
        return migrationFailure<void>("A table has the wrong primary-key index structure.");
    }
    if (expectedPrimaryKeyIndexes == 1U)
    {
        const auto primary = std::find_if(indexes.begin(), indexes.end(),
                                          [](const auto &item) { return item.origin == "pk"; });
        if (primary == indexes.end() || !primary->unique || primary->partial ||
            !stringColumnsEqual(primary->columns, primaryKeyColumns))
        {
            return migrationFailure<void>("A table has the wrong primary-key index columns.");
        }
    }

    const auto expectedUnique = expectedUniqueConstraints(table.name);
    std::vector<bool> matchedUnique(expectedUnique.size(), false);
    std::size_t observedUniqueCount{};
    for (const auto &index : indexes)
    {
        if (index.origin != "u")
        {
            if (index.origin != "c" && index.origin != "pk")
            {
                return migrationFailure<void>("A table has an unknown index origin.");
            }
            continue;
        }
        ++observedUniqueCount;
        if (!index.unique || index.partial)
        {
            return migrationFailure<void>("A table has malformed UNIQUE-constraint metadata.");
        }
        bool matched{};
        for (std::size_t expectedIndex = 0; expectedIndex < expectedUnique.size(); ++expectedIndex)
        {
            if (!matchedUnique[expectedIndex] &&
                stringColumnsEqual(index.columns, expectedUnique[expectedIndex].columns))
            {
                matchedUnique[expectedIndex] = true;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            return migrationFailure<void>("A table has an unexpected UNIQUE constraint.");
        }
    }
    if (observedUniqueCount != expectedUnique.size() ||
        std::any_of(matchedUnique.begin(), matchedUnique.end(),
                    [](const bool item) { return !item; }))
    {
        return migrationFailure<void>("A table is missing a required UNIQUE constraint.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateTableForeignKeys(Reader &reader, const TableSpec &table)
{
    auto prepared =
        reader.prepare("SELECT id, seq, \"table\", \"from\", \"to\", on_update, on_delete, match "
                       "FROM pragma_foreign_key_list(?1) ORDER BY id, seq;");
    if (!prepared)
    {
        return Domain::Result<void>::failure(std::move(prepared).error());
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindText(1, table.name);
    if (!bound)
    {
        return Domain::Result<void>::failure(std::move(bound).error());
    }
    const auto expected = expectedForeignKeys(table.name);
    std::vector<bool> matched(expected.size(), false);
    std::size_t observedCount{};
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        if (observedCount >= 32U)
        {
            return migrationFailure<void>("A table has too many foreign keys.");
        }
        auto id = statement.columnInt64(0);
        auto sequence = statement.columnInt64(1);
        auto referencedTable = statement.columnText(2, MaximumSchemaIdentifierBytes);
        auto fromColumn = statement.columnText(3, MaximumSchemaIdentifierBytes);
        auto toColumn = statement.columnText(4, MaximumSchemaIdentifierBytes);
        auto onUpdate = statement.columnText(5, MaximumColumnTypeBytes);
        auto onDelete = statement.columnText(6, MaximumColumnTypeBytes);
        auto match = statement.columnText(7, MaximumColumnTypeBytes);
        if (!id || !sequence || !referencedTable || !fromColumn || !toColumn || !onUpdate ||
            !onDelete || !match || id.value() < 0 || sequence.value() != 0 ||
            !referencedTable.value().has_value() || !fromColumn.value().has_value() ||
            !toColumn.value().has_value() || !onUpdate.value().has_value() ||
            !onDelete.value().has_value() || !match.value().has_value())
        {
            return migrationFailure<void>("A table has malformed foreign-key metadata.");
        }
        bool found{};
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            if (!matched[index] && *referencedTable.value() == expected[index].referencedTable &&
                *fromColumn.value() == expected[index].fromColumn &&
                *toColumn.value() == expected[index].toColumn &&
                *onUpdate.value() == expected[index].onUpdate &&
                *onDelete.value() == expected[index].onDelete &&
                *match.value() == expected[index].match)
            {
                matched[index] = true;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return migrationFailure<void>("A table has an unexpected foreign-key constraint.");
        }
        ++observedCount;
    }
    if (observedCount != expected.size() ||
        std::any_of(matched.begin(), matched.end(), [](const bool item) { return !item; }))
    {
        return migrationFailure<void>("A table is missing a required foreign-key constraint.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateTableSqlConstraints(Reader &reader,
                                                               const TableSpec &table,
                                                               const LayoutSpec &layout)
{
    auto sql = readSchemaObjectSql(reader, "table", table.name);
    if (!sql)
    {
        return Domain::Result<void>::failure(std::move(sql).error());
    }
    const auto canonical = canonicalSqlFragment(sql.value());
    if (canonical.find("WITHOUTROWID") != std::string::npos ||
        canonical.find("STRICT") != std::string::npos ||
        canonical.find("ONCONFLICT") != std::string::npos ||
        canonical.find("COLLATE") != std::string::npos ||
        canonical.find("DEFERRABLE") != std::string::npos ||
        canonical.find("GENERATED") != std::string::npos)
    {
        return migrationFailure<void>("A database table has unsupported storage modifiers.");
    }
    const auto autoincrementCount = countCanonicalToken(canonical, "AUTOINCREMENT");
    if (autoincrementCount != (expectsAutoincrement(table.name) ? 1U : 0U))
    {
        return migrationFailure<void>("A database table has the wrong AUTOINCREMENT contract.");
    }
    const auto checkCount = countCanonicalToken(canonical, "CHECK(");
    const bool expectsMutatingCheck =
        layout.layout == SchemaLayout::CentralVersion6 && table.name == "audit_events";
    if (checkCount != (expectsMutatingCheck ? 1U : 0U) ||
        (expectsMutatingCheck && canonical.find("CHECK(MUTATINGIN(0,1))") == std::string::npos))
    {
        return migrationFailure<void>("A database table has the wrong CHECK constraints.");
    }
    auto indexes = validateTableConstraintIndexes(reader, table, layout);
    if (!indexes)
    {
        return indexes;
    }
    return validateTableForeignKeys(reader, table);
}

[[nodiscard]] bool objectSetMatches(const std::span<const ObservedObject> objects,
                                    const LayoutSpec &expected) noexcept
{
    if (objects.size() != expected.tables.size() + expected.indexes.size())
    {
        return false;
    }
    for (const auto &table : expected.tables)
    {
        const bool found = std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
            return object.type == "table" && object.name == table.name &&
                   object.tableName == table.name;
        });
        if (!found)
        {
            return false;
        }
    }
    for (const auto &index : expected.indexes)
    {
        const bool found = std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
            return object.type == "index" && object.name == index.name &&
                   object.tableName == index.tableName;
        });
        if (!found)
        {
            return false;
        }
    }
    return true;
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateLayout(Reader &reader, const LayoutSpec &expected,
                                                  const std::span<const ObservedObject> objects)
{
    if (!objectSetMatches(objects, expected))
    {
        return migrationFailure<void>(
            "The database user-object set does not exactly match a supported schema.");
    }
    for (const auto &table : expected.tables)
    {
        auto observed = readTableColumns(reader, table.name);
        if (!observed)
        {
            return Domain::Result<void>::failure(std::move(observed).error());
        }
        const bool primaryMatches = columnsMatch(observed.value(), table.columns);
        const bool alternateMatches = !table.alternateColumns.empty() &&
                                      columnsMatch(observed.value(), table.alternateColumns);
        if (!primaryMatches && !alternateMatches)
        {
            return migrationFailure<void>(
                "A database table does not exactly match its supported column layout.");
        }
        auto constraints = validateTableSqlConstraints(reader, table, expected);
        if (!constraints)
        {
            return constraints;
        }
    }
    for (const auto &index : expected.indexes)
    {
        auto validated = validateIndex(reader, index);
        if (!validated)
        {
            return validated;
        }
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<void> validateMigrationLedger(
    Reader &reader, const std::span<const MigrationStep> steps)
{
    auto prepared = reader.prepare(
        "SELECT version, identifier, applied_at, content_sha256, "
        "typeof(version), typeof(identifier), typeof(applied_at), typeof(content_sha256) "
        "FROM schema_migrations ORDER BY version;");
    if (!prepared)
    {
        return migrationFailure<void>("The migration ledger is unreadable.");
    }
    auto statement = std::move(prepared).value();
    std::size_t rowIndex{};
    for (;;)
    {
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(std::move(stepped).error());
        }
        if (stepped.value() == Detail::WinsqliteStepResult::Done)
        {
            break;
        }
        auto version = statement.columnInt64(0);
        auto identifier = statement.columnText(1, MaximumSchemaIdentifierBytes);
        auto appliedAt = statement.columnText(2, MaximumAppliedAtBytes);
        auto checksum = statement.columnText(3, 64U);
        auto versionType = statement.columnText(4, MaximumColumnTypeBytes);
        auto identifierType = statement.columnText(5, MaximumColumnTypeBytes);
        auto appliedAtType = statement.columnText(6, MaximumColumnTypeBytes);
        auto checksumType = statement.columnText(7, MaximumColumnTypeBytes);
        if (!version || !identifier || !appliedAt || !checksum || !versionType || !identifierType ||
            !appliedAtType || !checksumType)
        {
            return migrationFailure<void>("The migration ledger contains an unreadable row.");
        }
        if (version.value() > static_cast<std::int64_t>(steps.size()))
        {
            return unsupportedVersion<void>(
                "The migration ledger contains a future schema version.");
        }
        if (version.value() <= 0 || rowIndex >= steps.size() ||
            version.value() != static_cast<std::int64_t>(rowIndex + 1U) ||
            !identifier.value().has_value() || !appliedAt.value().has_value() ||
            !checksum.value().has_value() || !versionType.value().has_value() ||
            !identifierType.value().has_value() || !appliedAtType.value().has_value() ||
            !checksumType.value().has_value() || *versionType.value() != "integer" ||
            *identifierType.value() != "text" || *appliedAtType.value() != "text" ||
            *checksumType.value() != "text" ||
            !isBoundedPrintableText(*appliedAt.value(), MaximumAppliedAtBytes))
        {
            return migrationFailure<void>(
                "The migration ledger is not a contiguous, well-formed sequence.");
        }
        const auto &expected = steps[rowIndex];
        if (*identifier.value() != expected.identifier ||
            *checksum.value() != expected.contentSha256)
        {
            return migrationFailure<void>(
                "The migration ledger identifier or checksum does not match its immutable step.");
        }
        ++rowIndex;
    }
    if (rowIndex != steps.size())
    {
        return migrationFailure<void>(
            "The migration ledger does not contain every required contiguous step.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader>
[[nodiscard]] Domain::Result<SchemaAssessment> assessImpl(Reader &reader,
                                                          const DatabaseKind databaseKind)
{
    if (!validDatabaseKind(databaseKind))
    {
        return migrationFailure<SchemaAssessment>("The requested database kind is invalid.");
    }

    auto objectsResult = readUserObjects(reader);
    if (!objectsResult)
    {
        return Domain::Result<SchemaAssessment>::failure(std::move(objectsResult).error());
    }
    auto objects = std::move(objectsResult).value();

    auto userVersion = readUserVersion(reader);
    if (!userVersion)
    {
        return Domain::Result<SchemaAssessment>::failure(std::move(userVersion).error());
    }

    if (objects.empty())
    {
        if (userVersion.value() != 0)
        {
            if (databaseKind == DatabaseKind::Project &&
                userVersion.value() > ProjectPhysicalVersion)
            {
                return unsupportedVersion<SchemaAssessment>(
                    "The empty project database advertises a future user_version.");
            }
            return migrationFailure<SchemaAssessment>(
                "An empty database has a nonzero schema version marker.");
        }
        return Domain::Result<SchemaAssessment>::success(emptyAssessment(databaseKind));
    }

    if (databaseKind == DatabaseKind::Central)
    {
        if (userVersion.value() != 0)
        {
            return migrationFailure<SchemaAssessment>(
                "A central database unexpectedly uses PRAGMA user_version.");
        }
        if (!hasObject(objects, "table", "schema_version"))
        {
            return migrationFailure<SchemaAssessment>(
                "The central database has no supported schema-version evidence.");
        }
        auto sourceVersion = readCentralVersion(reader);
        if (!sourceVersion)
        {
            return Domain::Result<SchemaAssessment>::failure(std::move(sourceVersion).error());
        }
        if (sourceVersion.value() > CentralPhysicalVersion)
        {
            return unsupportedVersion<SchemaAssessment>(
                "The central database schema is newer than this application supports.");
        }
        if (sourceVersion.value() == 1 || sourceVersion.value() == 2 || sourceVersion.value() == 4)
        {
            return unsupportedVersion<SchemaAssessment>(
                "The central database uses a released version without sufficient shape evidence.");
        }

        const LayoutSpec *layout{};
        if (sourceVersion.value() == 3)
        {
            if (objectSetMatches(objects, CentralVersion3MinimalLayout))
            {
                layout = &CentralVersion3MinimalLayout;
            }
            else
            {
                layout = &CentralVersion3Layout;
            }
        }
        else if (sourceVersion.value() == 5)
        {
            layout = &CentralVersion5Layout;
        }
        else if (sourceVersion.value() == CentralPhysicalVersion)
        {
            layout = &CentralVersion6Layout;
        }
        else
        {
            return migrationFailure<SchemaAssessment>(
                "The central database has an invalid schema version marker.");
        }

        auto validated = validateLayout(reader, *layout, objects);
        if (!validated)
        {
            return Domain::Result<SchemaAssessment>::failure(std::move(validated).error());
        }
        if (layout->requiresLedger)
        {
            validated = validateMigrationLedger(reader, centralMigrationSteps());
            if (!validated)
            {
                return Domain::Result<SchemaAssessment>::failure(std::move(validated).error());
            }
        }
        return Domain::Result<SchemaAssessment>::success(assessmentFor(*layout));
    }

    if (userVersion.value() > ProjectPhysicalVersion)
    {
        return unsupportedVersion<SchemaAssessment>(
            "The project database schema is newer than this application supports.");
    }
    if (userVersion.value() != ProjectSourceVersion && userVersion.value() != 2 &&
        userVersion.value() != ProjectPhysicalVersion)
    {
        return migrationFailure<SchemaAssessment>(
            "The project database has an invalid user_version marker.");
    }
    auto baseObjects = removeVerifiedProjectFtsBundle(reader, objects);
    if (!baseObjects)
    {
        return Domain::Result<SchemaAssessment>::failure(std::move(baseObjects).error());
    }
    const LayoutSpec *layout{};
    if (userVersion.value() == ProjectSourceVersion)
    {
        layout = &ProjectVersion1Layout;
    }
    else if (userVersion.value() == 2)
    {
        layout = &ProjectVersion2Layout;
    }
    else
    {
        layout = &ProjectVersion3Layout;
    }
    auto validated = validateLayout(reader, *layout, baseObjects.value());
    if (!validated)
    {
        return Domain::Result<SchemaAssessment>::failure(std::move(validated).error());
    }
    if (layout->requiresLedger)
    {
        validated = validateMigrationLedger(
            reader, projectMigrationSteps().first(static_cast<std::size_t>(layout->sourceVersion)));
        if (!validated)
        {
            return Domain::Result<SchemaAssessment>::failure(std::move(validated).error());
        }
    }
    return Domain::Result<SchemaAssessment>::success(assessmentFor(*layout));
}

[[nodiscard]] Domain::Error asMigrationError(Domain::Error error)
{
    if (preserveOperationalError(error.code) ||
        error.code == Domain::ErrorCodes::UnsupportedVersion ||
        error.code == Domain::ErrorCodes::MigrationFailed ||
        error.code == Domain::ErrorCodes::IntegrityFailure)
    {
        return error;
    }
    return Domain::makeError(Domain::ErrorCodes::MigrationFailed,
                             "A database migration operation failed.", error.retryable,
                             std::move(error.evidenceId));
}

template <typename T>
[[nodiscard]] Domain::Result<T> rollbackFailure(Detail::WinsqliteTransaction &transaction,
                                                Domain::Error error)
{
    Domain::Error primary = asMigrationError(std::move(error));
    auto rolledBack = transaction.rollback();
    if (!rolledBack)
    {
        try
        {
            primary.message += " Transaction rollback also failed: ";
            primary.message += rolledBack.error().message;
            if (!primary.evidenceId.has_value() &&
                rolledBack.error().evidenceId.has_value())
            {
                primary.evidenceId = rolledBack.error().evidenceId;
            }
        }
        catch (...)
        {
            // Preserve the primary typed migration/integrity failure under
            // memory pressure even if rollback diagnostics cannot be appended.
        }
    }
    return Domain::Result<T>::failure(std::move(primary));
}

[[nodiscard]] Domain::Result<void> executeRequiredMigrationSteps(
    Detail::WinsqliteTransaction &transaction, const SchemaAssessment &assessment)
{
    const auto steps = assessment.databaseKind == DatabaseKind::Central ? centralMigrationSteps()
                                                                        : projectMigrationSteps();
    if (assessment.layout == SchemaLayout::Empty)
    {
        for (const auto &step : steps)
        {
            auto executed = transaction.execute(step.sql);
            if (!executed)
            {
                return Domain::Result<void>::failure(asMigrationError(std::move(executed).error()));
            }
        }
        return Domain::Result<void>::success();
    }

    if (assessment.sourceVersion >= assessment.targetVersion)
    {
        return Domain::Result<void>::success();
    }

    if (assessment.databaseKind == DatabaseKind::Central)
    {
        auto ledgerCreated = transaction.execute(steps.front().sql);
        if (!ledgerCreated)
        {
            return Domain::Result<void>::failure(
                asMigrationError(std::move(ledgerCreated).error()));
        }
        if (assessment.layout == SchemaLayout::CentralVersion3Minimal)
        {
            constexpr std::string_view MissingVersion3CoreSql = R"sql(
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
);
CREATE INDEX idx_context_handoffs_updated
    ON context_handoffs(updated_at DESC);
)sql";
            auto bootstrapped = transaction.execute(MissingVersion3CoreSql);
            if (!bootstrapped)
            {
                return Domain::Result<void>::failure(
                    asMigrationError(std::move(bootstrapped).error()));
            }
        }
    }
    for (const auto &step : steps)
    {
        if (step.version <= assessment.sourceVersion)
        {
            continue;
        }
        auto executed = transaction.execute(step.sql);
        if (!executed)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(executed).error()));
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> recordMigrationLedger(Detail::WinsqliteTransaction &transaction,
                                                         const SchemaAssessment &assessment,
                                                         const std::string_view appliedAt)
{
    if (assessment.sourceVersion >= assessment.targetVersion)
    {
        return Domain::Result<void>::success();
    }
    const auto steps = assessment.databaseKind == DatabaseKind::Central ? centralMigrationSteps()
                                                                         : projectMigrationSteps();
    const bool ledgerAlreadyExists = assessment.layout == SchemaLayout::ProjectVersion2;
    for (const auto &step : steps)
    {
        if (ledgerAlreadyExists && step.version <= assessment.sourceVersion)
        {
            continue;
        }
        auto prepared = transaction.prepare("INSERT INTO schema_migrations("
                                            "version, identifier, applied_at, content_sha256"
                                            ") VALUES(?1, ?2, ?3, ?4);");
        if (!prepared)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(prepared).error()));
        }
        auto statement = std::move(prepared).value();
        auto bound = statement.bindInt64(1, step.version);
        if (!bound)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(bound).error()));
        }
        bound = statement.bindText(2, step.identifier);
        if (!bound)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(bound).error()));
        }
        bound = statement.bindText(3, appliedAt);
        if (!bound)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(bound).error()));
        }
        bound = statement.bindText(4, step.contentSha256);
        if (!bound)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(bound).error()));
        }
        auto stepped = statement.step();
        if (!stepped)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(stepped).error()));
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Done)
        {
            return migrationFailure<void>(
                "A migration-ledger insertion unexpectedly returned a row.");
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> updatePhysicalVersion(Detail::WinsqliteTransaction &transaction,
                                                         const SchemaAssessment &assessment)
{
    if (assessment.sourceVersion >= assessment.targetVersion)
    {
        return Domain::Result<void>::success();
    }
    if (assessment.databaseKind == DatabaseKind::Project)
    {
        auto updated = transaction.execute("PRAGMA main.user_version = 3;");
        if (!updated)
        {
            return Domain::Result<void>::failure(asMigrationError(std::move(updated).error()));
        }
        return Domain::Result<void>::success();
    }

    auto prepared = transaction.prepare("UPDATE schema_version SET version = ?1;");
    if (!prepared)
    {
        return Domain::Result<void>::failure(asMigrationError(std::move(prepared).error()));
    }
    auto statement = std::move(prepared).value();
    auto bound = statement.bindInt64(1, CentralPhysicalVersion);
    if (!bound)
    {
        return Domain::Result<void>::failure(asMigrationError(std::move(bound).error()));
    }
    auto stepped = statement.step();
    if (!stepped)
    {
        return Domain::Result<void>::failure(asMigrationError(std::move(stepped).error()));
    }
    if (stepped.value() != Detail::WinsqliteStepResult::Done)
    {
        return migrationFailure<void>(
            "The central schema-version update unexpectedly returned a row.");
    }
    return Domain::Result<void>::success();
}

template <typename Reader> [[nodiscard]] Domain::Result<void> runIntegrityChecks(Reader &reader)
{
    {
        auto quickPrepared = reader.prepare("PRAGMA main.quick_check(1);");
        if (!quickPrepared)
        {
            return integrityFailure<void>(
                "The migrated database quick-check could not be started.");
        }
        auto quick = std::move(quickPrepared).value();
        auto stepped = quick.step();
        if (!stepped)
        {
            if (preserveOperationalError(stepped.error().code))
            {
                return Domain::Result<void>::failure(std::move(stepped).error());
            }
            return integrityFailure<void>("The migrated database failed its quick-check.");
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Row)
        {
            return integrityFailure<void>("The migrated database quick-check returned no result.");
        }
        auto quickResult = quick.columnText(0, MaximumSchemaTextBytes);
        if (!quickResult)
        {
            if (preserveOperationalError(quickResult.error().code))
            {
                return Domain::Result<void>::failure(std::move(quickResult).error());
            }
            return integrityFailure<void>(
                "The migrated database quick-check result is unreadable.");
        }
        if (!quickResult.value().has_value() || *quickResult.value() != "ok")
        {
            return integrityFailure<void>("The migrated database failed its quick-check.");
        }
        stepped = quick.step();
        if (!stepped)
        {
            if (preserveOperationalError(stepped.error().code))
            {
                return Domain::Result<void>::failure(std::move(stepped).error());
            }
            return integrityFailure<void>(
                "The migrated database quick-check did not finish cleanly.");
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Done)
        {
            return integrityFailure<void>(
                "The migrated database quick-check reported multiple rows.");
        }
    }
    {
        auto foreignPrepared = reader.prepare("PRAGMA main.foreign_key_check;");
        if (!foreignPrepared)
        {
            return integrityFailure<void>(
                "The migrated database foreign-key check could not be started.");
        }
        auto foreign = std::move(foreignPrepared).value();
        auto stepped = foreign.step();
        if (!stepped)
        {
            if (preserveOperationalError(stepped.error().code))
            {
                return Domain::Result<void>::failure(std::move(stepped).error());
            }
            return integrityFailure<void>("The migrated database failed its foreign-key check.");
        }
        if (stepped.value() != Detail::WinsqliteStepResult::Done)
        {
            return integrityFailure<void>("The migrated database has a foreign-key violation.");
        }
    }
    return Domain::Result<void>::success();
}

} // namespace

MigrationBackupReceipt::MigrationBackupReceipt(
                                               std::shared_ptr<Detail::WinsqliteTransactionLifetime>
                                                   transaction,
                                               const SchemaAssessment &assessment,
                                               std::string evidenceId,
                                               std::shared_ptr<const
                                                   Detail::MigrationBackupArtifactLease>
                                                   backupArtifact) noexcept
    : transaction_{std::move(transaction)}, assessment_{assessment},
      evidenceId_{std::move(evidenceId)},
      backupArtifact_{std::move(backupArtifact)}
{
}

Domain::Result<void> MigrationBackupReceipt::revalidateArtifact() const noexcept
{
    return Detail::revalidateMigrationBackupArtifact(backupArtifact_);
}

SchemaMigrator::SchemaMigrator(Detail::WinsqliteConnection &connection) noexcept
    : connection_{connection}
{
}

Domain::Result<SchemaAssessment> SchemaMigrator::assess(
    const DatabaseKind databaseKind, const Domain::OperationContext &context) noexcept
{
    try
    {
        ConnectionReader reader{connection_, context};
        return assessImpl(reader, databaseKind);
    }
    catch (...)
    {
        return Domain::Result<SchemaAssessment>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The database schema assessment failed unexpectedly."));
    }
}

Domain::Result<SchemaAssessment> SchemaMigrator::assess(
    Detail::WinsqliteTransaction &transaction,
    const DatabaseKind databaseKind) noexcept
{
    try
    {
        auto lifetime = transaction.lifetime_;
        if (!transaction.active_ || transaction.operation_ == nullptr ||
            lifetime == nullptr || !lifetime->active ||
            transaction.connectionIdentity_ != connection_.state_.get())
        {
            return Domain::Result<SchemaAssessment>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Schema assessment requires the exact live admitted transaction."));
        }
        std::unique_lock lock{lifetime->mutex};
        if (!lifetime->active)
        {
            return Domain::Result<SchemaAssessment>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The admitted schema-assessment transaction already ended."));
        }
        TransactionReader reader{transaction};
        return assessImpl(reader, databaseKind);
    }
    catch (...)
    {
        return Domain::Result<SchemaAssessment>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The locked database schema assessment failed unexpectedly."));
    }
}

Domain::Result<SchemaAssessment> SchemaMigrator::migrate(
    Detail::WinsqliteTransaction &transaction,
    const SchemaAssessment &priorAssessment,
    const MigrationBackupReceipt *const verifiedBackup,
    const std::string_view appliedAt) noexcept
{
    try
    {
        auto transactionLifetime = transaction.lifetime_;
        if (!transaction.active_ || transaction.operation_ == nullptr ||
            transactionLifetime == nullptr ||
            transaction.connectionIdentity_ != connection_.state_.get())
        {
            return migrationFailure<SchemaAssessment>(
                "Migration requires the exact live admitted transaction.");
        }
        std::unique_lock transactionLock{transactionLifetime->mutex};
        if (!transactionLifetime->active)
        {
            return migrationFailure<SchemaAssessment>(
                "The admitted migration transaction already ended.");
        }
        if (!validDatabaseKind(priorAssessment.databaseKind) ||
            !validLayoutForKind(priorAssessment.databaseKind, priorAssessment.layout) ||
            !isBoundedPrintableText(appliedAt, MaximumAppliedAtBytes))
        {
            return rollbackFailure<SchemaAssessment>(
                transaction,
                Domain::makeError(
                    Domain::ErrorCodes::MigrationFailed,
                    "The migration assessment or applied-at value is invalid."));
        }

        if (priorAssessment.requiresOnlineBackup)
        {
            auto receiptTransaction = verifiedBackup != nullptr
                ? verifiedBackup->transaction_.lock()
                : std::shared_ptr<Detail::WinsqliteTransactionLifetime>{};
            if (verifiedBackup == nullptr ||
                receiptTransaction.get() != transactionLifetime.get() ||
                verifiedBackup->assessment_ != priorAssessment ||
                !isBoundedEvidenceId(verifiedBackup->evidenceId_))
            {
                return rollbackFailure<SchemaAssessment>(
                    transaction,
                    Domain::makeError(
                        Domain::ErrorCodes::MigrationFailed,
                        "A matching transaction-bound online-backup receipt is required before migration."));
            }
            auto validBackupArtifact = verifiedBackup->revalidateArtifact();
            if (!validBackupArtifact)
            {
                return rollbackFailure<SchemaAssessment>(
                    transaction, std::move(validBackupArtifact).error());
            }
        }
        else if (verifiedBackup != nullptr)
        {
            return rollbackFailure<SchemaAssessment>(
                transaction,
                Domain::makeError(
                    Domain::ErrorCodes::MigrationFailed,
                    "The migration received a backup receipt for a schema that does not require one."));
        }
        TransactionReader reader{transaction};

        auto lockedAssessment = assessImpl(reader, priorAssessment.databaseKind);
        if (!lockedAssessment)
        {
            return rollbackFailure<SchemaAssessment>(transaction,
                                                     std::move(lockedAssessment).error());
        }
        if (lockedAssessment.value() != priorAssessment)
        {
            return rollbackFailure<SchemaAssessment>(
                transaction,
                Domain::makeError(
                    Domain::ErrorCodes::MigrationFailed,
                    "The database schema changed after its read-only migration assessment."));
        }

        auto executed = executeRequiredMigrationSteps(transaction, priorAssessment);
        if (!executed)
        {
            return rollbackFailure<SchemaAssessment>(transaction, std::move(executed).error());
        }
        auto recorded = recordMigrationLedger(transaction, priorAssessment, appliedAt);
        if (!recorded)
        {
            return rollbackFailure<SchemaAssessment>(transaction, std::move(recorded).error());
        }
        auto versionUpdated = updatePhysicalVersion(transaction, priorAssessment);
        if (!versionUpdated)
        {
            return rollbackFailure<SchemaAssessment>(transaction,
                                                     std::move(versionUpdated).error());
        }

        auto finalAssessment = assessImpl(reader, priorAssessment.databaseKind);
        if (!finalAssessment)
        {
            return rollbackFailure<SchemaAssessment>(transaction,
                                                     std::move(finalAssessment).error());
        }
        const SchemaLayout expectedCurrentLayout =
            priorAssessment.databaseKind == DatabaseKind::Central ? SchemaLayout::CentralVersion6
                                                                  : SchemaLayout::ProjectVersion3;
        if (finalAssessment.value().layout != expectedCurrentLayout ||
            finalAssessment.value().sourceVersion != priorAssessment.targetVersion ||
            finalAssessment.value().targetVersion != priorAssessment.targetVersion ||
            finalAssessment.value().requiresOnlineBackup)
        {
            return rollbackFailure<SchemaAssessment>(
                transaction,
                Domain::makeError(Domain::ErrorCodes::MigrationFailed,
                                  "The migrated database did not reach its exact target schema."));
        }

        auto integrity = runIntegrityChecks(reader);
        if (!integrity)
        {
            return rollbackFailure<SchemaAssessment>(transaction, std::move(integrity).error());
        }

        auto committed = transaction.commit();
        if (!committed)
        {
            return rollbackFailure<SchemaAssessment>(transaction, std::move(committed).error());
        }
        return finalAssessment;
    }
    catch (...)
    {
        if (transaction.isActive())
        {
            static_cast<void>(transaction.rollback());
        }
        return migrationFailure<SchemaAssessment>(
            "The database migration failed unexpectedly and was rolled back.");
    }
}

} // namespace ForgeConductor::Persistence::Windows::Migrations
