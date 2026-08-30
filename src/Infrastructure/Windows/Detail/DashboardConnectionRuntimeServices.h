#pragma once

#include "DashboardBoundedMonotonicSequence.h"
#include "DashboardFixedIocpKeyAuthority.h"
#include "DashboardIoCompletionPort.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Atomic composition-owned observation used when a request leaves the IOCP
// boundary. The returned value is copied into the handler operation, so one
// request cannot observe a changing operational state while it is prepared.
class IDashboardOperationalStateSource {
public:
    virtual ~IDashboardOperationalStateSource() noexcept = default;

    [[nodiscard]] virtual bool operationalServiceActive() const noexcept = 0;
};

struct DashboardConnectionRuntimeIdentity final {
    std::uint64_t registrationId{};
    DashboardIoCompletionKey completionKey{0U};

    bool operator==(const DashboardConnectionRuntimeIdentity&) const = default;
};

// Move-only proof that one connection owns the runtime admission mutex for the
// few instructions that commit a prepared exchange to a delivery lifecycle.
// It must not be retained across scheduler, executor, socket, or callback work.
class DashboardRuntimeResponseAdmission final {
public:
    DashboardRuntimeResponseAdmission(
        const DashboardRuntimeResponseAdmission&) = delete;
    DashboardRuntimeResponseAdmission& operator=(
        const DashboardRuntimeResponseAdmission&) = delete;
    DashboardRuntimeResponseAdmission(
        DashboardRuntimeResponseAdmission&&) noexcept = default;
    DashboardRuntimeResponseAdmission& operator=(
        DashboardRuntimeResponseAdmission&&) = delete;
    ~DashboardRuntimeResponseAdmission() noexcept = default;

    [[nodiscard]] bool ownsAdmission() const noexcept
    {
        return lock_.owns_lock();
    }

private:
    friend class DashboardConnectionRuntimeServices;

    explicit DashboardRuntimeResponseAdmission(std::mutex& mutex)
        : lock_{mutex}
    {
    }

    std::unique_lock<std::mutex> lock_;
};

// Process-owned source of connection identities, time observations, and
// bounded handler contexts. Identities are issued in strictly increasing
// order and are never returned to either sequence. Completion key zero is not
// dynamically issued, the IOCP shutdown key is always reserved, and all
// caller-supplied fixed keys are skipped permanently.
class DashboardConnectionRuntimeServices final {
public:
    static constexpr std::size_t MaximumFixedCompletionKeyCount = 32U;
    static constexpr auto MaximumOperationLifetime =
        std::chrono::seconds{5};

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardConnectionRuntimeServices>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<IDashboardOperationalStateSource> operationalState,
        const DashboardFixedIocpKeyAuthority& fixedKeyAuthority) noexcept;

    // Injectable seam for sequence-boundary and reserved-key tests.
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<DashboardConnectionRuntimeServices>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<IDashboardOperationalStateSource> operationalState,
        std::span<const DashboardIoCompletionKey> fixedCompletionKeys = {})
        noexcept;

    ~DashboardConnectionRuntimeServices() noexcept = default;

    DashboardConnectionRuntimeServices(
        const DashboardConnectionRuntimeServices&) = delete;
    DashboardConnectionRuntimeServices& operator=(
        const DashboardConnectionRuntimeServices&) = delete;
    DashboardConnectionRuntimeServices(
        DashboardConnectionRuntimeServices&&) = delete;
    DashboardConnectionRuntimeServices& operator=(
        DashboardConnectionRuntimeServices&&) = delete;

    [[nodiscard]] Domain::Result<DashboardConnectionRuntimeIdentity>
    allocateConnectionIdentity() noexcept;

    // Couples a fresh registration identity to one key reserved by this
    // runtime's fixed-key authority. This is identity issuance, not a key
    // lease: production listener callers must already own one exact A/B slot.
    // The fixed completion key may be reused by a later fully drained owner;
    // its registration identity is never reused.
    [[nodiscard]] Domain::Result<DashboardConnectionRuntimeIdentity>
    allocateFixedIdentity(
        DashboardIoCompletionKey completionKey) noexcept;

    // Issues registration ownership for an auxiliary target that does not
    // require a dynamically allocated IOCP completion key.
    [[nodiscard]] Domain::Result<std::uint64_t>
    allocateAuxiliaryRegistrationId() noexcept;

    // Linearizes only the allocation-free state transition that makes an
    // immutable response eligible for graceful delivery. The returned guard
    // holds admission until destroyed.
    [[nodiscard]] Domain::Result<DashboardRuntimeResponseAdmission>
    acquireResponseAdmission() noexcept;

    // Atomically closes every source of new runtime-owned work. Existing
    // identities and operation contexts remain valid for their bounded
    // lifetimes; no identifier is returned to either sequence.
    void beginShutdown() noexcept;

    [[nodiscard]] bool isShuttingDown() const noexcept;

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept;

    [[nodiscard]] bool operationalServiceActive() const noexcept;

    // Produces a fresh operation and correlation identity. The deadline is
    // min(now + five seconds, absoluteDeadlineCeiling). Already-cancelled or
    // expired requests are rejected rather than creating unusable work.
    [[nodiscard]] Domain::Result<Domain::OperationContext>
    createOperationContext(
        Domain::MonotonicTimePoint absoluteDeadlineCeiling,
        std::stop_token cancellation = {}) noexcept;

private:
    DashboardConnectionRuntimeServices(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<IDashboardOperationalStateSource> operationalState,
        std::vector<std::uintptr_t> fixedCompletionKeys) noexcept;

    [[nodiscard]] bool isReservedCompletionKey(
        std::uintptr_t value) const noexcept;
    [[nodiscard]] bool isFixedCompletionKey(
        std::uintptr_t value) const noexcept;

    const std::shared_ptr<Contracts::IClock> clock_;
    const std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    const std::shared_ptr<IDashboardOperationalStateSource> operationalState_;
    const std::vector<std::uintptr_t> fixedCompletionKeys_;

    mutable std::mutex identityMutex_;
    DashboardBoundedMonotonicSequence<std::uint64_t>
        registrationIds_{1U};
    DashboardBoundedMonotonicSequence<std::uintptr_t>
        completionKeys_{1U};
    bool shutdown_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
