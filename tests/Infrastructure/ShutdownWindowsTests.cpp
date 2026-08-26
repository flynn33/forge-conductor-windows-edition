#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

class FixedClock final : public Contracts::IClock {
public:
    explicit FixedClock(const Domain::MonotonicTimePoint monotonic) noexcept : monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{std::chrono::seconds{1'787'650'000}};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic_;
    }

private:
    Domain::MonotonicTimePoint monotonic_;
};

class DeadlineAdvancingClock final : public Contracts::IClock {
public:
    DeadlineAdvancingClock(const Domain::MonotonicTimePoint first,
                           const Domain::MonotonicTimePoint second) noexcept
        : first_{first}, second_{second}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{std::chrono::seconds{1'787'650'000}};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return reads_++ == 0U ? first_ : second_;
    }

private:
    Domain::MonotonicTimePoint first_;
    Domain::MonotonicTimePoint second_;
    mutable std::size_t reads_{};
};

void requireAllZero(const Domain::RuntimeDiagnosticSnapshot& snapshot)
{
    require(snapshot.ownedOperations == 0U, "owned operations leaked");
    require(snapshot.pendingCallbacks == 0U, "pending callbacks leaked");
    require(snapshot.backgroundThreads == 0U, "background threads leaked");
    require(snapshot.openRepositories == 0U, "repositories leaked");
    require(snapshot.telemetryPendingSnapshots == 0U, "telemetry leases leaked");
    require(snapshot.activeTimers == 0U, "timers leaked");
    require(snapshot.childProcesses == 0U, "child processes leaked");
    require(snapshot.processReaders == 0U, "process readers leaked");
    require(snapshot.openDatabases == 0U, "databases leaked");
}

void ownershipMovesAndReleasesExactlyOnce()
{
    TestContext context;
    FixedClock clock{context.now};
    Infrastructure::Windows::WindowsRuntimeDiagnostics diagnostics{
        clock, Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)};

    constexpr std::array kinds{Contracts::RuntimeOwnerKind::OwnedOperation,
                               Contracts::RuntimeOwnerKind::PendingCallback,
                               Contracts::RuntimeOwnerKind::BackgroundThread,
                               Contracts::RuntimeOwnerKind::OpenRepository,
                               Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot,
                               Contracts::RuntimeOwnerKind::ActiveTimer,
                               Contracts::RuntimeOwnerKind::ChildProcess,
                               Contracts::RuntimeOwnerKind::ProcessReader,
                               Contracts::RuntimeOwnerKind::OpenDatabase};
    std::vector<Contracts::RuntimeOwnershipLease> leases;
    leases.reserve(kinds.size());
    for (const auto kind : kinds) {
        leases.push_back(take(diagnostics.acquire(kind, context.active())));
    }
    const auto occupied = take(diagnostics.snapshot(context.active()));
    require(occupied.ownedOperations == 1U, "owned operation was not counted");
    require(occupied.pendingCallbacks == 1U, "callback was not counted");
    require(occupied.backgroundThreads == 1U, "thread was not counted");
    require(occupied.openRepositories == 1U, "repository was not counted");
    require(occupied.telemetryPendingSnapshots == 1U, "telemetry snapshot was not counted");
    require(occupied.activeTimers == 1U, "timer was not counted");
    require(occupied.childProcesses == 1U, "child process was not counted");
    require(occupied.processReaders == 1U, "process reader was not counted");
    require(occupied.openDatabases == 1U, "database was not counted");

    auto moved = std::move(leases.front());
    require(moved.active(), "moved ownership lease became inactive");
    require(!leases.front().active(), "moved-from ownership lease stayed active");
    moved.reset();
    moved.reset();
    leases.clear();
    requireAllZero(take(diagnostics.snapshot(context.active())));
}

void fixedCapacitiesAndShutdownFailClosed()
{
    TestContext context;
    const auto admissionStart = context.now;
    DeadlineAdvancingClock advancingClock{admissionStart, admissionStart + std::chrono::seconds{2}};
    Infrastructure::Windows::WindowsRuntimeDiagnostics racingDiagnostics{
        advancingClock, Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)};
    const Domain::OperationContext expiringDuringAdmission{
        context.operationId, admissionStart + std::chrono::seconds{1}, {}, context.correlationId};
    requireError(racingDiagnostics.acquire(Contracts::RuntimeOwnerKind::OwnedOperation,
                                           expiringDuringAdmission),
                 Domain::ErrorCodes::DeadlineExceeded,
                 "runtime ownership ignored a deadline expiring at admission linearization");

    FixedClock clock{context.now};
    Infrastructure::Windows::WindowsRuntimeDiagnostics diagnostics{
        clock, Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)};

    auto telemetry = take(diagnostics.acquire(Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot,
                                              context.active()));
    requireError(diagnostics.acquire(Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot,
                                     context.active()),
                 Domain::ErrorCodes::LimitExceeded,
                 "telemetry admitted more than one pending snapshot");
    telemetry.reset();

    std::vector<Contracts::RuntimeOwnershipLease> processes;
    processes.reserve(Domain::MaximumConcurrentProcessOperations);
    for (std::size_t index = 0U; index < Domain::MaximumConcurrentProcessOperations; ++index) {
        processes.push_back(
            take(diagnostics.acquire(Contracts::RuntimeOwnerKind::ChildProcess, context.active())));
    }
    requireError(diagnostics.acquire(Contracts::RuntimeOwnerKind::ChildProcess, context.active()),
                 Domain::ErrorCodes::LimitExceeded,
                 "runtime ownership admitted a 65th child process");
    processes.clear();
    requireAllZero(take(diagnostics.snapshot(context.active())));

    diagnostics.shutdown();
    requireError(diagnostics.acquire(Contracts::RuntimeOwnerKind::OwnedOperation, context.active()),
                 Domain::ErrorCodes::Cancelled,
                 "runtime diagnostics admitted ownership after shutdown");
    requireError(diagnostics.snapshot(context.active()), Domain::ErrorCodes::Cancelled,
                 "runtime diagnostics exposed a snapshot after shutdown");
}

void leasesNeverProlongRegistryLifetime()
{
    TestContext context;
    FixedClock clock{context.now};
    std::optional<Contracts::RuntimeOwnershipLease> survivor;
    {
        Infrastructure::Windows::WindowsRuntimeDiagnostics diagnostics{
            clock, Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)};
        survivor.emplace(take(
            diagnostics.acquire(Contracts::RuntimeOwnerKind::OwnedOperation, context.active())));
        diagnostics.shutdown();
    }
    require(survivor->active(), "registry destruction unexpectedly mutated the lease object");
    survivor.reset();
}

} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    const TestRegistry tests{
        {"runtime.moves-and-zero", ownershipMovesAndReleasesExactlyOnce},
        {"runtime.capacities-and-shutdown", fixedCapacitiesAndShutdownFailClosed},
        {"runtime.weak-lifetime", leasesNeverProlongRegistryLifetime}};
    std::size_t passed = 0U;
    for (const auto& [name, run] : tests) {
        try {
            run();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
            return 1;
        }
    }
    std::cout << passed << '/' << tests.size() << " shutdown and ownership tests passed.\n";
    return 0;
}
