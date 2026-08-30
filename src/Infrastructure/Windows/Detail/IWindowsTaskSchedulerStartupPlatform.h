#pragma once

#include "ManagerStartupDefinitionBuilder.h"
#include "ForgeConductor/Domain/ManagerStartupModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Manager/ManagerStartupTaskPolicy.h"

#include <memory>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class ManagerStartupRegistrationMutation {
    CreateMissing,
    ReplaceOwned
};

// All methods are invoked exclusively on ManagerStartupComWorker's MTA thread.
// The concrete implementation may therefore retain apartment-bound state only
// for the duration of a call and never expose COM types across this seam.
class IWindowsTaskSchedulerStartupPlatform {
public:
    virtual ~IWindowsTaskSchedulerStartupPlatform() = default;

    [[nodiscard]] virtual Domain::Result<ManagerStartupResolvedRegistration>
    resolve(
        const Domain::ManagerStartupDefinition& expected,
        std::string_view purposeSuffix,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Manager::ManagerStartupTaskObservation>
    inspect(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> registerCanonical(
        const ManagerStartupResolvedRegistration& registration,
        ManagerStartupRegistrationMutation mutation,
        bool enabled,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> setEnabled(
        const ManagerStartupResolvedRegistration& registration,
        bool enabled,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> startNow(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> remove(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<IWindowsTaskSchedulerStartupPlatform>
createWindowsTaskSchedulerStartupPlatform();

} // namespace ForgeConductor::Infrastructure::Windows::Detail
