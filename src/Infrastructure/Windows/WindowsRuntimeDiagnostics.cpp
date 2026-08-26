#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"

#include <array>
#include <mutex>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::size_t OwnerKindCount = 9U;

[[nodiscard]] constexpr std::size_t ownerIndex(const Contracts::RuntimeOwnerKind kind) noexcept
{
    return static_cast<std::size_t>(kind);
}

static_assert(ownerIndex(Contracts::RuntimeOwnerKind::OpenDatabase) + 1U == OwnerKindCount);

template <typename T>
[[nodiscard]] Domain::Result<T> contextFailure(const Domain::OperationContext& context,
                                               const Domain::MonotonicTimePoint now,
                                               const bool shutdown)
{
    if (shutdown || context.isCancellationRequested()) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled, "Runtime diagnostics operation was cancelled."));
    }
    if (context.isExpired(now)) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded, "Runtime diagnostics deadline expired."));
    }
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure,
                          "Runtime diagnostics context failure was not classified."));
}

} // namespace

struct WindowsRuntimeDiagnostics::Control final {
    std::mutex mutex;
    std::array<std::size_t, OwnerKindCount> counts{};
    bool shutdown{};
};

WindowsRuntimeDiagnostics::WindowsRuntimeDiagnostics(Contracts::IClock& clock,
                                                     Domain::ResourceBudgets budgets)
    : clock_{clock}, budgets_{std::move(budgets)}, control_{std::make_shared<Control>()}
{
}

WindowsRuntimeDiagnostics::~WindowsRuntimeDiagnostics() noexcept { shutdown(); }

std::size_t
WindowsRuntimeDiagnostics::capacityFor(const Contracts::RuntimeOwnerKind kind) const noexcept
{
    switch (kind) {
    case Contracts::RuntimeOwnerKind::OwnedOperation:
    case Contracts::RuntimeOwnerKind::PendingCallback:
    case Contracts::RuntimeOwnerKind::ActiveTimer:
        return MaximumGenericOwnershipCount;
    case Contracts::RuntimeOwnerKind::BackgroundThread:
        return budgets_.managerThreadsMaximum;
    case Contracts::RuntimeOwnerKind::OpenRepository:
    case Contracts::RuntimeOwnerKind::OpenDatabase:
        return budgets_.openProjectRepositoriesMaximum;
    case Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot:
        return budgets_.telemetryPendingSnapshotsMaximum;
    case Contracts::RuntimeOwnerKind::ChildProcess:
        return Domain::MaximumConcurrentProcessOperations;
    case Contracts::RuntimeOwnerKind::ProcessReader:
        return MaximumProcessReaderCount;
    }
    return 0U;
}

Domain::Result<Contracts::RuntimeOwnershipLease>
WindowsRuntimeDiagnostics::acquire(const Contracts::RuntimeOwnerKind kind,
                                   const Domain::OperationContext& context) noexcept
{
    try {
        const auto now = clock_.monotonicNow();
        {
            std::lock_guard lock{control_->mutex};
            if (control_->shutdown || context.isCancellationRequested() || context.isExpired(now)) {
                return contextFailure<Contracts::RuntimeOwnershipLease>(context, now,
                                                                        control_->shutdown);
            }
        }

        const auto index = ownerIndex(kind);
        const auto capacity = capacityFor(kind);
        if (index >= OwnerKindCount || capacity == 0U) {
            return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "Runtime ownership kind has no configured capacity."));
        }

        {
            std::lock_guard lock{control_->mutex};
            const auto admissionNow = clock_.monotonicNow();
            if (control_->shutdown || context.isCancellationRequested() ||
                context.isExpired(admissionNow)) {
                return contextFailure<Contracts::RuntimeOwnershipLease>(context, admissionNow,
                                                                        control_->shutdown);
            }
            if (control_->counts[index] >= capacity) {
                return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded, "Runtime ownership capacity is exhausted."));
            }
            ++control_->counts[index];
        }

        try {
            std::weak_ptr<Control> weakControl{control_};
            return Domain::Result<Contracts::RuntimeOwnershipLease>::success(
                issueOwnershipLease([weakControl, index]() noexcept {
                    if (const auto control = weakControl.lock()) {
                        std::lock_guard lock{control->mutex};
                        if (control->counts[index] > 0U) {
                            --control->counts[index];
                        }
                    }
                }));
        } catch (...) {
            std::lock_guard lock{control_->mutex};
            if (control_->counts[index] > 0U) {
                --control_->counts[index];
            }
            throw;
        }
    } catch (...) {
        return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Runtime ownership acquisition failed."));
    }
}

Domain::Result<Domain::RuntimeDiagnosticSnapshot>
WindowsRuntimeDiagnostics::snapshot(const Domain::OperationContext& context) noexcept
{
    try {
        const auto now = clock_.monotonicNow();
        std::array<std::size_t, OwnerKindCount> counts{};
        {
            std::lock_guard lock{control_->mutex};
            if (control_->shutdown || context.isCancellationRequested() || context.isExpired(now)) {
                return contextFailure<Domain::RuntimeDiagnosticSnapshot>(context, now,
                                                                         control_->shutdown);
            }
            counts = control_->counts;
        }

        auto pressure = Domain::ResourcePressureLevel::Nominal;
        for (std::size_t index = 0U; index < counts.size(); ++index) {
            const auto kind = static_cast<Contracts::RuntimeOwnerKind>(index);
            const auto capacity = capacityFor(kind);
            if (capacity == 0U) {
                continue;
            }
            if (counts[index] >= capacity) {
                pressure = Domain::ResourcePressureLevel::Critical;
                break;
            }
            if (counts[index] >= ((capacity * 4U) / 5U)) {
                pressure = Domain::ResourcePressureLevel::Warning;
            }
        }

        return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::success(
            Domain::RuntimeDiagnosticSnapshot{
                clock_.utcNow(), counts[ownerIndex(Contracts::RuntimeOwnerKind::OwnedOperation)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::PendingCallback)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::BackgroundThread)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::OpenRepository)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot)], pressure,
                counts[ownerIndex(Contracts::RuntimeOwnerKind::ActiveTimer)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::ChildProcess)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::ProcessReader)],
                counts[ownerIndex(Contracts::RuntimeOwnerKind::OpenDatabase)]});
    } catch (...) {
        return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Runtime diagnostics snapshot failed."));
    }
}

void WindowsRuntimeDiagnostics::shutdown() noexcept
{
    try {
        std::lock_guard lock{control_->mutex};
        control_->shutdown = true;
    } catch (...) {
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
