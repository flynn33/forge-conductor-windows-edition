#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsRuntimeDiagnostics final : public Contracts::IRuntimeDiagnostics {
public:
    static constexpr std::size_t MaximumGenericOwnershipCount = 4'096U;
    static constexpr std::size_t MaximumProcessReaderCount =
        2U * Domain::MaximumConcurrentProcessOperations;

    WindowsRuntimeDiagnostics(
        Contracts::IClock& clock,
        Domain::ResourceBudgets budgets);
    ~WindowsRuntimeDiagnostics() noexcept override;

    WindowsRuntimeDiagnostics(const WindowsRuntimeDiagnostics&) = delete;
    WindowsRuntimeDiagnostics& operator=(const WindowsRuntimeDiagnostics&) = delete;
    WindowsRuntimeDiagnostics(WindowsRuntimeDiagnostics&&) = delete;
    WindowsRuntimeDiagnostics& operator=(WindowsRuntimeDiagnostics&&) = delete;

    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        Contracts::RuntimeOwnerKind kind,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    struct Control;

    [[nodiscard]] std::size_t capacityFor(
        Contracts::RuntimeOwnerKind kind) const noexcept;

    Contracts::IClock& clock_;
    Domain::ResourceBudgets budgets_;
    std::shared_ptr<Control> control_;
};

} // namespace ForgeConductor::Infrastructure::Windows
