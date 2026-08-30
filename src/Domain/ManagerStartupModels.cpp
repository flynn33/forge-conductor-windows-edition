#include "ForgeConductor/Domain/ManagerStartupModels.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <string_view>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] Result<void> invalidStartupModel(std::string message)
{
    return Result<void>::failure(makeError(
        ErrorCodes::InvalidRequest,
        std::move(message)));
}

[[nodiscard]] Result<void> validateRegistrationIdentity(
    const std::optional<std::string>& identity)
{
    if (!identity.has_value()) {
        return invalidStartupModel(
            "A registered Manager startup entry requires an identity.");
    }
    const std::string_view value{*identity};
    if (value.size() > MaximumManagerStartupRegistrationIdentityBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "The Manager startup registration identity exceeds its byte limit."));
    }
    if (value.empty() || value.find('\0') != std::string_view::npos ||
        !isValidUtf8(value)) {
        return invalidStartupModel(
            "The Manager startup registration identity is not strict UTF-8 text.");
    }
    return Result<void>::success();
}

} // namespace

Result<void> validateManagerStartupDefinition(
    const ManagerStartupDefinition& definition)
{
    if (definition.managerExecutable.value() == definition.home.value()) {
        return invalidStartupModel(
            "The Manager executable and Manager home must be distinct paths.");
    }
    if (!isValidUtf8(definition.managerExecutable.value()) ||
        !isValidUtf8(definition.home.value())) {
        return invalidStartupModel(
            "Manager startup paths must be strict UTF-8 text.");
    }
    return Result<void>::success();
}

Result<void> validateManagerStartupStatus(const ManagerStartupStatus& status)
{
    if (status.registered !=
        (status.state != ManagerStartupState::Missing)) {
        return invalidStartupModel(
            "Manager startup registration presence does not match its state.");
    }

    if (!status.registered) {
        if (status.enabled || status.definitionMatches || status.running ||
            status.registrationIdentity.has_value() ||
            status.lastResult.has_value() || status.lastRunAt.has_value()) {
            return invalidStartupModel(
                "A missing Manager startup registration cannot expose task state or history.");
        }
        return Result<void>::success();
    }

    auto validIdentity = validateRegistrationIdentity(
        status.registrationIdentity);
    if (!validIdentity) {
        return validIdentity;
    }

    switch (status.state) {
    case ManagerStartupState::Missing:
        return invalidStartupModel(
            "A registered Manager startup entry cannot be missing.");
    case ManagerStartupState::Disabled:
        if (status.enabled || !status.definitionMatches) {
            return invalidStartupModel(
                "A disabled Manager startup entry must have an exact disabled definition.");
        }
        break;
    case ManagerStartupState::Ready:
        if (!status.enabled || !status.definitionMatches) {
            return invalidStartupModel(
                "A ready Manager startup entry must have an exact enabled definition.");
        }
        break;
    case ManagerStartupState::Drifted:
    case ManagerStartupState::ForeignConflict:
        if (status.definitionMatches) {
            return invalidStartupModel(
                "A drifted or foreign Manager startup entry cannot match the expected definition.");
        }
        break;
    }

    return Result<void>::success();
}

Result<void> validateManagerStartupOutcome(const ManagerStartupOutcome& outcome)
{
    auto validStatus = validateManagerStartupStatus(outcome.status);
    if (!validStatus) {
        return validStatus;
    }
    if (outcome.changed &&
        outcome.status.state == ManagerStartupState::ForeignConflict) {
        return invalidStartupModel(
            "A foreign Manager startup registration cannot be mutated.");
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
