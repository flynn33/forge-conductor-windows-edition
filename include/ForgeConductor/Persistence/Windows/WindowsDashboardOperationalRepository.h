#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ForgeConductor::Persistence::Windows {

// Repository-side allocation ceilings mirror the released dashboard response
// budgets without introducing a Persistence -> Dashboard dependency. They are
// enforced while rows are decoded, before an over-bound projection is retained.
struct WindowsDashboardOperationalLimits final {
    static constexpr std::size_t MaximumSessionTextBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumPresenceTextBytes = 512U * 1024U;
};

// Persistence-neutral representation of one canonical client-presence row.
// The Manager application adapter maps this value into its dashboard model.
struct WindowsDashboardPresenceProjection final {
    std::string clientId;
    std::string hostKind;
    std::uint32_t processId{};
    Domain::PathText workingDirectory;
    Domain::UtcTimePoint lastHeartbeat;
};

// One consistent central-database observation. Open sessions are complete up
// to the caller's maximum; recent sessions are the intentional newest-N view.
struct WindowsDashboardOperationalProjection final {
    std::vector<Domain::AgentSession> openSessions;
    std::vector<Domain::AgentSession> recentSessions;
    std::vector<WindowsDashboardPresenceProjection> presence;
};

class WindowsDashboardOperationalRepository final {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsDashboardOperationalRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database) noexcept;

    ~WindowsDashboardOperationalRepository() noexcept;

    WindowsDashboardOperationalRepository(
        const WindowsDashboardOperationalRepository&) = delete;
    WindowsDashboardOperationalRepository& operator=(
        const WindowsDashboardOperationalRepository&) = delete;
    WindowsDashboardOperationalRepository(
        WindowsDashboardOperationalRepository&&) = delete;
    WindowsDashboardOperationalRepository& operator=(
        WindowsDashboardOperationalRepository&&) = delete;

    [[nodiscard]] Domain::Result<WindowsDashboardOperationalProjection> snapshot(
        std::size_t maximumOpenSessions,
        std::size_t maximumRecentSessions,
        std::size_t maximumPresenceRecords,
        const Domain::OperationContext& context) noexcept;

    void close() noexcept;

private:
    struct Impl;

    explicit WindowsDashboardOperationalRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
