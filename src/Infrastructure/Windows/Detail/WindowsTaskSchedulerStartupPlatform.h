#pragma once

#include "IWindowsTaskSchedulerStartupPlatform.h"

namespace ForgeConductor::Infrastructure::Windows::Detail {

class WindowsTaskSchedulerStartupPlatform final
    : public IWindowsTaskSchedulerStartupPlatform {
public:
    [[nodiscard]] Domain::Result<ManagerStartupResolvedRegistration> resolve(
        const Domain::ManagerStartupDefinition& expected,
        std::string_view purposeSuffix,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Manager::ManagerStartupTaskObservation> inspect(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> registerCanonical(
        const ManagerStartupResolvedRegistration& registration,
        ManagerStartupRegistrationMutation mutation,
        bool enabled,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> setEnabled(
        const ManagerStartupResolvedRegistration& registration,
        bool enabled,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> startNow(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> remove(
        const ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext& context) noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
