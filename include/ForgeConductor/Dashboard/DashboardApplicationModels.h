#pragma once

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/DiagnosticsModels.h"
#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/TelemetryModels.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Dashboard {

// Closed bounds for the transport-neutral application projections. Concrete
// services reject an over-bound result with a typed error; they do not silently
// truncate a collection or string that has application meaning.
struct DashboardApplicationLimits final {
    static constexpr std::size_t MaximumProductBytes = 256U;
    static constexpr std::size_t MaximumVersionBytes = 128U;
    static constexpr std::size_t MaximumRuntimeBytes = 64U;
    static constexpr std::size_t MaximumDashboardHostBytes = 45U;
    static constexpr std::size_t MaximumIdentityTextBytes = 128U * 1024U;

    static constexpr std::size_t MaximumAgentSpecs = 4'096U;
    static constexpr std::size_t MaximumOpenSessions =
        Domain::AgentSessionLimits::MaximumSessionQueryRows;
    static constexpr std::size_t MaximumPresenceRecords = 256U;
    static constexpr std::size_t MaximumPresenceClientIdBytes = 256U;
    static constexpr std::size_t MaximumPresenceHostKindBytes = 64U;
    static constexpr std::size_t MaximumStatusAuditEvents = 20U;
    static constexpr std::size_t MaximumToolNames = 4'096U;
    static constexpr std::size_t MaximumToolNameBytes = 128U;

    static constexpr std::size_t MaximumRecentSessions = 40U;
    static constexpr std::size_t MaximumAuditEvents = 80U;
    static constexpr std::size_t MaximumDoctorChecks = 128U;
    static constexpr std::size_t MaximumDiagnosticLines = 100U;
    static constexpr std::size_t MaximumDiagnosticLineBytes = 16U * 1024U;

    // These aggregate text limits give adapters a second bound below the
    // collection counts. The JSON codec separately enforces the final encoded
    // response ceiling because escaping and structural overhead are data
    // dependent.
    static constexpr std::size_t MaximumAgentTextBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumSessionTextBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumPresenceTextBytes = 512U * 1024U;
    static constexpr std::size_t MaximumToolNameTextBytes = 512U * 1024U;
    static constexpr std::size_t MaximumAuditTextBytes = 512U * 1024U;
    static constexpr std::size_t MaximumDoctorTextBytes = 512U * 1024U;
    static constexpr std::size_t MaximumDiagnosticTextBytes = 512U * 1024U;
};

// Stable product and process identity supplied by the manager composition
// root. Paths are already validated PathText values and no platform handle is
// exposed across this boundary.
struct DashboardApplicationIdentity final {
    std::string product;
    std::string version;
    std::string runtime;
    Domain::PathText home;
    Domain::PathText store;
    std::uint32_t processId{};
};

// One immutable telemetry observation. A successful observation always owns a
// non-null snapshot. A missing measured rate is explicit and is never replaced
// with a synthetic zero.
struct DashboardTelemetryObservation final {
    std::shared_ptr<const Domain::TelemetrySnapshot> snapshot;
    std::optional<double> measuredSampleHz;
};

// Complete health projection for the independently owned telemetry source.
// targetSampleHz and measuredSampleHz are finite and nonnegative; the
// streamRunning bit determines whether the measured value represents a live
// producer.
struct DashboardTelemetryHealth final {
    Domain::TelemetryHealthReport report;
    double targetSampleHz{};
    double measuredSampleHz{};
    bool streamRunning{};
    Domain::RuntimeDiagnosticSnapshot runtimeDiagnostics;
    bool exportPresent{};
    bool staticPresent{};
    bool nodeAvailable{};
};

// Presentation-compatible presence identity. clientId remains text because
// the released macOS store admits composite presence identifiers in addition
// to UUID-shaped client identifiers.
struct DashboardPresenceRecord final {
    std::string clientId;
    std::string hostKind;
    std::uint32_t processId{};
    Domain::PathText workingDirectory;
    Domain::UtcTimePoint lastHeartbeat;
};

struct DashboardSessionListing final {
    std::vector<Domain::AgentSession> open;
    std::vector<Domain::AgentSession> recent;
};

// Operational-only status. Identity, telemetry, manager state, and listener-
// generation policy are independently injected so this read model remains
// available when one of those other surfaces is unavailable.
struct DashboardStatusData final {
    std::vector<Domain::AgentSpec> agents;
    std::vector<Domain::AgentSession> openSessions;
    std::vector<DashboardPresenceRecord> presence;
    std::vector<Domain::AuditEvent> recentAudit;
    std::vector<std::string> toolNames;
    Domain::RuntimeDiagnosticSnapshot runtimeDiagnostics;
};

// Fully composed /api/status input. The endpoint and service-active values are
// captured from the immutable listener generation rather than reconstructed
// from mutable manager settings.
struct DashboardApplicationStatus final {
    DashboardApplicationIdentity identity;
    DashboardStatusData operational;
    DashboardTelemetryHealth telemetry;
    Domain::ManagerStatus manager;
    std::string dashboardHost;
    std::uint16_t dashboardPort{};
    bool serviceActive{};
};

} // namespace ForgeConductor::Dashboard
