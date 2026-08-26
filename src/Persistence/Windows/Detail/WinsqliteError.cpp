#include "WinsqliteError.h"

#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

inline constexpr std::size_t MaximumNativeMessageBytes = 512U;

[[nodiscard]] int primaryResultCode(const int nativeCode) noexcept
{
    return nativeCode & 0xFF;
}

[[nodiscard]] std::string_view stableCodeFor(
    const int nativeCode,
    const char* const nativeMessage,
    const WinsqliteInterruptionReason interruptionReason,
    const Domain::OperationContext* const context) noexcept
{
    if (interruptionReason == WinsqliteInterruptionReason::Cancelled ||
        (primaryResultCode(nativeCode) == SQLITE_INTERRUPT && context != nullptr &&
         context->isCancellationRequested())) {
        return Domain::ErrorCodes::Cancelled;
    }
    if (interruptionReason == WinsqliteInterruptionReason::DeadlineExceeded ||
        (primaryResultCode(nativeCode) == SQLITE_INTERRUPT && context != nullptr &&
         context->isExpired(std::chrono::steady_clock::now()))) {
        return Domain::ErrorCodes::DeadlineExceeded;
    }
    if (interruptionReason == WinsqliteInterruptionReason::BusyTimeout) {
        return Domain::ErrorCodes::DatabaseBusy;
    }
    if (primaryResultCode(nativeCode) == SQLITE_ERROR && nativeMessage != nullptr) {
        const std::string_view message{nativeMessage};
        if (message.starts_with("not authorized") ||
            message.starts_with("authorization denied")) {
            return Domain::ErrorCodes::Unauthorized;
        }
    }

    switch (primaryResultCode(nativeCode)) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return Domain::ErrorCodes::DatabaseBusy;
    case SQLITE_INTERRUPT:
        return Domain::ErrorCodes::Cancelled;
    case SQLITE_FULL:
        return Domain::ErrorCodes::StorageFull;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return Domain::ErrorCodes::IntegrityFailure;
    case SQLITE_TOOBIG:
        return Domain::ErrorCodes::PayloadTooLarge;
    case SQLITE_CONSTRAINT:
        return Domain::ErrorCodes::Conflict;
    case SQLITE_NOMEM:
        return Domain::ErrorCodes::LimitExceeded;
    case SQLITE_AUTH:
    case SQLITE_PERM:
    case SQLITE_READONLY:
        return Domain::ErrorCodes::Unauthorized;
    default:
        return Domain::ErrorCodes::InternalFailure;
    }
}

[[nodiscard]] bool retryableFor(
    const int nativeCode,
    const WinsqliteInterruptionReason interruptionReason) noexcept
{
    if (interruptionReason == WinsqliteInterruptionReason::Cancelled ||
        interruptionReason == WinsqliteInterruptionReason::DeadlineExceeded) {
        return false;
    }
    if (interruptionReason == WinsqliteInterruptionReason::BusyTimeout) {
        return true;
    }
    const int primaryCode = primaryResultCode(nativeCode);
    return primaryCode == SQLITE_BUSY || primaryCode == SQLITE_LOCKED;
}

} // namespace

Domain::Error makeWinsqliteError(
    const int nativeCode,
    const std::string_view action,
    const char* const nativeMessage,
    const WinsqliteInterruptionReason interruptionReason,
    const Domain::OperationContext* const context) noexcept
{
    try {
        const auto stableCode = stableCodeFor(
            nativeCode, nativeMessage, interruptionReason, context);
        std::string message{"Winsqlite3 could not "};
        message.append(action);
        message += " (result ";
        message += std::to_string(nativeCode);
        message += ')';

        if (nativeMessage != nullptr && nativeMessage[0] != '\0') {
            const std::string_view nativeView{nativeMessage};
            const auto boundedLength = (std::min)(nativeView.size(), MaximumNativeMessageBytes);
            message += ": ";
            message.append(nativeView.substr(0, boundedLength));
            if (nativeView.size() > boundedLength) {
                message += "...";
            }
        }
        message += '.';
        return Domain::makeError(
            stableCode, std::move(message), retryableFor(nativeCode, interruptionReason));
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Winsqlite3 failure occurred and its bounded diagnostic could not be formatted.");
    }
}

} // namespace ForgeConductor::Persistence::Windows::Detail
