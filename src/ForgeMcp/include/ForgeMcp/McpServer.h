// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Models.h"
#include "ForgeOrchestration/ForgeServices.h"

#include <string>

namespace Forge::Mcp {

class McpServer final {
public:
    explicit McpServer(Orchestration::ForgeServices& services, std::string role = "primary");

    [[nodiscard]] std::string handleLine(const std::string& line);
    int runStdio();

    [[nodiscard]] static std::string negotiateProtocolVersion(const std::string& requested);

private:
    std::string handleObject(const std::string& jsonText);
    void refreshPresence();

    Orchestration::ForgeServices& services_;
    Domain::ClientID clientID_;
    std::string role_;
    std::string deploymentID_;
};

} // namespace Forge::Mcp
