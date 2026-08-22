// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgePersistence/AppPaths.h"
#include "ForgePersistence/SQLiteStore.h"

#include <mutex>

namespace Forge::Persistence {

class AuditService final {
public:
    AuditService(SQLiteStore& store, AppPaths paths);

    void record(
        const std::string& tool,
        const std::string& argsJson,
        const std::string& status,
        std::int64_t durationMs,
        const std::optional<std::string>& error,
        const std::optional<std::string>& clientID);

    std::vector<Domain::AuditEvent> recent(int limit);

private:
    SQLiteStore& store_;
    AppPaths paths_;
    std::mutex mutex_;
};

class DiagnosticLog final : public Domain::IDiagnosticLog {
public:
    DiagnosticLog(AppPaths paths, std::string role);

    void info(const std::string& event, const std::map<std::string, std::string>& fields) override;
    void warn(const std::string& event, const std::map<std::string, std::string>& fields) override;
    void error(const std::string& event, const std::map<std::string, std::string>& fields) override;

private:
    void write(const std::string& level, const std::string& event, const std::map<std::string, std::string>& fields);
    AppPaths paths_;
    std::string role_;
    std::mutex mutex_;
};

} // namespace Forge::Persistence
