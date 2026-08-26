#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/DiagnosticsModels.h"

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace ForgeConductor::Contracts {

class IDiagnosticSink {
public:
    virtual ~IDiagnosticSink() = default;

    [[nodiscard]] virtual Domain::Result<void> record(
        const Domain::DiagnosticEnvelope& event,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IAuditRepository {
public:
    virtual ~IAuditRepository() = default;

    [[nodiscard]] virtual Domain::Result<void> append(
        const Domain::AuditEvent& event,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::AuditEvent>> recent(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

class IDoctorService {
public:
    virtual ~IDoctorService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::DoctorReport> run(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

enum class RuntimeOwnerKind {
    OwnedOperation,
    PendingCallback,
    BackgroundThread,
    OpenRepository,
    TelemetryPendingSnapshot,
    ActiveTimer,
    ChildProcess,
    ProcessReader,
    OpenDatabase
};

class IRuntimeDiagnostics;

class RuntimeOwnershipLease final {
public:
    RuntimeOwnershipLease(const RuntimeOwnershipLease&) = delete;
    RuntimeOwnershipLease& operator=(const RuntimeOwnershipLease&) = delete;

    RuntimeOwnershipLease(RuntimeOwnershipLease&& other) noexcept
    {
        release_.swap(other.release_);
    }

    RuntimeOwnershipLease& operator=(RuntimeOwnershipLease&& other) noexcept
    {
        if (this != &other) {
            reset();
            release_.swap(other.release_);
        }
        return *this;
    }

    ~RuntimeOwnershipLease() noexcept
    {
        reset();
    }

    [[nodiscard]] bool active() const noexcept
    {
        return static_cast<bool>(release_);
    }

    void reset() noexcept
    {
        auto release = std::move(release_);
        release_ = nullptr;
        if (release) {
            try {
                release();
            } catch (...) {
            }
        }
    }

private:
    friend class IRuntimeDiagnostics;

    explicit RuntimeOwnershipLease(std::function<void()> release)
        : release_{std::move(release)}
    {
    }

    std::function<void()> release_;
};

class IRuntimeDiagnostics {
public:
    virtual ~IRuntimeDiagnostics() = default;

    [[nodiscard]] virtual Domain::Result<RuntimeOwnershipLease> acquire(
        RuntimeOwnerKind kind,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;

protected:
    [[nodiscard]] static RuntimeOwnershipLease issueOwnershipLease(
        std::function<void()> release)
    {
        return RuntimeOwnershipLease{std::move(release)};
    }
};

} // namespace ForgeConductor::Contracts
