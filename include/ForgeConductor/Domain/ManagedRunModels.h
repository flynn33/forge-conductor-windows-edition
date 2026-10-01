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
inline constexpr std::size_t MaximumManagedRunCursorAdvances = 16U;

enum class ManagedRunState {
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Paused
};

enum class ManagedRunEvidenceIntegrity {
    NotTerminal,
    LegacyUnsealed,
    Verified,
    Mismatch
};

enum class ManagedRunDispatchPhase {
    CursorPending,
    Ready,
    ProviderClaimed
};

struct ManagedNativeTaskCheck final {
    Sha256Digest commandDigest;
    Sha256Digest stdoutDigest;
    Sha256Digest stderrDigest;
    int exitCode{};
    bool passed{};
    bool timedOut{};
    bool cancelled{};
    bool terminationConfirmed{};
    std::uint64_t elapsedMilliseconds{};
    UtcTimePoint checkedAt;
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

struct ManagedRunInstructionCursorAdvance final {
    MemoryRecordId recordId;
    std::uint32_t expectedVersion{};
    std::string queueRowId;
    std::uint64_t targetEntry{};
    bool completed{};

    bool operator==(const ManagedRunInstructionCursorAdvance&) const = default;
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
    std::optional<Sha256Digest> evidenceSeal;
    ManagedRunEvidenceIntegrity evidenceIntegrity{
        ManagedRunEvidenceIntegrity::NotTerminal};
    std::vector<ManagedNativeTaskCheck> nativeTaskChecks;
    bool automaticContinuity{true};
    // Stable identity of the caller's pre-enrichment start request. Manager
    // policy and instruction snapshots may change after initial admission.
    std::optional<Sha256Digest> admissionIdentity;
    // Pending admissions are durable but cannot dispatch until the exact
    // instruction cursor plan is committed or reconciled.
    bool dispatchPending{};
    std::vector<ManagedRunInstructionCursorAdvance> instructionCursorAdvances;
    ManagedRunDispatchPhase dispatchPhase{ManagedRunDispatchPhase::Ready};
    std::optional<OperationId> dispatchOperationId;
    std::optional<CorrelationId> dispatchCorrelationId;
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
    bool automaticContinuity{true};
    std::optional<Sha256Digest> admissionIdentity;
    std::vector<ManagedRunInstructionCursorAdvance> instructionCursorAdvances;
};

struct ManagedRunSnapshot final {
    ManagedRunRecord record;
    bool managerOwned{true};
    bool cancellationRequested{};
    bool pauseRequested{};
};

enum class ManagedRunAdmissionDisposition {
    Admitted,
    Replayed
};

struct ManagedRunStartOutcome final {
    ManagedRunSnapshot snapshot;
    ManagedRunAdmissionDisposition disposition{
        ManagedRunAdmissionDisposition::Admitted};
};

} // namespace ForgeConductor::Domain
