// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Forge::Domain {

struct ClientID final {
    std::string rawValue;
    static ClientID generate();
};

struct JsonObject final {
    std::map<std::string, std::string> strings;
    std::map<std::string, double> numbers;
    std::map<std::string, bool> booleans;
    std::map<std::string, std::string> raw; // pre-encoded JSON fragments
};

struct ToolResult final {
    bool ok{true};
    std::map<std::string, std::string> payload;
    std::string code;
    std::string message;
    bool retryable{false};

    static ToolResult success(std::map<std::string, std::string> payload = {}) {
        ToolResult result;
        result.ok = true;
        result.payload = std::move(payload);
        return result;
    }

    static ToolResult failure(std::string code, std::string message, bool retryable = false) {
        ToolResult result;
        result.ok = false;
        result.code = std::move(code);
        result.message = std::move(message);
        result.retryable = retryable;
        result.payload["code"] = result.code;
        result.payload["message"] = result.message;
        return result;
    }
};

enum class SessionStatus { Open, Completed, Failed, Idle };

struct AgentSpec final {
    std::string id;
    std::string displayName;
    std::string description;
    std::vector<std::string> tools;
    std::vector<std::string> toolsForbidden;
    std::vector<std::string> whenToUse;
    std::string playbookMarkdown;
};

struct AgentSession final {
    std::string id;
    std::string agentID;
    std::optional<std::string> clientID;
    SessionStatus status{SessionStatus::Open};
    std::optional<std::string> summary;
    std::string createdAt;
    std::string updatedAt;
    std::optional<std::string> cwd;
};

struct ActiveBinding final {
    std::string sessionID;
    std::string agentID;
    std::optional<std::string> cwd;
    std::vector<std::string> toolsPrimary;
    std::vector<std::string> toolsForbidden;
};

struct PresenceRecord final {
    std::string clientID;
    std::string hostKind;
    std::int32_t pid{0};
    std::string cwd;
    std::string lastHeartbeat;
};

struct AuditEvent final {
    std::int64_t id{0};
    std::string timestamp;
    std::optional<std::string> clientID;
    std::string tool;
    std::string argsDigest;
    std::string argsJson;
    std::string status;
    std::int64_t durationMs{0};
    std::optional<std::string> error;
};

struct MemoryNote final {
    std::string key;
    std::string body;
    std::vector<std::string> tags;
    std::string createdAt;
    std::string updatedAt;
};

struct HandoffPacket final {
    std::string id;
    std::string createdAt;
    std::string updatedAt;
    std::string source;
    bool resumeReady{false};
    std::string clientID;
    std::int64_t writeSequence{0};
    std::optional<std::string> cwd;
    std::vector<std::string> keyFiles;
    std::string goal;
    std::string narrative;
    std::vector<std::string> nextActions;
    std::string resumeSeed;
    std::string packetJson;
};

struct DoctorCheck final {
    std::string name;
    bool ok{true};
    std::string detail;
    bool hard{true};
};

struct DoctorReport final {
    bool ok{true};
    std::vector<DoctorCheck> checks;
};

struct ComfyToolSpec final {
    std::string name;
    std::string description;
    std::string inputSchemaJson;
};

struct PrepareSession final {
    std::string id;
    std::string createdAt;
    bool ready{false};
    std::string executionPolicy;
    std::string comfyUrl;
    std::vector<std::string> workflowIds;
    std::vector<std::string> workflowFiles;
    std::vector<std::string> missingNodes;
    std::string userMessage;
    std::vector<std::string> nextSteps;
    std::string resultJson;
};

struct AppConfig final {
    std::string logLevel{"info"};
    std::vector<std::string> allowedRoots;
    int shellTimeoutSec{30};
    std::string dashboardHost{"127.0.0.1"};
    int dashboardPort{7788};
    bool managerAutoRestart{true};
    int managerWatchdogSec{3};
    bool openOnStart{false};
    std::string mcpRole{"primary"};
    int sessionIdleTtlSec{14400};
};

struct CPUMetrics final {
    double totalPercent{0};
    std::vector<double> perCore;
    double frequencyMhz{0};
    std::string brand;
};

struct RAMMetrics final {
    double usedPercent{0};
    std::uint64_t usedBytes{0};
    std::uint64_t totalBytes{0};
};

struct GPUMetrics final {
    std::string name;
    double utilizationPercent{0};
    double util3d{0};
    double utilCompute{0};
    double utilCopy{0};
    std::uint64_t dedicatedMemoryBytes{0};
};

struct DiskVolume final {
    std::string name;
    std::string mount;
    std::uint64_t totalBytes{0};
    std::uint64_t freeBytes{0};
};

struct DiskIOMetrics final {
    double readMBps{0};
    double writeMBps{0};
    double readIops{0};
    double writeIops{0};
};

struct ProcessMetrics final {
    std::int32_t pid{0};
    std::string name;
    double cpuPercent{0};
    std::uint64_t workingSetBytes{0};
};

struct PowerMetrics final {
    bool onAc{true};
    int batteryPercent{-1};
};

struct SystemMetrics final {
    CPUMetrics cpu;
    RAMMetrics ram;
    std::vector<GPUMetrics> gpus;
    std::vector<DiskVolume> volumes;
    DiskIOMetrics diskIO;
    std::vector<ProcessMetrics> processes;
    PowerMetrics power;
    double sampleHz{0};
};

struct TelemetrySnapshot final {
    SystemMetrics system;
    std::string version{"0.1.0"};
    std::string product{"Forge-Conductor"};
    std::int32_t pid{0};
    bool primaryAlive{false};
    bool fallbackAlive{false};
    std::vector<PresenceRecord> presence;
    std::vector<std::string> tools;
    std::vector<AuditEvent> recentAudit;
};

enum class TelemetryStatusTone { Healthy, Caution, Failure, Informational, Unavailable };

} // namespace Forge::Domain
