// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Models.h"

#include <cstdint>
#include <memory>
#include <vector>

struct HWND__;
using HWND = HWND__*;

namespace Forge::Gauge {

struct GaugeSample final {
    float cpu{0};
    float ram{0};
    float gpu{0};
    std::vector<float> cpuHistory;
    std::vector<float> ramHistory;
    std::vector<float> gpuHistory;
    std::vector<float> perCore;
    std::vector<float> disks;
    bool linkHealthy{true};
    bool mcpAlive{false};
    bool managerAlive{false};
};

class GaugeRenderer final {
public:
    GaugeRenderer();
    ~GaugeRenderer();

    bool attach(HWND hwnd, std::uint32_t width, std::uint32_t height);
    void resize(std::uint32_t width, std::uint32_t height);
    void render(const GaugeSample& sample);
    void detach();
    [[nodiscard]] bool isAttached() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Forge::Gauge
