// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePersistence/SQLiteStore.h"

#include <nlohmann/json.hpp>
#include <winsqlite/winsqlite3.h>

#include <stdexcept>
#include <utility>

namespace Forge::Persistence {
namespace {

class Statement final {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }
    ~Statement() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int index, std::int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void bindNull(int index) { sqlite3_bind_null(stmt_, index); }

    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error("SQLite step failed");
    }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : std::string{};
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    bool isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

private:
    sqlite3_stmt* stmt_{nullptr};
};

std::string tagsToJson(const std::vector<std::string>& tags) {
    return nlohmann::json(tags).dump();
}

std::vector<std::string> tagsFromJson(const std::string& json) {
    if (json.empty()) {
        return {};
    }
    return nlohmann::json::parse(json).get<std::vector<std::string>>();
}

} // namespace

SQLiteStore::SQLiteStore(std::filesystem::path path, std::shared_ptr<Domain::IClock> clock)
    : path_(std::move(path))
    , clock_(std::move(clock)) {
    std::filesystem::create_directories(path_.parent_path());
    const auto utf8 = path_.u8string();
    const std::string pathUtf8(utf8.begin(), utf8.end());
    if (sqlite3_open(pathUtf8.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("SQLite open failed: " + msg);
    }
    exec("PRAGMA busy_timeout=3000;");
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    migrate();
}

SQLiteStore::~SQLiteStore() { close(); }

void SQLiteStore::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SQLiteStore::exec(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "exec failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void SQLiteStore::migrate() {
    exec(R"(
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
        CREATE INDEX IF NOT EXISTS idx_context_handoffs_updated ON context_handoffs(updated_at DESC);
        CREATE INDEX IF NOT EXISTS idx_context_handoffs_sequence ON context_handoffs(write_sequence DESC);
    )");

    Statement version(db_, "SELECT version FROM schema_version LIMIT 1");
    if (!version.step()) {
        exec("INSERT INTO schema_version(version) VALUES (5);");
    } else if (version.integer(0) < 5) {
        exec("UPDATE schema_version SET version = 5;");
    }
}

std::vector<Domain::PresenceRecord> SQLiteStore::presenceRecords() {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT client_id, host_kind, pid, cwd, last_heartbeat FROM presence");
    std::vector<Domain::PresenceRecord> rows;
    while (stmt.step()) {
        Domain::PresenceRecord row;
        row.clientID = stmt.text(0);
        row.hostKind = stmt.text(1);
        row.pid = static_cast<std::int32_t>(stmt.integer(2));
        row.cwd = stmt.text(3);
        row.lastHeartbeat = stmt.text(4);
        rows.push_back(std::move(row));
    }
    return rows;
}

void SQLiteStore::presenceUpsert(
    const std::string& clientID,
    const std::string& hostKind,
    std::int32_t pid,
    const std::string& cwd) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, R"(
        INSERT INTO presence(client_id, host_kind, pid, cwd, last_heartbeat)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(client_id) DO UPDATE SET
            host_kind=excluded.host_kind,
            pid=excluded.pid,
            cwd=excluded.cwd,
            last_heartbeat=excluded.last_heartbeat
    )");
    stmt.bind(1, clientID);
    stmt.bind(2, hostKind);
    stmt.bind(3, static_cast<std::int64_t>(pid));
    stmt.bind(4, cwd);
    stmt.bind(5, Domain::iso8601(clock_->now()));
    stmt.step();
}

void SQLiteStore::presenceDelete(const std::string& clientID) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM presence WHERE client_id=?");
    stmt.bind(1, clientID);
    stmt.step();
}

int SQLiteStore::presencePrune(double maxAgeSec) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT client_id, last_heartbeat FROM presence");
    std::vector<std::string> stale;
    const auto now = clock_->now();
    while (stmt.step()) {
        // Best-effort: if timestamp parse fails, keep the row.
        (void)maxAgeSec;
        (void)now;
    }
    Statement del(db_, R"(
        DELETE FROM presence WHERE last_heartbeat < datetime('now', ?)
    )");
    del.bind(1, "-" + std::to_string(static_cast<int>(maxAgeSec)) + " seconds");
    del.step();
    return sqlite3_changes(db_);
}

std::vector<Domain::AgentSession> SQLiteStore::sessionList(const std::optional<std::string>& agentID) {
    std::lock_guard lock(mutex_);
    const char* sql = agentID
        ? "SELECT id, agent_id, client_id, status, summary, created_at, updated_at FROM agent_sessions WHERE agent_id=?"
        : "SELECT id, agent_id, client_id, status, summary, created_at, updated_at FROM agent_sessions";
    Statement stmt(db_, sql);
    if (agentID) {
        stmt.bind(1, *agentID);
    }
    std::vector<Domain::AgentSession> rows;
    while (stmt.step()) {
        Domain::AgentSession session;
        session.id = stmt.text(0);
        session.agentID = stmt.text(1);
        if (!stmt.isNull(2)) session.clientID = stmt.text(2);
        const auto status = stmt.text(3);
        session.status = status == "completed" ? Domain::SessionStatus::Completed
            : status == "failed" ? Domain::SessionStatus::Failed
            : status == "idle" ? Domain::SessionStatus::Idle
            : Domain::SessionStatus::Open;
        if (!stmt.isNull(4)) session.summary = stmt.text(4);
        session.createdAt = stmt.text(5);
        session.updatedAt = stmt.text(6);
        rows.push_back(std::move(session));
    }
    return rows;
}

void SQLiteStore::sessionUpsert(const Domain::AgentSession& session) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, R"(
        INSERT INTO agent_sessions(id, agent_id, client_id, status, summary, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            agent_id=excluded.agent_id,
            client_id=excluded.client_id,
            status=excluded.status,
            summary=excluded.summary,
            updated_at=excluded.updated_at
    )");
    const char* status = "open";
    switch (session.status) {
    case Domain::SessionStatus::Completed: status = "completed"; break;
    case Domain::SessionStatus::Failed: status = "failed"; break;
    case Domain::SessionStatus::Idle: status = "idle"; break;
    case Domain::SessionStatus::Open: status = "open"; break;
    }
    stmt.bind(1, session.id);
    stmt.bind(2, session.agentID);
    if (session.clientID) stmt.bind(3, *session.clientID); else stmt.bindNull(3);
    stmt.bind(4, status);
    if (session.summary) stmt.bind(5, *session.summary); else stmt.bindNull(5);
    stmt.bind(6, session.createdAt);
    stmt.bind(7, session.updatedAt);
    stmt.step();
}

std::vector<Domain::AuditEvent> SQLiteStore::auditRecent(int limit) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT id, timestamp, client_id, tool, args_digest, args_json, status, duration_ms, error FROM audit_events ORDER BY id DESC LIMIT ?");
    stmt.bind(1, static_cast<std::int64_t>(limit));
    std::vector<Domain::AuditEvent> rows;
    while (stmt.step()) {
        Domain::AuditEvent event;
        event.id = stmt.integer(0);
        event.timestamp = stmt.text(1);
        if (!stmt.isNull(2)) event.clientID = stmt.text(2);
        event.tool = stmt.text(3);
        event.argsDigest = stmt.text(4);
        event.argsJson = stmt.text(5);
        event.status = stmt.text(6);
        event.durationMs = stmt.integer(7);
        if (!stmt.isNull(8)) event.error = stmt.text(8);
        rows.push_back(std::move(event));
    }
    return rows;
}

void SQLiteStore::auditInsert(const Domain::AuditEvent& event) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, R"(
        INSERT INTO audit_events(timestamp, client_id, tool, args_digest, args_json, status, duration_ms, error)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )");
    stmt.bind(1, event.timestamp);
    if (event.clientID) stmt.bind(2, *event.clientID); else stmt.bindNull(2);
    stmt.bind(3, event.tool);
    stmt.bind(4, event.argsDigest);
    stmt.bind(5, event.argsJson);
    stmt.bind(6, event.status);
    stmt.bind(7, event.durationMs);
    if (event.error) stmt.bind(8, *event.error); else stmt.bindNull(8);
    stmt.step();
}

void SQLiteStore::memorySet(const Domain::MemoryNote& note) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, R"(
        INSERT INTO memory_notes(key, body, tags_json, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(key) DO UPDATE SET body=excluded.body, tags_json=excluded.tags_json, updated_at=excluded.updated_at
    )");
    stmt.bind(1, note.key);
    stmt.bind(2, note.body);
    stmt.bind(3, tagsToJson(note.tags));
    stmt.bind(4, note.createdAt);
    stmt.bind(5, note.updatedAt);
    stmt.step();
}

std::optional<Domain::MemoryNote> SQLiteStore::memoryGet(const std::string& key) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT key, body, tags_json, created_at, updated_at FROM memory_notes WHERE key=?");
    stmt.bind(1, key);
    if (!stmt.step()) {
        return std::nullopt;
    }
    Domain::MemoryNote note;
    note.key = stmt.text(0);
    note.body = stmt.text(1);
    note.tags = tagsFromJson(stmt.text(2));
    note.createdAt = stmt.text(3);
    note.updatedAt = stmt.text(4);
    return note;
}

std::vector<Domain::MemoryNote> SQLiteStore::memoryList(
    const std::optional<std::string>& prefix,
    bool includeSystem) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT key, body, tags_json, created_at, updated_at FROM memory_notes ORDER BY key");
    std::vector<Domain::MemoryNote> rows;
    while (stmt.step()) {
        Domain::MemoryNote note;
        note.key = stmt.text(0);
        if (!includeSystem && (note.key.rfind("agent_run/", 0) == 0 || note.key.rfind("continuity/", 0) == 0)) {
            continue;
        }
        if (prefix && note.key.rfind(*prefix, 0) != 0) {
            continue;
        }
        note.body = stmt.text(1);
        note.tags = tagsFromJson(stmt.text(2));
        note.createdAt = stmt.text(3);
        note.updatedAt = stmt.text(4);
        rows.push_back(std::move(note));
    }
    return rows;
}

void SQLiteStore::memoryDelete(const std::string& key) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM memory_notes WHERE key=?");
    stmt.bind(1, key);
    stmt.step();
}

std::vector<Domain::MemoryNote> SQLiteStore::memorySearch(const std::string& query) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT key, body, tags_json, created_at, updated_at FROM memory_notes WHERE key LIKE ? OR body LIKE ?");
    const auto like = "%" + query + "%";
    stmt.bind(1, like);
    stmt.bind(2, like);
    std::vector<Domain::MemoryNote> rows;
    while (stmt.step()) {
        Domain::MemoryNote note;
        note.key = stmt.text(0);
        if (note.key.rfind("agent_run/", 0) == 0 || note.key.rfind("continuity/", 0) == 0) {
            continue;
        }
        note.body = stmt.text(1);
        note.tags = tagsFromJson(stmt.text(2));
        note.createdAt = stmt.text(3);
        note.updatedAt = stmt.text(4);
        rows.push_back(std::move(note));
    }
    return rows;
}

int SQLiteStore::memoryCount() const {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT COUNT(*) FROM memory_notes");
    return stmt.step() ? static_cast<int>(stmt.integer(0)) : 0;
}

namespace {

Domain::HandoffPacket readHandoffRow(Statement& stmt) {
    Domain::HandoffPacket packet;
    packet.id = stmt.text(0);
    packet.createdAt = stmt.text(1);
    packet.updatedAt = stmt.text(2);
    packet.source = stmt.text(3);
    packet.resumeReady = stmt.integer(4) != 0;
    packet.packetJson = stmt.text(5);
    if (!stmt.isNull(6)) packet.clientID = stmt.text(6);
    packet.writeSequence = stmt.integer(7);
    if (!packet.packetJson.empty()) {
        const auto json = nlohmann::json::parse(packet.packetJson, nullptr, false);
        if (json.is_object()) {
            if (json.contains("cwd") && json["cwd"].is_string()) packet.cwd = json["cwd"].get<std::string>();
            if (json.contains("goal")) packet.goal = json.value("goal", "");
            if (json.contains("narrative")) packet.narrative = json.value("narrative", "");
            if (json.contains("resume_seed")) packet.resumeSeed = json.value("resume_seed", "");
            if (json.contains("key_files") && json["key_files"].is_array()) {
                packet.keyFiles = json["key_files"].get<std::vector<std::string>>();
            }
            if (json.contains("next_actions") && json["next_actions"].is_array()) {
                packet.nextActions = json["next_actions"].get<std::vector<std::string>>();
            }
        }
    }
    return packet;
}

} // namespace

void SQLiteStore::handoffUpsert(const Domain::HandoffPacket& packet) {
    std::lock_guard lock(mutex_);
    nlohmann::json json = nlohmann::json::parse(packet.packetJson.empty() ? "{}" : packet.packetJson, nullptr, false);
    if (!json.is_object()) {
        json = nlohmann::json::object();
    }
    if (packet.cwd) json["cwd"] = *packet.cwd;
    json["goal"] = packet.goal;
    json["narrative"] = packet.narrative;
    json["resume_seed"] = packet.resumeSeed;
    json["key_files"] = packet.keyFiles;
    json["next_actions"] = packet.nextActions;
    Statement stmt(db_, R"(
        INSERT INTO context_handoffs(id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            updated_at=excluded.updated_at,
            source=excluded.source,
            resume_ready=excluded.resume_ready,
            packet_json=excluded.packet_json,
            client_id=excluded.client_id,
            write_sequence=excluded.write_sequence
    )");
    stmt.bind(1, packet.id);
    stmt.bind(2, packet.createdAt);
    stmt.bind(3, packet.updatedAt);
    stmt.bind(4, packet.source);
    stmt.bind(5, static_cast<std::int64_t>(packet.resumeReady ? 1 : 0));
    stmt.bind(6, json.dump());
    stmt.bind(7, packet.clientID);
    stmt.bind(8, packet.writeSequence);
    stmt.step();
}

std::optional<Domain::HandoffPacket> SQLiteStore::handoffLatest(bool resumeReadyOnly) {
    std::lock_guard lock(mutex_);
    const char* sql = resumeReadyOnly
        ? "SELECT id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence FROM context_handoffs WHERE resume_ready=1 ORDER BY write_sequence DESC LIMIT 1"
        : "SELECT id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence FROM context_handoffs ORDER BY write_sequence DESC LIMIT 1";
    Statement stmt(db_, sql);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return readHandoffRow(stmt);
}

std::optional<Domain::HandoffPacket> SQLiteStore::handoffGet(const std::string& id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence FROM context_handoffs WHERE id=?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return readHandoffRow(stmt);
}

std::vector<Domain::HandoffPacket> SQLiteStore::handoffList(int limit) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT id, created_at, updated_at, source, resume_ready, packet_json, client_id, write_sequence FROM context_handoffs ORDER BY write_sequence DESC LIMIT ?");
    stmt.bind(1, static_cast<std::int64_t>(limit));
    std::vector<Domain::HandoffPacket> rows;
    while (stmt.step()) {
        rows.push_back(readHandoffRow(stmt));
    }
    return rows;
}

} // namespace Forge::Persistence
