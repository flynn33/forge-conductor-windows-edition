// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfyStore.h"

#include "ForgeDomain/Clock.h"

#include <nlohmann/json.hpp>
#include <winsqlite/winsqlite3.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Forge::Comfy {
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
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        throw std::runtime_error("sqlite step failed");
    }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? reinterpret_cast<const char*>(value) : std::string{};
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(stmt_, column); }
    bool isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

    sqlite3_stmt* stmt_{nullptr};
};

Domain::PrepareSession sessionFromJson(const std::string& jsonText) {
    Domain::PrepareSession session;
    const auto json = nlohmann::json::parse(jsonText);
    session.id = json.value("id", "");
    session.createdAt = json.value("created_at", "");
    session.ready = json.value("ready", false);
    session.executionPolicy = json.value("execution_policy", "");
    session.comfyUrl = json.value("comfy_url", "");
    session.userMessage = json.value("user_message", "");
    session.resultJson = jsonText;
    if (json.contains("workflow_ids") && json["workflow_ids"].is_array()) {
        session.workflowIds = json["workflow_ids"].get<std::vector<std::string>>();
    }
    if (json.contains("workflow_files") && json["workflow_files"].is_array()) {
        session.workflowFiles = json["workflow_files"].get<std::vector<std::string>>();
    }
    if (json.contains("missing_nodes") && json["missing_nodes"].is_array()) {
        session.missingNodes = json["missing_nodes"].get<std::vector<std::string>>();
    }
    if (json.contains("next_steps") && json["next_steps"].is_array()) {
        session.nextSteps = json["next_steps"].get<std::vector<std::string>>();
    }
    return session;
}

} // namespace

std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashLen = 0;
    DWORD cb = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptGetProperty failed");
    }
    std::vector<unsigned char> digest(hashLen);
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hash, digest.data(), hashLen, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0');
    for (auto byte : digest) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

ComfyStore::ComfyStore(std::filesystem::path path) : path_(std::move(path)) {
    std::filesystem::create_directories(path_.parent_path());
    const auto pathUtf8 = path_.string();
    if (sqlite3_open(pathUtf8.c_str(), &db_) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(msg);
    }
    exec("PRAGMA busy_timeout=3000;");
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    migrate();
}

ComfyStore::~ComfyStore() { close(); }

void ComfyStore::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void ComfyStore::exec(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "exec failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void ComfyStore::migrate() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS workflows (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            revision INTEGER NOT NULL,
            api_json TEXT NOT NULL,
            api_hash TEXT NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS owned_processes (
            id TEXT PRIMARY KEY,
            kind TEXT NOT NULL,
            pid INTEGER NOT NULL,
            executable TEXT NOT NULL,
            command_line TEXT NOT NULL,
            create_time INTEGER NOT NULL,
            cwd TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS prepare_sessions (
            id TEXT PRIMARY KEY,
            created_at TEXT NOT NULL,
            ready INTEGER NOT NULL,
            result_json TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_prepare_sessions_created ON prepare_sessions(created_at DESC);
    )");
    Statement version(db_, "SELECT version FROM schema_version LIMIT 1");
    if (!version.step()) {
        exec("INSERT INTO schema_version(version) VALUES (1);");
    }
}

WorkflowRecord ComfyStore::saveWorkflow(const std::string& name, const std::string& apiJson, const std::string& nowIso) {
    std::lock_guard lock(mutex_);
    WorkflowRecord record;
    record.id = Domain::makeUuid();
    record.name = name;
    record.revision = 1;
    record.apiJson = apiJson;
    record.apiHash = sha256Hex(apiJson);
    record.createdAt = nowIso;
    record.updatedAt = nowIso;
    Statement stmt(db_, "INSERT INTO workflows(id,name,revision,api_json,api_hash,created_at,updated_at) VALUES(?,?,?,?,?,?,?)");
    stmt.bind(1, record.id);
    stmt.bind(2, record.name);
    stmt.bind(3, record.revision);
    stmt.bind(4, record.apiJson);
    stmt.bind(5, record.apiHash);
    stmt.bind(6, record.createdAt);
    stmt.bind(7, record.updatedAt);
    stmt.step();
    return record;
}

std::vector<WorkflowRecord> ComfyStore::listWorkflows() {
    std::lock_guard lock(mutex_);
    std::vector<WorkflowRecord> out;
    Statement stmt(db_, "SELECT id,name,revision,api_json,api_hash,created_at,updated_at FROM workflows ORDER BY updated_at DESC");
    while (stmt.step()) {
        WorkflowRecord record;
        record.id = stmt.text(0);
        record.name = stmt.text(1);
        record.revision = static_cast<int>(stmt.integer(2));
        record.apiJson = stmt.text(3);
        record.apiHash = stmt.text(4);
        record.createdAt = stmt.text(5);
        record.updatedAt = stmt.text(6);
        out.push_back(std::move(record));
    }
    return out;
}

std::optional<WorkflowRecord> ComfyStore::getWorkflow(const std::string& id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT id,name,revision,api_json,api_hash,created_at,updated_at FROM workflows WHERE id=?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    WorkflowRecord record;
    record.id = stmt.text(0);
    record.name = stmt.text(1);
    record.revision = static_cast<int>(stmt.integer(2));
    record.apiJson = stmt.text(3);
    record.apiHash = stmt.text(4);
    record.createdAt = stmt.text(5);
    record.updatedAt = stmt.text(6);
    return record;
}

void ComfyStore::saveProcess(const std::string& id, const std::string& kind, const ProcessIdentity& identity) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "INSERT OR REPLACE INTO owned_processes(id,kind,pid,executable,command_line,create_time,cwd) VALUES(?,?,?,?,?,?,?)");
    stmt.bind(1, id);
    stmt.bind(2, kind);
    stmt.bind(3, identity.pid);
    stmt.bind(4, identity.executable);
    stmt.bind(5, identity.commandLine);
    stmt.bind(6, static_cast<std::int64_t>(identity.createTime));
    stmt.bind(7, identity.workingDirectory.string());
    stmt.step();
}

std::optional<ProcessIdentity> ComfyStore::loadProcess(const std::string& id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT pid,executable,command_line,create_time,cwd FROM owned_processes WHERE id=?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    ProcessIdentity identity;
    identity.pid = static_cast<std::int32_t>(stmt.integer(0));
    identity.executable = stmt.text(1);
    identity.commandLine = stmt.text(2);
    identity.createTime = static_cast<std::uint64_t>(stmt.integer(3));
    identity.workingDirectory = stmt.text(4);
    return identity;
}

void ComfyStore::deleteProcess(const std::string& id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM owned_processes WHERE id=?");
    stmt.bind(1, id);
    stmt.step();
}

std::vector<std::pair<std::string, ProcessIdentity>> ComfyStore::listProcesses() {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, ProcessIdentity>> out;
    Statement stmt(db_, "SELECT id,pid,executable,command_line,create_time,cwd FROM owned_processes");
    while (stmt.step()) {
        ProcessIdentity identity;
        identity.pid = static_cast<std::int32_t>(stmt.integer(1));
        identity.executable = stmt.text(2);
        identity.commandLine = stmt.text(3);
        identity.createTime = static_cast<std::uint64_t>(stmt.integer(4));
        identity.workingDirectory = stmt.text(5);
        out.emplace_back(stmt.text(0), identity);
    }
    return out;
}

void ComfyStore::savePrepareSession(const Domain::PrepareSession& session) {
    std::lock_guard lock(mutex_);
    nlohmann::json json;
    json["id"] = session.id;
    json["created_at"] = session.createdAt;
    json["ready"] = session.ready;
    json["execution_policy"] = session.executionPolicy;
    json["comfy_url"] = session.comfyUrl;
    json["workflow_ids"] = session.workflowIds;
    json["workflow_files"] = session.workflowFiles;
    json["missing_nodes"] = session.missingNodes;
    json["user_message"] = session.userMessage;
    json["next_steps"] = session.nextSteps;
    const auto dump = json.dump();
    Statement stmt(db_, "INSERT INTO prepare_sessions(id,created_at,ready,result_json) VALUES(?,?,?,?)");
    stmt.bind(1, session.id);
    stmt.bind(2, session.createdAt);
    stmt.bind(3, session.ready ? 1 : 0);
    stmt.bind(4, dump);
    stmt.step();
}

std::optional<Domain::PrepareSession> ComfyStore::lastPrepareSession() const {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "SELECT result_json FROM prepare_sessions ORDER BY created_at DESC LIMIT 1");
    if (!stmt.step()) {
        return std::nullopt;
    }
    return sessionFromJson(stmt.text(0));
}

} // namespace Forge::Comfy
