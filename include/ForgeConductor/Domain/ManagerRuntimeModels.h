#pragma once

#include "ForgeConductor/Domain/ManagerModels.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace ForgeConductor::Domain {

// One immutable observation from the platform-owned manager runtime. The
// runtime is authoritative for actual listener/service state and restart
// counting; the application controller owns only desired state and policy.
struct ManagerRuntimeSnapshot final {
    bool listenerListening{};
    bool operationalServiceActive{};
    std::optional<UtcTimePoint> startedAt;
    std::uint32_t restartCount{};
    std::optional<std::string> lastError;
    bool shutdownRequested{};

    bool operator==(const ManagerRuntimeSnapshot&) const = default;
};

// Immutable process identity supplied by the composition root. It deliberately
// contains no platform handle and never consults process-global state.
struct ManagerControllerOptions final {
    PathText home;
    std::string version;
    std::uint32_t processId{};
};

// Atomic controller projection used by the manager host run loop. The existing
// ManagerStatus wire model remains source-compatible while shutdown intent is
// available as a distinct process-lifecycle signal.
struct ManagerControllerSnapshot final {
    ManagerStatus status;
    bool shutdownRequested{};
};

} // namespace ForgeConductor::Domain
