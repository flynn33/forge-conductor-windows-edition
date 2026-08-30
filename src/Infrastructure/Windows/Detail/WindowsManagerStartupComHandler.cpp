#include "Infrastructure/Windows/Detail/WindowsManagerStartupComHandler.h"

#include "Infrastructure/Windows/Detail/IWindowsTaskSchedulerStartupPlatform.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] ManagerStartupComResult failure(
    const std::string_view code,
    std::string message)
{
    return ManagerStartupComResult::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] ManagerStartupComResult statusResponse(
    Domain::ManagerStartupStatus status)
{
    auto valid = Domain::validateManagerStartupStatus(status);
    if (!valid) {
        return ManagerStartupComResult::failure(std::move(valid).error());
    }
    return ManagerStartupComResult::success(ManagerStartupComResponse{
        std::in_place_type<Domain::ManagerStartupStatus>,
        std::move(status)});
}

[[nodiscard]] ManagerStartupComResult outcomeResponse(
    Domain::ManagerStartupStatus status,
    const bool changed)
{
    Domain::ManagerStartupOutcome outcome{std::move(status), changed};
    auto valid = Domain::validateManagerStartupOutcome(outcome);
    if (!valid) {
        return ManagerStartupComResult::failure(std::move(valid).error());
    }
    return ManagerStartupComResult::success(ManagerStartupComResponse{
        std::in_place_type<Domain::ManagerStartupOutcome>,
        std::move(outcome)});
}

[[nodiscard]] Domain::Result<Domain::ManagerStartupStatus> inspectStatus(
    IWindowsTaskSchedulerStartupPlatform& platform,
    const ManagerStartupResolvedRegistration& registration,
    const Domain::OperationContext& context)
{
    auto observed = platform.inspect(registration, context);
    if (!observed) {
        return Domain::Result<Domain::ManagerStartupStatus>::failure(
            std::move(observed).error());
    }
    return Manager::ManagerStartupTaskPolicy::classify(
        registration.definition,
        observed.value());
}

[[nodiscard]] ManagerStartupComResult mutationFailure(
    Domain::Result<void> result)
{
    return ManagerStartupComResult::failure(std::move(result).error());
}

[[nodiscard]] ManagerStartupComResult unexpectedPostcondition(
    const std::string_view operation)
{
    return failure(
        Domain::ErrorCodes::IntegrityFailure,
        "The Manager startup task did not satisfy the exact postcondition after " +
            std::string{operation} + ".");
}

[[nodiscard]] bool isExactState(
    const Domain::ManagerStartupStatus& status,
    const bool enabled) noexcept
{
    return status.state ==
        (enabled
             ? Domain::ManagerStartupState::Ready
             : Domain::ManagerStartupState::Disabled);
}

} // namespace

WindowsManagerStartupComHandler::WindowsManagerStartupComHandler(
    std::shared_ptr<IWindowsTaskSchedulerStartupPlatform> platform)
    : platform_{std::move(platform)}
{
}

ManagerStartupComResult WindowsManagerStartupComHandler::handle(
    const ManagerStartupComRequest& request) noexcept
{
    try {
        if (!platform_) {
            return failure(
                Domain::ErrorCodes::IntegrityFailure,
                "The Windows Manager startup handler has no Task Scheduler platform.");
        }

        auto registration = platform_->resolve(
            request.expected,
            request.purposeSuffix,
            request.context);
        if (!registration) {
            return ManagerStartupComResult::failure(
                std::move(registration).error());
        }

        auto before = inspectStatus(
            *platform_, registration.value(), request.context);
        if (!before) {
            return ManagerStartupComResult::failure(std::move(before).error());
        }

        if (request.kind == ManagerStartupComOperationKind::Inspect) {
            return statusResponse(std::move(before).value());
        }

        const auto unchanged = [&before]() {
            return outcomeResponse(std::move(before).value(), false);
        };
        const auto inspectAfter = [this, &registration, &request]() {
            return inspectStatus(
                *platform_, registration.value(), request.context);
        };
        const auto foreign = before.value().state ==
            Domain::ManagerStartupState::ForeignConflict;

        switch (request.kind) {
        case ManagerStartupComOperationKind::Inspect:
            return failure(
                Domain::ErrorCodes::InternalFailure,
                "The Manager startup inspect operation was dispatched twice.");

        case ManagerStartupComOperationKind::Register:
            if (foreign) {
                return unchanged();
            }
            if (before.value().state == Domain::ManagerStartupState::Drifted) {
                return failure(
                    Domain::ErrorCodes::Conflict,
                    "The owned Manager startup task has drifted; explicit repair is required before registration can be enabled.");
            }
            if (before.value().state == Domain::ManagerStartupState::Ready) {
                return unchanged();
            }
            if (before.value().state == Domain::ManagerStartupState::Missing) {
                auto mutated = platform_->registerCanonical(
                    registration.value(),
                    ManagerStartupRegistrationMutation::CreateMissing,
                    true,
                    request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
            } else {
                auto mutated = platform_->setEnabled(
                    registration.value(), true, request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
            }
            {
                auto after = inspectAfter();
                if (!after) {
                    return ManagerStartupComResult::failure(
                        std::move(after).error());
                }
                if (!isExactState(after.value(), true)) {
                    return unexpectedPostcondition("registration");
                }
                return outcomeResponse(std::move(after).value(), true);
            }

        case ManagerStartupComOperationKind::Repair:
            if (foreign ||
                before.value().state == Domain::ManagerStartupState::Ready ||
                before.value().state == Domain::ManagerStartupState::Disabled) {
                return unchanged();
            }
            {
                const bool repairEnabled =
                    before.value().state == Domain::ManagerStartupState::Missing
                        ? true
                        : before.value().enabled;
                auto mutated = platform_->registerCanonical(
                    registration.value(),
                    before.value().state == Domain::ManagerStartupState::Missing
                        ? ManagerStartupRegistrationMutation::CreateMissing
                        : ManagerStartupRegistrationMutation::ReplaceOwned,
                    repairEnabled,
                    request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
                auto after = inspectAfter();
                if (!after) {
                    return ManagerStartupComResult::failure(
                        std::move(after).error());
                }
                if (!isExactState(after.value(), repairEnabled)) {
                    return unexpectedPostcondition("repair");
                }
                return outcomeResponse(std::move(after).value(), true);
            }

        case ManagerStartupComOperationKind::SetEnabled:
            if (foreign ||
                before.value().state == Domain::ManagerStartupState::Missing) {
                return unchanged();
            }
            if (before.value().state == Domain::ManagerStartupState::Drifted &&
                request.enabled) {
                return failure(
                    Domain::ErrorCodes::Conflict,
                    "A drifted Manager startup task cannot be enabled before repair.");
            }
            if (before.value().enabled == request.enabled) {
                return unchanged();
            }
            {
                auto mutated = platform_->setEnabled(
                    registration.value(), request.enabled, request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
                auto after = inspectAfter();
                if (!after) {
                    return ManagerStartupComResult::failure(
                        std::move(after).error());
                }
                const bool acceptable =
                    before.value().state == Domain::ManagerStartupState::Drifted
                        ? after.value().state ==
                              Domain::ManagerStartupState::Drifted &&
                              !after.value().enabled
                        : isExactState(after.value(), request.enabled);
                if (!acceptable) {
                    return unexpectedPostcondition("the enablement change");
                }
                return outcomeResponse(std::move(after).value(), true);
            }

        case ManagerStartupComOperationKind::StartNow:
            if (foreign) {
                return unchanged();
            }
            if (before.value().state != Domain::ManagerStartupState::Ready) {
                return failure(
                    Domain::ErrorCodes::Conflict,
                    "Only an exact enabled Manager startup task can be started.");
            }
            {
                auto mutated = platform_->startNow(
                    registration.value(), request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
                auto after = inspectAfter();
                if (!after) {
                    return ManagerStartupComResult::failure(
                        std::move(after).error());
                }
                if (!isExactState(after.value(), true)) {
                    return unexpectedPostcondition("the on-demand start");
                }
                return outcomeResponse(std::move(after).value(), true);
            }

        case ManagerStartupComOperationKind::Remove:
            if (foreign ||
                before.value().state == Domain::ManagerStartupState::Missing) {
                return unchanged();
            }
            {
                auto mutated = platform_->remove(
                    registration.value(), request.context);
                if (!mutated) {
                    return mutationFailure(std::move(mutated));
                }
                auto after = inspectAfter();
                if (!after) {
                    return ManagerStartupComResult::failure(
                        std::move(after).error());
                }
                if (after.value().state != Domain::ManagerStartupState::Missing) {
                    return unexpectedPostcondition("removal");
                }
                return outcomeResponse(std::move(after).value(), true);
            }
        }

        return failure(
            Domain::ErrorCodes::InvalidRequest,
            "The Manager startup operation kind is not recognized.");
    } catch (const std::exception& exception) {
        return failure(
            Domain::ErrorCodes::InternalFailure,
            std::string{"The Windows Manager startup handler failed: "} +
                exception.what());
    } catch (...) {
        return failure(
            Domain::ErrorCodes::InternalFailure,
            "The Windows Manager startup handler failed with an unknown exception.");
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
