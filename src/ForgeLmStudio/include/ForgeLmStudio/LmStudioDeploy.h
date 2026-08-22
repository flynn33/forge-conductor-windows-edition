// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Ports.h"
#include "ForgePersistence/AppPaths.h"

#include <filesystem>

namespace Forge::LmStudio {

class LmStudioDeployService final : public Domain::ILmStudioDeploy {
public:
    LmStudioDeployService(Persistence::AppPaths paths, std::filesystem::path executable);

    Domain::DoctorReport status() override;
    Domain::DoctorReport deploy() override;

    [[nodiscard]] std::filesystem::path lmStudioHome() const;
    [[nodiscard]] std::filesystem::path mcpJsonPath() const;

private:
    Persistence::AppPaths paths_;
    std::filesystem::path executable_;
};

} // namespace Forge::LmStudio
