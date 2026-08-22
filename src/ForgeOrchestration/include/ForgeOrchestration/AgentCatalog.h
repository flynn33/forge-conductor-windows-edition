// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgePersistence/AppPaths.h"

namespace Forge::Orchestration {

class AgentCatalog final : public Domain::IAgentCatalog {
public:
    AgentCatalog(Persistence::AppPaths paths, std::filesystem::path bundledAgents);

    std::vector<Domain::AgentSpec> all() const override;
    std::optional<Domain::AgentSpec> get(const std::string& id) const override;
    Domain::AgentSpec recommend(const std::string& task) const override;

    void installBundledPlaybooks() const;

private:
    Persistence::AppPaths paths_;
    std::filesystem::path bundledAgents_;
    std::vector<Domain::AgentSpec> agents_;
};

} // namespace Forge::Orchestration
