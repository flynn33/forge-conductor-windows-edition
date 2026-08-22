// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeRuntime/UIModels.h"

#include <string>

namespace Forge::Runtime {

class UISurfaceManager final {
public:
    void setActive(const std::string& moduleID, const UIContributions& contributions);
    void clear();
    [[nodiscard]] SurfaceStateSnapshot snapshot() const;

private:
    SurfaceStateSnapshot state_{};
};

} // namespace Forge::Runtime
