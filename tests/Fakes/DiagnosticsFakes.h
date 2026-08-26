#pragma once

#include "BoundedFakeSupport.h"
#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

class DiagnosticSinkFake final : public Contracts::IDiagnosticSink {
public:
    explicit DiagnosticSinkFake(
        const std::size_t maximumEvents,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : maximumEvents_{maximumEvents}, gate_{now}
    {
    }

    DeterministicResult<Domain::DiagnosticExportResult> exportResult;

    [[nodiscard]] Domain::Result<void> record(
        const Domain::DiagnosticEnvelope& event,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastEvent_ = event;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            auto valid = Domain::validateDiagnosticEnvelope(event);
            if (!valid) {
                return valid;
            }
            if (events_.size() >= maximumEvents_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The deterministic diagnostic sink is at capacity."));
            }
            events_.push_back(event);
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastMaximumCount_ = maximumCount;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::vector<Domain::DiagnosticEnvelope>>(std::move(accepted));
            }
            const auto count = std::min(maximumCount, events_.size());
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(
                std::vector<Domain::DiagnosticEnvelope>{
                    events_.end() - static_cast<std::ptrdiff_t>(count),
                    events_.end()});
        } catch (...) {
            return fakeInternalFailure<std::vector<Domain::DiagnosticEnvelope>>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastExportRequest_ = request;
            lastExportAuthority_.emplace(authority);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::DiagnosticExportResult>(
                    std::move(accepted));
            }
            return exportResult.get();
        } catch (...) {
            return fakeInternalFailure<Domain::DiagnosticExportResult>();
        }
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }
    [[nodiscard]] std::size_t eventCount() const noexcept { return events_.size(); }

    [[nodiscard]] const std::optional<Domain::DiagnosticEnvelope>&
    lastEvent() const noexcept
    {
        return lastEvent_;
    }

    [[nodiscard]] const std::optional<Domain::DiagnosticExportRequest>&
    lastExportRequest() const noexcept
    {
        return lastExportRequest_;
    }

    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastExportAuthority() const noexcept
    {
        return lastExportAuthority_;
    }

private:
    const std::size_t maximumEvents_;
    std::vector<Domain::DiagnosticEnvelope> events_;
    std::optional<Domain::DiagnosticEnvelope> lastEvent_;
    std::optional<std::size_t> lastMaximumCount_;
    std::optional<Domain::DiagnosticExportRequest> lastExportRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastExportAuthority_;
    BoundedFakeOperationGate gate_;
};

class AuditRepositoryFake final : public Contracts::IAuditRepository {
public:
    explicit AuditRepositoryFake(
        const std::size_t maximumEvents,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : maximumEvents_{maximumEvents}, gate_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> append(
        const Domain::AuditEvent& event,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastEvent_ = event;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            if (events_.size() >= maximumEvents_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The deterministic audit repository is at capacity."));
            }
            events_.push_back(event);
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastMaximumCount_ = maximumCount;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<std::vector<Domain::AuditEvent>>(
                    std::move(accepted));
            }
            const auto count = std::min(maximumCount, events_.size());
            return Domain::Result<std::vector<Domain::AuditEvent>>::success(
                std::vector<Domain::AuditEvent>{
                    events_.end() - static_cast<std::ptrdiff_t>(count),
                    events_.end()});
        } catch (...) {
            return fakeInternalFailure<std::vector<Domain::AuditEvent>>();
        }
    }

    void close() noexcept override { gate_.close(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }
    [[nodiscard]] std::size_t eventCount() const noexcept { return events_.size(); }

    [[nodiscard]] const std::optional<Domain::AuditEvent>&
    lastEvent() const noexcept
    {
        return lastEvent_;
    }

private:
    const std::size_t maximumEvents_;
    std::vector<Domain::AuditEvent> events_;
    std::optional<Domain::AuditEvent> lastEvent_;
    std::optional<std::size_t> lastMaximumCount_;
    BoundedFakeOperationGate gate_;
};

class DoctorServiceFake final : public Contracts::IDoctorService {
public:
    explicit DoctorServiceFake(const Domain::MonotonicTimePoint now = {}) noexcept
        : gate_{now}
    {
    }

    DeterministicResult<Domain::DoctorReport> runResult;

    [[nodiscard]] Domain::Result<Domain::DoctorReport> run(
        const Domain::OperationContext& context) noexcept override
    {
        auto accepted = gate_.enter(context);
        if (!accepted) {
            return propagateFakeGateFailure<Domain::DoctorReport>(std::move(accepted));
        }
        try {
            return runResult.get();
        } catch (...) {
            return fakeInternalFailure<Domain::DoctorReport>();
        }
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return gate_.lastContext();
    }

private:
    BoundedFakeOperationGate gate_;
};

class RuntimeDiagnosticsFake final : public Contracts::IRuntimeDiagnostics {
public:
    explicit RuntimeDiagnosticsFake(const Domain::MonotonicTimePoint now = {})
        : gate_{now}, ownership_{std::make_shared<OwnershipState>()}
    {
    }

    DeterministicResult<Domain::RuntimeDiagnosticSnapshot> snapshotResult;

    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        const Contracts::RuntimeOwnerKind kind,
        const Domain::OperationContext& context) noexcept override
    {
        auto accepted = gate_.enter(context);
        if (!accepted) {
            return propagateFakeGateFailure<Contracts::RuntimeOwnershipLease>(
                std::move(accepted));
        }
        try {
            const auto index = ownerIndex(kind);
            {
                std::lock_guard lock{ownership_->mutex};
                if (ownership_->shutdown) {
                    return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Cancelled,
                            "The deterministic runtime registry is shut down."));
                }
                if (kind == Contracts::RuntimeOwnerKind::TelemetryPendingSnapshot &&
                    ownership_->counts[index] >= 1U) {
                    return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The deterministic telemetry ownership slot is full."));
                }
                ++ownership_->counts[index];
            }

            std::weak_ptr<OwnershipState> weakOwnership{ownership_};
            return Domain::Result<Contracts::RuntimeOwnershipLease>::success(
                issueOwnershipLease([weakOwnership, index]() noexcept {
                    if (const auto ownership = weakOwnership.lock()) {
                        std::lock_guard lock{ownership->mutex};
                        if (ownership->counts[index] > 0U) {
                            --ownership->counts[index];
                        }
                    }
                }));
        } catch (...) {
            return fakeInternalFailure<Contracts::RuntimeOwnershipLease>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override
    {
        auto accepted = gate_.enter(context);
        if (!accepted) {
            return propagateFakeGateFailure<Domain::RuntimeDiagnosticSnapshot>(
                std::move(accepted));
        }
        try {
            return snapshotResult.get();
        } catch (...) {
            return fakeInternalFailure<Domain::RuntimeDiagnosticSnapshot>();
        }
    }

    void shutdown() noexcept override
    {
        gate_.shutdown();
        try {
            std::lock_guard lock{ownership_->mutex};
            ownership_->shutdown = true;
        } catch (...) {
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return gate_.lastContext();
    }

    [[nodiscard]] std::size_t activeOwnership(
        const Contracts::RuntimeOwnerKind kind) const noexcept
    {
        try {
            std::lock_guard lock{ownership_->mutex};
            return ownership_->counts[ownerIndex(kind)];
        } catch (...) {
            return 0U;
        }
    }

private:
    static constexpr std::size_t OwnerKindCount = 9U;

    struct OwnershipState final {
        std::mutex mutex;
        std::array<std::size_t, OwnerKindCount> counts{};
        bool shutdown{};
    };

    [[nodiscard]] static constexpr std::size_t ownerIndex(
        const Contracts::RuntimeOwnerKind kind) noexcept
    {
        return static_cast<std::size_t>(kind);
    }

    static_assert(
        static_cast<std::size_t>(Contracts::RuntimeOwnerKind::OpenDatabase) + 1U ==
        OwnerKindCount);

    BoundedFakeOperationGate gate_;
    std::shared_ptr<OwnershipState> ownership_;
};

} // namespace ForgeConductor::Tests::Fakes
