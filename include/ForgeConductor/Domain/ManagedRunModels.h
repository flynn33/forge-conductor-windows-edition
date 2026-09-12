#pragma once

#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumManagedRunTaskBytes = 128U * 1024U;
inline constexpr std::size_t MaximumManagedRunOutputBytes = 256U * 1024U;

enum class ManagedRunState {
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled
};

struct ManagedProviderTurnRequest final {
    ProjectId projectId;
    SessionId runId;
    std::uint64_t authorityGeneration{};
    std::string input;
    std::optional<ProviderSessionId> previousResponseId;
};

struct ManagedProviderTurnResult final {
    ProviderSessionId responseId;
    std::string outputText;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
};

struct ManagedRunRecord final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    std::string task;
    ManagedRunState state{ManagedRunState::Running};
    std::optional<ProviderSessionId> providerResponseId;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    std::optional<std::uint64_t> retainedContextTokens;
    std::optional<std::string> outputText;
    std::optional<Error> lastError;
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
};

struct ManagedRunStartRequest final {
    SessionId runId;
    ProjectId projectId;
    ClientId clientId;
    OperationId operationId;
    CorrelationId correlationId;
    std::uint64_t authorityGeneration{};
    std::string task;
};

struct ManagedRunSnapshot final {
    ManagedRunRecord record;
    bool managerOwned{true};
    bool cancellationRequested{};
};

} // namespace ForgeConductor::Domain
