// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/Sessions.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace Forge::Orchestration {

class ToolRouter final : public Domain::IToolExecutor {
public:
    explicit ToolRouter(ForgeServices& services);

    std::vector<std::string> toolNames() const override;
    Domain::ToolResult call(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID& clientID) override;

    ForgeServices& services() { return services_; }

private:
    ForgeServices& services_;
    std::vector<std::unique_ptr<Domain::IToolPack>> packs_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::pair<std::string, int>> lastCall_;
};

std::unique_ptr<Domain::IToolPack> makeAgentToolPack(ForgeServices& services);
std::unique_ptr<Domain::IToolPack> makeMemoryToolPack(ForgeServices& services);
std::unique_ptr<Domain::IToolPack> makeContinuityToolPack(ForgeServices& services);
std::unique_ptr<Domain::IToolPack> makeFilesystemToolPack();
std::unique_ptr<Domain::IToolPack> makeGitToolPack();
std::unique_ptr<Domain::IToolPack> makeShellToolPack();
std::unique_ptr<Domain::IToolPack> makeDocsToolPack();
std::unique_ptr<Domain::IToolPack> makeSearchToolPack();

std::string arg(const std::map<std::string, std::string>& arguments, const std::string& key);
std::filesystem::path resolvePath(const std::string& path);

} // namespace Forge::Orchestration
