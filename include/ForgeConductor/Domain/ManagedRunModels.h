#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumManagedRunTaskBytes = 128U * 1024U;
inline constexpr std::size_t MaximumManagedRunOutputBytes = 256U * 1024U;

enum class ManagedRunState {
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Paused
};

struct ManagedFunctionCall final {
    std::string callId;
    std::string name;
    std::string canonicalArguments;
};

struct ManagedFunctionCallOutput final {
    std::string callId;
    std::string canonicalOutput;
};

struct ManagedProviderTurnRequest final {
    ProjectId projectId;
    SessionId runId;
    std::uint64_t authorityGeneration{};
    std::string input;
    std::optional<ProviderSessionId> previousResponseId;
    std::vector<McpToolDescriptor> tools;
    std::vector<ManagedFunctionCallOutput> toolOutputs;
};

struct ManagedProviderTurnResult final {
    ProviderSessionId responseId;
    std::string outputText;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
    std::vector<ManagedFunctionCall> functionCalls;
};

struct ManagedRunRecord final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    std::string task;
    std::uint64_t authorityGeneration{};
    ManagedRunState state{ManagedRunState::Running};
    std::optional<ProviderSessionId> providerResponseId;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
    std::optional<std::string> outputText;
    std::optional<Error> lastError;
    std::vector<ManagedFunctionCall> pendingFunctionCalls;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
    bool allowTools{true};
};

struct ManagedRunStartRequest final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    OperationId operationId;
    CorrelationId correlationId;
    std::uint64_t authorityGeneration{};
    std::string task;
    bool allowTools{true};
};

struct ManagedRunSnapshot final {
    ManagedRunRecord record;
    bool managerOwned{true};
    bool cancellationRequested{};
    bool pauseRequested{};
};

} // namespace ForgeConductor::Domain
