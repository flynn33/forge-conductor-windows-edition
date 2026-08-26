#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Domain {

namespace ErrorCodes {

inline constexpr std::string_view InvalidRequest = "invalid_request";
inline constexpr std::string_view UnsupportedVersion = "unsupported_version";
inline constexpr std::string_view ProjectNotFound = "project_not_found";
inline constexpr std::string_view ProjectScopeMismatch = "project_scope_mismatch";
inline constexpr std::string_view RecordNotFound = "record_not_found";
inline constexpr std::string_view AgentNotFound = "agent_not_found";
inline constexpr std::string_view SessionNotFound = "session_not_found";
inline constexpr std::string_view OwnershipConflict = "ownership_conflict";
inline constexpr std::string_view Conflict = "conflict";
inline constexpr std::string_view PayloadTooLarge = "payload_too_large";
inline constexpr std::string_view LimitExceeded = "limit_exceeded";
inline constexpr std::string_view DatabaseBusy = "database_busy";
inline constexpr std::string_view RateLimited = "rate_limited";
inline constexpr std::string_view StorageFull = "storage_full";
inline constexpr std::string_view DeadlineExceeded = "deadline_exceeded";
inline constexpr std::string_view Cancelled = "cancelled";
inline constexpr std::string_view MigrationFailed = "migration_failed";
inline constexpr std::string_view IntegrityFailure = "integrity_failure";
inline constexpr std::string_view RedactionRejected = "redaction_rejected";
inline constexpr std::string_view Unauthorized = "unauthorized";
inline constexpr std::string_view PathOutsideAuthority = "path_outside_authority";
inline constexpr std::string_view ShellDisabled = "shell_disabled";
inline constexpr std::string_view ProcessLaunchFailed = "process_launch_failed";
inline constexpr std::string_view ProcessTimeout = "process_timeout";
inline constexpr std::string_view ProcessTerminationUnconfirmed =
    "process_termination_unconfirmed";
inline constexpr std::string_view TransportClosed = "transport_closed";
inline constexpr std::string_view MalformedMessage = "malformed_message";
inline constexpr std::string_view HostCapabilityUnavailable =
    "host_capability_unavailable";
inline constexpr std::string_view AcknowledgementTimeout =
    "acknowledgement_timeout";
inline constexpr std::string_view InternalFailure = "internal_failure";

} // namespace ErrorCodes

struct Error final {
    std::string code;
    std::string message;
    bool retryable{};
    std::optional<std::string> evidenceId;

    bool operator==(const Error&) const = default;
};

[[nodiscard]] inline Error makeError(
    const std::string_view code,
    std::string message,
    const bool retryable = false,
    std::optional<std::string> evidenceId = std::nullopt)
{
    return Error{
        std::string{code},
        std::move(message),
        retryable,
        std::move(evidenceId)};
}

} // namespace ForgeConductor::Domain
