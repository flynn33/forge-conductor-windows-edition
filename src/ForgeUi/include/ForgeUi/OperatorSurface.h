// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

namespace Forge::Ui {

enum class SurfaceTab {
    Rig = 0,
    Mcp,
    Agents,
    Tools,
    Feed,
    Diagnostics,
    Manager,
    Count
};

enum class SurfaceHit {
    None = 0,
    TabRig,
    TabMcp,
    TabAgents,
    TabTools,
    TabFeed,
    TabDiagnostics,
    TabManager,
    Deploy,
    RefreshMcp,
    PrunePresence,
    PruneSessions,
    ExportDiagnostics,
    RefreshDiagnostics,
    ManagerStart,
    ManagerStop,
    ManagerRestart,
    ManagerLogin
};

struct SurfaceRow final {
    std::wstring a;
    std::wstring b;
    std::wstring c;
};

struct SurfaceCard final {
    std::wstring title;
    std::wstring value;
    std::wstring meta;
    float fraction{0};
    float r{0.08f};
    float g{0.90f};
    float b{0.96f};
};

struct SurfaceOrchCard final {
    std::wstring title;
    std::wstring state;
    std::wstring detail;
    float fraction{0};
    float r{0.08f};
    float g{0.90f};
    float b{0.96f};
};

struct SurfaceMcpCard final {
    std::wstring label;
    std::wstring role;
    std::wstring status;
    std::wstring health;
    std::wstring meta;
    float activity{0};
    bool live{false};
};

struct SurfaceToolCard final {
    std::wstring name;
    std::wstring pack;
    std::wstring shortLabel;
    std::wstring status;
    std::wstring health;
    float activity{0};
    int events{0};
    int loadTier{0};
};

struct SurfaceAgentCard final {
    std::wstring id;
    std::wstring name;
    std::wstring status;
    std::wstring health;
    std::wstring summary;
    float activity{0};
    bool live{false};
};

struct SurfaceProcessRow final {
    std::wstring name;
    std::int32_t pid{0};
    float cpu{0};
    std::wstring rss;
};

struct SurfaceFeedRow final {
    std::wstring timestamp;
    std::wstring status;
    std::wstring tool;
    std::wstring duration;
    float durationFrac{0};
    float r{0.70f};
    float g{0.86f};
    float b{0.78f};
};

struct SurfaceFrame final {
    SurfaceTab tab{SurfaceTab::Rig};
    std::wstring status;
    std::wstring notice;
    float cpu{0};
    float ram{0};
    float gpu{0};
    std::wstring cpuBrand;
    std::wstring ramMeta;
    std::wstring gpuName;
    std::wstring diskIoMeta;
    float freqMhz{0};
    float diskReadMBs{0};
    float diskWriteMBs{0};
    float diskReadIops{0};
    float diskWriteIops{0};
    float gpuDevice{0};
    float gpuCompute{0};
    float gpuCopy{0};
    std::vector<float> cpuHistory;
    std::vector<float> ramHistory;
    std::vector<float> gpuHistory;
    std::vector<float> perCore;
    std::vector<SurfaceCard> disks;
    bool mcpAlive{false};
    bool managerAlive{false};
    bool startWithWindows{false};
    bool orchOk{true};
    bool pluginOk{false};
    int liveMcp{0};
    int configuredMcp{2};
    int agentCount{0};
    int toolCount{0};
    int feedErrors{0};
    int feedDenied{0};
    int feedWarn{0};
    int serveCount{0};
    std::wstring pluginStatus;
    std::wstring homePath;
    std::wstring toolFilter;
    std::vector<SurfaceOrchCard> orch;
    std::vector<SurfaceMcpCard> mcpCards;
    std::vector<SurfaceToolCard> tools;
    std::vector<SurfaceAgentCard> agents;
    std::vector<SurfaceProcessRow> processes;
    std::vector<SurfaceFeedRow> feed;
    std::vector<SurfaceRow> rows;
    std::vector<SurfaceRow> diagnostics;
};

class OperatorSurface final {
public:
    OperatorSurface();
    ~OperatorSurface();

    bool attach(HWND hwnd, std::uint32_t width, std::uint32_t height);
    void resize(std::uint32_t width, std::uint32_t height);
    void render(const SurfaceFrame& frame);
    void detach();
    void addScroll(float delta);
    void resetScroll();
    [[nodiscard]] bool isAttached() const;
    [[nodiscard]] SurfaceHit hitTest(int x, int y) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Forge::Ui
