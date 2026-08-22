// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeComfy/ComfyClient.h"
#include "ForgeComfy/ComfyProcess.h"
#include "ForgeComfy/ComfySettings.h"
#include "ForgeComfy/ComfyStore.h"
#include "ForgeDomain/Clock.h"
#include "ForgeDomain/Ports.h"
#include "ForgePersistence/AppPaths.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Forge::Comfy {

class ComfyControl final : public Domain::IComfyControl {
public:
    ComfyControl(
        Persistence::AppPaths paths,
        ComfySettings settings,
        std::shared_ptr<Platform::IHttpClient> http,
        std::shared_ptr<IProcessLauncher> launcher,
        std::shared_ptr<IProcessInspector> inspector,
        std::shared_ptr<Domain::IClock> clock = std::make_shared<Domain::SystemClock>());

    static std::shared_ptr<ComfyControl> create(const Persistence::AppPaths& paths);

    Domain::DoctorReport status() override;
    Domain::DoctorReport doctor() override;
    std::vector<Domain::ComfyToolSpec> tools() const override;
    Domain::ToolResult call(const std::string& name, const std::string& argumentsJson) override;
    std::optional<Domain::PrepareSession> lastPrepareSession() const override;

private:
    Domain::ToolResult dispatchSystem(const nlohmann::json& arguments);
    Domain::ToolResult dispatchNodes(const nlohmann::json& arguments);
    Domain::ToolResult dispatchWorkflow(const nlohmann::json& arguments);
    Domain::ToolResult prepareVideo(const nlohmann::json& arguments);
    Domain::ToolResult rejected(const std::string& action) const;
    nlohmann::json configurationJson() const;
    bool ensureReachable(bool startIfNeeded, std::string& error);
    std::vector<std::filesystem::path> findVideoWorkflows() const;
    std::vector<std::string> videoNodeHints(const nlohmann::json& objectInfo) const;

    Persistence::AppPaths paths_;
    ComfySettings settings_;
    std::shared_ptr<Platform::IHttpClient> http_;
    std::shared_ptr<IProcessLauncher> launcher_;
    std::shared_ptr<IProcessInspector> inspector_;
    std::shared_ptr<Domain::IClock> clock_;
    ComfyStore store_;
    ComfyClient client_;
};

} // namespace Forge::Comfy
