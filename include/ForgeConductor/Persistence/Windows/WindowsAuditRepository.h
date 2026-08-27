#pragma once

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Persistence::Windows {

class WindowsCentralDatabase;

// Central-store audit implementation. The repository owns only its admission;
// the composition root retains and closes the shared central database after all
// MCP work has drained.
class WindowsAuditRepository final : public Contracts::IAuditRepository {
public:
    static constexpr std::size_t MaximumRecentEvents = 200U;
    static constexpr std::size_t MaximumRetainedEvents = 10'000U;

    [[nodiscard]] static Domain::Result<std::shared_ptr<WindowsAuditRepository>>
    attach(std::shared_ptr<WindowsCentralDatabase> database) noexcept;

    ~WindowsAuditRepository() noexcept override;

    WindowsAuditRepository(const WindowsAuditRepository&) = delete;
    WindowsAuditRepository& operator=(const WindowsAuditRepository&) = delete;
    WindowsAuditRepository(WindowsAuditRepository&&) = delete;
    WindowsAuditRepository& operator=(WindowsAuditRepository&&) = delete;

    [[nodiscard]] Domain::Result<void> append(
        const Domain::AuditEvent& event,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<Domain::AuditEvent>> recent(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsAuditRepository(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
