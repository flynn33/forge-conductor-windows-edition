// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/ModuleModels.h"

#include <string>
#include <vector>

namespace Forge::Runtime {

class ManifestLoader final {
public:
    [[nodiscard]] std::vector<ModuleManifest> loadDirectory(const std::string& directory) const;
    [[nodiscard]] ModuleManifest loadFile(const std::string& path) const;
    [[nodiscard]] ModuleManifest parseJson(const std::string& jsonText) const;
};

} // namespace Forge::Runtime
