#pragma once

#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Infrastructure::Windows {

struct PreparedLocalModel final {
    std::string identifier;
    std::uint32_t contextCapacity{};
    bool serverStarted{};
    bool modelLoaded{};
};

// Starts only the installed local LM Studio CLI, on loopback, and loads only an
// already-downloaded language model. Never downloads models or unloads another
// application's model. All postconditions are read back from the server.
class WindowsModelPreparation final {
public:
    [[nodiscard]] Domain::Result<PreparedLocalModel> prepare(
        const Domain::ManagerSettings& settings,
        const Domain::OperationContext& context) noexcept;
};

} // namespace ForgeConductor::Infrastructure::Windows
