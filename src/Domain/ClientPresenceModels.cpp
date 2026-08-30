#include "ForgeConductor/Domain/ClientPresenceModels.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <chrono>
#include <string>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] Result<void> invalid(std::string message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::InvalidRequest, std::move(message)));
}

[[nodiscard]] Result<void> payloadTooLarge(std::string message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::PayloadTooLarge, std::move(message)));
}

[[nodiscard]] bool precedesUnixEpoch(const UtcTimePoint value) noexcept
{
    return value.time_since_epoch() < UtcTimePoint::duration::zero();
}

} // namespace

Result<void> validateClientPresenceIdentity(
    const ClientPresenceIdentity& identity) noexcept
{
    try {
        if (identity.role.empty() ||
            identity.role.find('\0') != std::string::npos ||
            !isValidUtf8(identity.role)) {
            return invalid(
                "Client presence role must be nonempty valid UTF-8 without null bytes.");
        }
        if (identity.role.size() > ClientPresenceLimits::MaximumRoleBytes) {
            return payloadTooLarge(
                "Client presence role exceeds its 64-byte UTF-8 limit.");
        }
        if (identity.processId && *identity.processId == 0U) {
            return invalid(
                "Client presence process ID must be positive when supplied.");
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The client presence identity could not be validated."));
    }
}

Result<void> validateClientPresenceRegistration(
    const ClientPresenceRegistration& registration) noexcept
{
    try {
        auto valid = validateClientPresenceIdentity(registration.identity);
        if (!valid) {
            return valid;
        }
        auto validWorkingDirectory =
            PathText::create(registration.workingDirectory.value());
        if (!validWorkingDirectory) {
            return Result<void>::failure(
                std::move(validWorkingDirectory).error());
        }
        if (precedesUnixEpoch(registration.firstSeenAt) ||
            precedesUnixEpoch(registration.lastSeenAt)) {
            return invalid(
                "Client presence timestamps must not precede the Unix epoch.");
        }
        if (registration.lastSeenAt < registration.firstSeenAt) {
            return invalid(
                "Client presence last-seen timestamp precedes first-seen timestamp.");
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "The client presence registration could not be validated."));
    }
}

} // namespace ForgeConductor::Domain
