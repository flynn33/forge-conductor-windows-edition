#pragma once

#include <cstdint>
#include <string_view>

namespace ForgeConductor::Dashboard {

enum class DashboardRouteId : std::uint8_t {
    Unknown,
    ShellRoot,
    ShellIndex,
    ShellControl,
    ShellManager,
    StaticResource,
    TelemetryHealth,
    TelemetryLive,
    TelemetryFrame,
    TelemetrySnapshot,
    TelemetrySystem,
    TelemetryForge,
    TelemetryPing,
    Status,
    TelemetryStream,
    OperationalDoctor,
    OperationalAgents,
    OperationalSessions,
    OperationalAudit,
    OperationalDiagnostics,
    OperationalSessionsPrune,
    OperationalSessionsClose,
    ManagerStatus,
    ManagerSettings,
    ManagerStart,
    ManagerStop,
    ManagerRestart,
    ManagerShutdown,
    UnavailableToolsCall,
};

enum class DashboardRouteSurface : std::uint8_t {
    Unknown,
    Shell,
    StaticResource,
    Telemetry,
    Status,
    ServerSentEvents,
    OperationalRead,
    OperationalWrite,
    ManagerRead,
    ManagerWrite,
    ManagerReadWrite,
    Unavailable,
};

enum class DashboardRouteDisposition : std::uint8_t {
    Dispatch,
    MethodNotAllowed,
    ServiceStopped,
    NotFound,
    Unavailable,
};

enum class DashboardRouteQuery : std::uint8_t {
    None,
    StreamHertz,
    LegacyStreamInterval,
    UnsupportedStreamQuery,
};

enum class DashboardServiceRequirement : std::uint8_t {
    Independent,
    OperationalService,
};

enum class DashboardAllowedMethods : std::uint8_t {
    None = 0U,
    Get = 1U,
    Post = 2U,
    Put = 4U,
};

[[nodiscard]] constexpr DashboardAllowedMethods operator|(
    const DashboardAllowedMethods left,
    const DashboardAllowedMethods right) noexcept
{
    return static_cast<DashboardAllowedMethods>(
        static_cast<std::uint8_t>(left) |
        static_cast<std::uint8_t>(right));
}

// Immutable routing decision produced after HTTP parsing and request policy.
// It borrows no request storage and owns no transport or platform resources.
class DashboardRouteMatch final {
public:
    [[nodiscard]] constexpr DashboardRouteId id() const noexcept
    {
        return id_;
    }

    [[nodiscard]] constexpr DashboardRouteSurface surface() const noexcept
    {
        return surface_;
    }

    [[nodiscard]] constexpr DashboardAllowedMethods allowedMethods() const noexcept
    {
        return allowedMethods_;
    }

    [[nodiscard]] constexpr DashboardRouteQuery query() const noexcept
    {
        return query_;
    }

    [[nodiscard]] constexpr DashboardServiceRequirement serviceRequirement()
        const noexcept
    {
        return serviceRequirement_;
    }

    [[nodiscard]] constexpr DashboardRouteDisposition disposition() const noexcept
    {
        return disposition_;
    }

    [[nodiscard]] bool allowsMethod(std::string_view method) const noexcept;

    [[nodiscard]] constexpr bool dispatchable() const noexcept
    {
        return disposition_ == DashboardRouteDisposition::Dispatch;
    }

    bool operator==(const DashboardRouteMatch&) const = default;

private:
    friend class DashboardRouteCatalog;

    constexpr DashboardRouteMatch(
        const DashboardRouteId id,
        const DashboardRouteSurface surface,
        const DashboardAllowedMethods allowedMethods,
        const DashboardRouteQuery query,
        const DashboardServiceRequirement serviceRequirement,
        const DashboardRouteDisposition disposition) noexcept
        : id_{id},
          surface_{surface},
          allowedMethods_{allowedMethods},
          query_{query},
          serviceRequirement_{serviceRequirement},
          disposition_{disposition}
    {
    }

    DashboardRouteId id_;
    DashboardRouteSurface surface_;
    DashboardAllowedMethods allowedMethods_;
    DashboardRouteQuery query_;
    DashboardServiceRequirement serviceRequirement_;
    DashboardRouteDisposition disposition_;
};

// Classifies the already-validated origin-form request target without decoding it.
// Only the first raw '?' separates path identity from a query suffix.
class DashboardRouteCatalog final {
public:
    [[nodiscard]] static DashboardRouteMatch classify(
        std::string_view method,
        std::string_view target,
        bool operationalServiceActive) noexcept;
};

} // namespace ForgeConductor::Dashboard
