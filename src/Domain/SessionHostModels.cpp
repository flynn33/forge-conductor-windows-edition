#include "ForgeConductor/Domain/SessionHostModels.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <unordered_set>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] Result<void> invalid(std::string message)
{
    return Result<void>::failure(makeError(
        ErrorCodes::IntegrityFailure,
        std::move(message)));
}

[[nodiscard]] bool requiresProvider(const HostSessionStatus status) noexcept
{
    return status == HostSessionStatus::Active ||
        status == HostSessionStatus::Bootstrapping ||
        status == HostSessionStatus::Ready ||
        status == HostSessionStatus::Sealed;
}

[[nodiscard]] bool requiresHandoff(const HostSessionStatus status) noexcept
{
    return status == HostSessionStatus::Bootstrapping ||
        status == HostSessionStatus::Ready ||
        status == HostSessionStatus::Sealed;
}

} // namespace

Result<void> validateNativeSessionLedger(const NativeSessionLedger& ledger)
{
    if (ledger.schemaVersion != NativeSessionLedgerSchemaVersion) {
        return invalid("The native session ledger schema version is unsupported.");
    }
    if (ledger.records.size() > MaximumNativeSessionRecords) {
        return invalid("The native session ledger exceeds its record bound.");
    }
    if (ledger.revision > 0U && !ledger.contentSha256) {
        return invalid(
            "The committed native session ledger omits its content checksum.");
    }

    std::unordered_set<std::string> sessionIds;
    std::unordered_set<std::string> idempotencyKeys;
    sessionIds.reserve(ledger.records.size());
    idempotencyKeys.reserve(ledger.records.size());
    for (const auto& record : ledger.records) {
        if (!sessionIds.insert(record.session.id.value()).second ||
            !idempotencyKeys.insert(record.session.idempotencyKey.value()).second) {
            return invalid(
                "The native session ledger contains duplicate lifecycle identifiers.");
        }
        if (record.updatedAt < record.createdAt) {
            return invalid(
                "The native session ledger contains a reversed timestamp interval.");
        }
        if (record.session.model &&
            (record.session.model->size() > 256U ||
             !isValidUtf8(*record.session.model))) {
            return invalid("The native session ledger contains an invalid model name.");
        }
        if (requiresProvider(record.session.status) &&
            !record.session.providerSessionId) {
            return invalid(
                "The native session ledger omits a required provider session binding.");
        }
        if (record.handoffId.has_value() != record.handoffSha256.has_value()) {
            return invalid(
                "The native session ledger contains a partial handoff binding.");
        }
        if (requiresHandoff(record.session.status) && !record.handoffId) {
            return invalid(
                "The native session ledger omits a required handoff binding.");
        }
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
