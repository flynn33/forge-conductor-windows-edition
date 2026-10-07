#pragma once
#include "ForgeConductor/Contracts/IToolServices.h"
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Application {
[[nodiscard]] inline bool isManagedWorkerToolPermitted(std::string_view name) noexcept {
    return name != "agent_spawn" && name != "agent_cancel" &&
        !name.starts_with("agent_run_") && name != "reviewer_start" && name != "reviewer_cancel" && name != "image_analyze" &&
        name != "workspace_authority_bind" && name != "clu.evaluate" && name != "clu.resolve" &&
        name != "session_checkpoint" && name != "session_handoff" &&
        (!name.starts_with("project_policy.") || name == "project_policy.read") &&
        (!name.starts_with("continuity.") || name == "continuity.status" || name == "continuity.get_pending_handoff") &&
        !name.starts_with("schedule_");
}
[[nodiscard]] inline std::vector<std::string> managedWorkerToolNames(
    const Contracts::IToolCatalog& catalog, bool readOnly = false) {
    std::vector<std::string> names;
    for (const auto& descriptor : catalog.tools())
        if (isManagedWorkerToolPermitted(descriptor.tool.name) &&
            (!readOnly || descriptor.tool.effect == Domain::ToolEffect::Read)) names.push_back(descriptor.tool.name);
    return names;
}
} // namespace ForgeConductor::Application
