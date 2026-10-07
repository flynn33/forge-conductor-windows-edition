#pragma once
#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include <functional>
#include <string>
#include <string_view>

namespace ForgeConductor::Mcp {
struct AgentWorkerToolDependencies final {
    std::function<Contracts::IManagedRunService*()> runs;
    Contracts::IAgentCatalog& agents;
    Contracts::IToolCatalog& catalog;
    Contracts::IUuidGenerator& uuids;
};
[[nodiscard]] Domain::Result<std::string> executeAgentWorkerTool(
    std::string_view name, std::string_view arguments,
    const Contracts::WorkspaceAuthority& actualAuthority,
    const Domain::OperationContext& context,
    const AgentWorkerToolDependencies& dependencies) noexcept;
} // namespace ForgeConductor::Mcp
