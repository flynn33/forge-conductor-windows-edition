#pragma once

#include "ForgeConductor/Domain/ContinuityModels.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::uint32_t NativeSessionLedgerSchemaVersion = 1U;
inline constexpr std::size_t MaximumNativeSessionRecords = 4096U;
inline constexpr std::size_t MaximumNativeResponseChunks = 256U;
inline constexpr std::size_t MaximumNativeResponseChunkBytes = 16U * 1024U;
inline constexpr std::size_t MaximumNativeResponseBytes = 256U * 1024U;

struct NativeTransportSession final {
    ProviderSessionId providerSessionId;
    std::optional<std::string> model;
};

struct NativeBootstrapRequest final {
    ContinuityOperationId operationId;
    ProjectId projectId;
    SessionId successorSessionId;
    ProviderSessionId providerSessionId;
    ContinuityHandoffId handoffId;
    Sha256Digest handoffSha256;
    std::string canonicalHandoffUtf8;
};

struct NativeBootstrapResponse final {
    std::vector<std::vector<std::byte>> chunks;
    std::int64_t inputTokens{};
    std::int64_t outputTokens{};
};

// The native ledger intentionally retains only lifecycle identifiers and
// usage. Complete transcripts and canonical handoff bodies never belong here.
struct NativeSessionRecord final {
    HostSession session;
    OperationId ownerOperationId;
    std::optional<ContinuityHandoffId> handoffId;
    std::optional<Sha256Digest> handoffSha256;
    std::uint64_t inputTokens{};
    std::uint64_t outputTokens{};
    UtcTimePoint createdAt;
    UtcTimePoint updatedAt;
};

struct NativeSessionLedger final {
    std::uint32_t schemaVersion{NativeSessionLedgerSchemaVersion};
    std::uint64_t revision{};
    std::vector<NativeSessionRecord> records;
    std::optional<Sha256Digest> contentSha256;
};

struct NativeSessionHostHealth final {
    bool healthy{};
    std::size_t records{};
    std::size_t maximumRecords{MaximumNativeSessionRecords};
    std::size_t maximumResponseBytes{MaximumNativeResponseBytes};
};

struct NativeLogicalContinuation final {
    ProviderSessionId providerSessionId;
    ContinuityHandoffId handoffId;
    std::uint64_t sequence{};
    std::string action;
    std::string command;
    std::string successCondition;
};

[[nodiscard]] Result<void> validateNativeSessionLedger(
    const NativeSessionLedger& ledger);

} // namespace ForgeConductor::Domain
