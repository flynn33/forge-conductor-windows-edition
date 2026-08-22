// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Models.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Forge::Domain {

class IConfiguration {
public:
    virtual std::string stringAt(const std::string& key, const std::string& fallback) const = 0;
    virtual int intAt(const std::string& key, int fallback) const = 0;
    virtual bool boolAt(const std::string& key, bool fallback) const = 0;
    virtual AppConfig model() const = 0;
    virtual void reload() = 0;
    virtual ~IConfiguration() = default;
};

class IPresenceStore {
public:
    virtual std::vector<PresenceRecord> presenceRecords() = 0;
    virtual void presenceUpsert(const std::string& clientID, const std::string& hostKind, std::int32_t pid, const std::string& cwd) = 0;
    virtual void presenceDelete(const std::string& clientID) = 0;
    virtual int presencePrune(double maxAgeSec) = 0;
    virtual ~IPresenceStore() = default;
};

class ISessionStore {
public:
    virtual std::vector<AgentSession> sessionList(const std::optional<std::string>& agentID = std::nullopt) = 0;
    virtual void sessionUpsert(const AgentSession& session) = 0;
    virtual ~ISessionStore() = default;
};

class IAuditReader {
public:
    virtual std::vector<AuditEvent> auditRecent(int limit) = 0;
    virtual ~IAuditReader() = default;
};

class IAgentCatalog {
public:
    virtual std::vector<AgentSpec> all() const = 0;
    virtual std::optional<AgentSpec> get(const std::string& id) const = 0;
    virtual AgentSpec recommend(const std::string& task) const = 0;
    virtual ~IAgentCatalog() = default;
};

class IToolPack {
public:
    virtual std::vector<std::string> toolNames() const = 0;
    virtual std::optional<ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const ClientID& clientID) = 0;
    virtual ~IToolPack() = default;
};

class IToolExecutor {
public:
    virtual std::vector<std::string> toolNames() const = 0;
    virtual ToolResult call(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const ClientID& clientID) = 0;
    virtual ~IToolExecutor() = default;
};

class ISystemMetricsCollector {
public:
    virtual SystemMetrics collect() = 0;
    virtual ~ISystemMetricsCollector() = default;
};

class ITelemetry {
public:
    virtual TelemetrySnapshot currentFrame() = 0;
    virtual void start(double intervalSec) = 0;
    virtual void stop() = 0;
    virtual std::uint64_t addListener(std::function<void(const TelemetrySnapshot&)> listener) = 0;
    virtual void removeListener(std::uint64_t id) = 0;
    virtual ~ITelemetry() = default;
};

class IDiagnosticLog {
public:
    virtual void info(const std::string& event, const std::map<std::string, std::string>& fields) = 0;
    virtual void warn(const std::string& event, const std::map<std::string, std::string>& fields) = 0;
    virtual void error(const std::string& event, const std::map<std::string, std::string>& fields) = 0;
    virtual ~IDiagnosticLog() = default;
};

class IManagerControl {
public:
    virtual bool isRunning() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void restart() = 0;
    virtual bool startWithWindows() const = 0;
    virtual void setStartWithWindows(bool enabled) = 0;
    virtual ~IManagerControl() = default;
};

class ILmStudioDeploy {
public:
    virtual DoctorReport status() = 0;
    virtual DoctorReport deploy() = 0;
    virtual ~ILmStudioDeploy() = default;
};

} // namespace Forge::Domain
