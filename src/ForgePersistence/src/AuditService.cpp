// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgePersistence/AuditService.h"

#include "ForgeDomain/Clock.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <functional>

namespace Forge::Persistence {
namespace {

std::string digestOf(const std::string& text) {
    std::hash<std::string> hash;
    return std::to_string(hash(text));
}

std::string redact(const std::string& json) {
    auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (!parsed.is_object()) {
        return json.size() > 512 ? json.substr(0, 512) : json;
    }
    for (const char* key : {"body", "narrative", "resume_seed", "content", "command", "text"}) {
        if (parsed.contains(key)) {
            parsed[key] = "[redacted]";
        }
    }
    return parsed.dump();
}

} // namespace

AuditService::AuditService(SQLiteStore& store, AppPaths paths)
    : store_(store)
    , paths_(std::move(paths)) {}

void AuditService::record(
    const std::string& tool,
    const std::string& argsJson,
    const std::string& status,
    std::int64_t durationMs,
    const std::optional<std::string>& error,
    const std::optional<std::string>& clientID) {
    std::lock_guard lock(mutex_);
    Domain::AuditEvent event;
    event.timestamp = Domain::iso8601(Domain::SystemClock{}.now());
    event.clientID = clientID;
    event.tool = tool;
    const auto sanitized = redact(argsJson);
    event.argsJson = sanitized.size() > 4000 ? sanitized.substr(0, 4000) : sanitized;
    event.argsDigest = digestOf(event.argsJson);
    event.status = status;
    event.durationMs = durationMs;
    event.error = error;
    store_.auditInsert(event);

    std::ofstream stream(paths_.auditJSONL(), std::ios::app | std::ios::binary);
    nlohmann::json line;
    line["ts"] = event.timestamp;
    line["tool"] = tool;
    line["status"] = status;
    line["duration_ms"] = durationMs;
    if (error) line["error"] = *error;
    stream << line.dump() << '\n';
}

std::vector<Domain::AuditEvent> AuditService::recent(int limit) {
    return store_.auditRecent(limit);
}

DiagnosticLog::DiagnosticLog(AppPaths paths, std::string role)
    : paths_(std::move(paths))
    , role_(std::move(role)) {}

void DiagnosticLog::info(const std::string& event, const std::map<std::string, std::string>& fields) {
    write("info", event, fields);
}
void DiagnosticLog::warn(const std::string& event, const std::map<std::string, std::string>& fields) {
    write("warn", event, fields);
}
void DiagnosticLog::error(const std::string& event, const std::map<std::string, std::string>& fields) {
    write("error", event, fields);
}

void DiagnosticLog::write(
    const std::string& level,
    const std::string& event,
    const std::map<std::string, std::string>& fields) {
    std::lock_guard lock(mutex_);
    std::filesystem::create_directories(paths_.logsDir());
    std::ofstream stream(paths_.masterLog(), std::ios::app | std::ios::binary);
    nlohmann::json line;
    line["ts"] = Domain::iso8601(Domain::SystemClock{}.now());
    line["level"] = level;
    line["event"] = event;
    line["role"] = role_;
    line["fields"] = fields;
    stream << line.dump() << '\n';
}

} // namespace Forge::Persistence
