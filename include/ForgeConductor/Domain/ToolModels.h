#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Domain {

enum class ToolEffect { Read, Write, Execute, Destructive };
enum class ToolAvailability { Available, Disabled, MissingDependency, Unhealthy };

struct ToolDescriptor final {
    std::string name;
    std::string description;
    std::string pack;
    ToolEffect effect{ToolEffect::Read};
    ToolAvailability availability{ToolAvailability::Available};
    bool requiresProject{};
    bool requiresShell{};
};

struct AuthorityReference final {
    AuthorityId authorityId;
    std::uint64_t generation{};
};


enum class McpReadStatus { Message, CleanEndOfStream };

struct McpFrame final {
    std::string utf8Json;
};

struct McpReadResult final {
    McpReadStatus status{McpReadStatus::CleanEndOfStream};
    std::optional<McpFrame> frame;
};

struct McpRequestMetadata final {
    RequestId requestId;
    CorrelationId correlationId;
    ClientId clientId;
    std::optional<ProjectId> projectId;
    std::string protocolVersion;
};

struct McpToolDescriptor final {
    ToolDescriptor tool;
    std::string inputSchema;
};

struct ToolCallRequest final {
    McpRequestMetadata metadata;
    std::string toolName;
    std::string canonicalArguments;
};

struct ToolAuthorizationRequest final {
    ToolCallRequest call;
    ToolEffect effect{ToolEffect::Read};
    AuthorityReference authority;
};

struct ToolExecutionReceipt final {
    RequestId requestId;
    std::string toolName;
    bool ok{};
    std::optional<Error> error;
    std::chrono::milliseconds elapsed{};
};

struct ClientWorkspaceSnapshot final {
    ClientId clientId;
    ProjectId projectId;
    PathText authorityRoot;
    LegacyHandoffId handoffId;
    std::uint64_t writeSequence{};
    std::uint64_t generation{};

    bool operator==(const ClientWorkspaceSnapshot&) const = default;
};

struct ClientWorkspaceAdoption final {
    std::optional<ClientWorkspaceSnapshot> snapshot;
    std::optional<Error> warning;
    bool superseded{};
};

// Canonical path observation emitted by the authorized tool adapter. The
// invocation guard consumes this metadata after dispatch so continuity state
// never derives workspace roots from pre-authorization wire arguments.
struct ToolContinuityObservation final {
    std::optional<PathText> path;
    std::optional<PathText> workingDirectory;
    std::optional<PathText> baseDirectory;

    bool operator==(const ToolContinuityObservation&) const = default;
};

struct ContextRecoveryReceipt final {
    ClientId clientId;
    LegacyHandoffId handoffId;
    std::optional<PathText> workingDirectory;
    std::vector<PathText> keyFiles;

    bool operator==(const ContextRecoveryReceipt&) const = default;
};

struct ToolCallOutcome final {
    ToolExecutionReceipt receipt;
    std::string canonicalPayload;
    std::optional<ContextRecoveryReceipt> contextRecovery;
    std::optional<ToolContinuityObservation> continuityObservation;
};

// A missing immediate outcome admits the invocation. A present outcome is a
// fully formed policy response returned without authorizing or dispatching the
// requested handler.
struct ToolInvocationAdmission final {
    std::optional<ToolCallOutcome> immediateOutcome;
};

[[nodiscard]] Result<void> validateToolDescriptor(const ToolDescriptor& descriptor);
[[nodiscard]] Result<void> validateMcpFrame(
    const McpFrame& frame,
    const ResourceBudgets& budgets);

} // namespace ForgeConductor::Domain
