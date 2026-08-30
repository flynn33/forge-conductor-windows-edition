#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Domain {

enum class ManagerStartupState {
    Missing,
    Disabled,
    Ready,
    Drifted,
    ForeignConflict
};

// Transport-neutral launch inputs. Platform adapters own canonicalization and
// registration identity; this value only describes the Manager being launched.
struct ManagerStartupDefinition final {
    PathText managerExecutable;
    PathText home;

    bool operator==(const ManagerStartupDefinition&) const = default;
};

struct ManagerStartupStatus final {
    ManagerStartupState state{ManagerStartupState::Missing};
    bool registered{};
    bool enabled{};
    bool definitionMatches{};
    bool running{};
    std::optional<std::string> registrationIdentity;
    std::optional<std::int32_t> lastResult;
    std::optional<UtcTimePoint> lastRunAt;

    bool operator==(const ManagerStartupStatus&) const = default;
};

struct ManagerStartupOutcome final {
    ManagerStartupStatus status;
    bool changed{};

    bool operator==(const ManagerStartupOutcome&) const = default;
};

inline constexpr std::size_t MaximumManagerStartupRegistrationIdentityBytes =
    2U * 1024U;

[[nodiscard]] Result<void> validateManagerStartupDefinition(
    const ManagerStartupDefinition& definition);

[[nodiscard]] Result<void> validateManagerStartupStatus(
    const ManagerStartupStatus& status);

[[nodiscard]] Result<void> validateManagerStartupOutcome(
    const ManagerStartupOutcome& outcome);

} // namespace ForgeConductor::Domain
