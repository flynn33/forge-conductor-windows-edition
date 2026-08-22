// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "OperatorWindow.h"

#include "ForgeDomain/Version.h"
#include "ForgeComfy/ComfyControl.h"
#include "ForgeLmStudio/LmStudioDeploy.h"
#include "ForgeManager/ManagerController.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/Sessions.h"
#include "ForgeOrchestration/ToolRouter.h"
#include "ForgeTelemetry/TelemetryService.h"

#include <nlohmann/json.hpp>

#include <dwmapi.h>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>
#include <windowsx.h>

#pragma comment(lib, "dwmapi.lib")

namespace Forge::App {
namespace {

constexpr UINT_PTR kTimer = 1;

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), n);
    }
    return out;
}

std::wstring formatBytes(std::uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    wchar_t buffer[48]{};
    if (gb >= 10.0) {
        swprintf(buffer, 48, L"%.0f GB", gb);
    } else {
        swprintf(buffer, 48, L"%.1f GB", gb);
    }
    return buffer;
}

std::wstring formatWs(std::uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    wchar_t buffer[48]{};
    if (mb >= 1024.0) {
        swprintf(buffer, 48, L"%.1f GB", mb / 1024.0);
    } else {
        swprintf(buffer, 48, L"%.0f MB", mb);
    }
    return buffer;
}

} // namespace

OperatorWindow::OperatorWindow(
    HINSTANCE instance,
    Orchestration::ForgeServices& services,
    Telemetry::TelemetryService& telemetry,
    LmStudio::LmStudioDeployService& lmStudio,
    Manager::ManagerController& manager,
    Comfy::ComfyControl* comfy)
    : instance_(instance)
    , services_(services)
    , telemetry_(telemetry)
    , lmStudio_(lmStudio)
    , manager_(manager)
    , comfy_(comfy) {}

OperatorWindow::~OperatorWindow() = default;

int OperatorWindow::run() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(instance_, MAKEINTRESOURCE(1));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ForgeConductor.Main";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Forge Conductor",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1680, 1020,
        nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (!surface_.attach(hwnd_, static_cast<std::uint32_t>(rc.right), static_cast<std::uint32_t>(rc.bottom))) {
        MessageBoxW(hwnd_, L"Direct2D failed to start. Check GPU drivers.", L"Forge Conductor", MB_OK | MB_ICONERROR);
        return 1;
    }
    render();
    SetTimer(hwnd_, kTimer, 50, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK OperatorWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OperatorWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OperatorWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<OperatorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handle(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OperatorWindow::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        KillTimer(hwnd_, kTimer);
        surface_.detach();
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            surface_.resize(LOWORD(lParam), HIWORD(lParam));
            render();
        }
        return 0;
    case WM_TIMER:
        if (wParam == kTimer) {
            render();
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        render();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP:
        onClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEWHEEL:
        surface_.addScroll(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) * 0.5f);
        render();
        return 0;
    case WM_CHAR:
        if (tab_ == Ui::SurfaceTab::Tools) {
            const auto ch = static_cast<wchar_t>(wParam);
            if (ch == 8) {
                if (!toolFilter_.empty()) {
                    toolFilter_.pop_back();
                }
            } else if (ch == 27) {
                toolFilter_.clear();
            } else if (ch >= 32 && toolFilter_.size() < 48) {
                toolFilter_.push_back(ch);
            }
            render();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void OperatorWindow::setTab(Ui::SurfaceTab tab) {
    if (tab_ != tab) {
        tab_ = tab;
        surface_.resetScroll();
    }
}

void OperatorWindow::onClick(int x, int y) {
    switch (surface_.hitTest(x, y)) {
    case Ui::SurfaceHit::TabRig: setTab(Ui::SurfaceTab::Rig); break;
    case Ui::SurfaceHit::TabMcp: setTab(Ui::SurfaceTab::Mcp); break;
    case Ui::SurfaceHit::TabAgents: setTab(Ui::SurfaceTab::Agents); break;
    case Ui::SurfaceHit::TabTools: setTab(Ui::SurfaceTab::Tools); break;
    case Ui::SurfaceHit::TabFeed: setTab(Ui::SurfaceTab::Feed); break;
    case Ui::SurfaceHit::TabDiagnostics: setTab(Ui::SurfaceTab::Diagnostics); break;
    case Ui::SurfaceHit::TabManager: setTab(Ui::SurfaceTab::Manager); break;
    case Ui::SurfaceHit::Deploy: {
        const auto report = lmStudio_.deploy();
        notice_ = report.ok ? L"Deployed primary, fallback, and comfy-control." : L"Deploy failed.";
        break;
    }
    case Ui::SurfaceHit::RefreshMcp:
        notice_ = L"LM Studio status refreshed.";
        break;
    case Ui::SurfaceHit::PrunePresence: {
        const int n = services_.store().presencePrune(300);
        notice_ = L"Pruned " + std::to_wstring(n) + L" stale presence rows.";
        break;
    }
    case Ui::SurfaceHit::PruneSessions:
        pruneIdleSessions();
        break;
    case Ui::SurfaceHit::ExportDiagnostics:
        exportDiagnostics();
        break;
    case Ui::SurfaceHit::RefreshDiagnostics:
        notice_ = L"Diagnostics reloaded.";
        break;
    case Ui::SurfaceHit::ManagerStart:
        manager_.start();
        notice_ = L"Manager start requested.";
        break;
    case Ui::SurfaceHit::ManagerStop:
        manager_.stop();
        notice_ = L"Manager stopped.";
        break;
    case Ui::SurfaceHit::ManagerRestart:
        manager_.restart();
        notice_ = L"Manager restart requested.";
        break;
    case Ui::SurfaceHit::ManagerLogin:
        manager_.setStartWithWindows(!manager_.startWithWindows());
        notice_ = manager_.startWithWindows() ? L"Start with Windows: on" : L"Start with Windows: off";
        break;
    default:
        break;
    }
    render();
}

Ui::SurfaceFrame OperatorWindow::buildFrame() const {
    const auto snapshot = telemetry_.currentFrame();
    const auto doctor = services_.doctor();
    Ui::SurfaceFrame frame;
    frame.tab = tab_;
    frame.cpu = static_cast<float>(snapshot.system.cpu.totalPercent);
    frame.ram = static_cast<float>(snapshot.system.ram.usedPercent);
    frame.gpu = snapshot.system.gpus.empty()
        ? 0.f
        : static_cast<float>(snapshot.system.gpus.front().utilizationPercent);
    frame.cpuBrand = widen(snapshot.system.cpu.brand);
    frame.freqMhz = static_cast<float>(snapshot.system.cpu.frequencyMhz);
    if (snapshot.system.ram.totalBytes > 0) {
        frame.ramMeta = formatBytes(snapshot.system.ram.usedBytes) + L" / " +
            formatBytes(snapshot.system.ram.totalBytes);
    }
    if (!snapshot.system.gpus.empty()) {
        frame.gpuName = widen(snapshot.system.gpus.front().name);
    }
    frame.diskReadMBs = static_cast<float>(snapshot.system.diskIO.readMBps);
    frame.diskWriteMBs = static_cast<float>(snapshot.system.diskIO.writeMBps);
    frame.diskReadIops = static_cast<float>(snapshot.system.diskIO.readIops);
    frame.diskWriteIops = static_cast<float>(snapshot.system.diskIO.writeIops);
    wchar_t ioMeta[80]{};
    swprintf(ioMeta, 80, L"R %.1f  ·  W %.1f MB/s", snapshot.system.diskIO.readMBps,
        snapshot.system.diskIO.writeMBps);
    frame.diskIoMeta = ioMeta;
    if (!snapshot.system.gpus.empty()) {
        frame.gpuDevice = static_cast<float>(snapshot.system.gpus.front().util3d);
        frame.gpuCompute = static_cast<float>(snapshot.system.gpus.front().utilCompute);
        frame.gpuCopy = static_cast<float>(snapshot.system.gpus.front().utilCopy);
        if (frame.gpuDevice <= 0) {
            frame.gpuDevice = frame.gpu;
        }
    }
    frame.cpuHistory = cpuHist_;
    frame.ramHistory = ramHist_;
    frame.gpuHistory = gpuHist_;
    bool comfyAlive = false;
    for (const auto& presence : snapshot.presence) {
        if (presence.hostKind == "mcp-stdio-comfy") {
            comfyAlive = true;
        }
    }
    frame.mcpAlive = snapshot.primaryAlive || snapshot.fallbackAlive || comfyAlive;
    frame.managerAlive = manager_.isRunning();
    frame.startWithWindows = manager_.startWithWindows();
    frame.orchOk = doctor.ok;
    frame.liveMcp = (snapshot.primaryAlive ? 1 : 0) + (snapshot.fallbackAlive ? 1 : 0) + (comfyAlive ? 1 : 0);
    frame.configuredMcp = 3;
    frame.serveCount = frame.liveMcp;
    frame.agentCount = static_cast<int>(services_.catalog().all().size());
    frame.toolCount = static_cast<int>(services_.tools().toolNames().size());
    frame.homePath = services_.paths().home().wstring();
    frame.toolFilter = toolFilter_;
    frame.notice = notice_;
    for (double value : snapshot.system.cpu.perCore) {
        frame.perCore.push_back(static_cast<float>(value));
    }
    for (const auto& volume : snapshot.system.volumes) {
        if (volume.totalBytes == 0) {
            continue;
        }
        Ui::SurfaceCard card;
        card.title = widen(volume.name);
        const auto used = volume.totalBytes - volume.freeBytes;
        card.fraction = static_cast<float>(used) / static_cast<float>(volume.totalBytes);
        card.meta = formatBytes(used) + L" / " + formatBytes(volume.totalBytes);
        frame.disks.push_back(std::move(card));
    }
    for (const auto& proc : snapshot.system.processes) {
        Ui::SurfaceProcessRow row;
        row.name = widen(proc.name);
        row.pid = proc.pid;
        row.cpu = static_cast<float>(proc.cpuPercent);
        row.rss = formatWs(proc.workingSetBytes);
        frame.processes.push_back(std::move(row));
    }

    auto tone = [](const std::string& status) {
        if (status == "ok" || status == "success") return std::tuple{0.28f, 0.88f, 0.48f};
        if (status == "error" || status == "fail") return std::tuple{1.0f, 0.38f, 0.32f};
        if (status == "denied" || status == "den") return std::tuple{1.0f, 0.55f, 0.18f};
        if (status == "warn") return std::tuple{1.0f, 0.78f, 0.22f};
        return std::tuple{0.70f, 0.86f, 0.78f};
    };
    for (const auto& event : services_.store().auditRecent(40)) {
        Ui::SurfaceFeedRow row;
        const auto ts = widen(event.timestamp);
        row.timestamp = ts.size() > 8 ? ts.substr(ts.size() - 8) : ts;
        row.status = widen(event.status);
        row.tool = widen(event.tool);
        if (event.durationMs > 0) {
            row.duration = std::to_wstring(event.durationMs) + L"ms";
            row.durationFrac = (std::min)(1.0f, static_cast<float>(event.durationMs) / 2000.0f);
        }
        const auto [r, g, b] = tone(event.status);
        row.r = r;
        row.g = g;
        row.b = b;
        if (event.status == "error" || event.status == "fail") ++frame.feedErrors;
        if (event.status == "denied") ++frame.feedDenied;
        if (event.status == "warn") ++frame.feedWarn;
        frame.feed.push_back(std::move(row));
    }

    auto packOf = [](const std::string& name) -> const char* {
        if (name.rfind("agent_", 0) == 0 || name == "forge_status") return "agents";
        if (name.rfind("memory_", 0) == 0) return "memory";
        if (name.rfind("session_", 0) == 0 || name.rfind("context_", 0) == 0) return "continuity";
        if (name.rfind("fs_", 0) == 0) return "filesystem";
        if (name.rfind("git_", 0) == 0) return "git";
        if (name.rfind("shell_", 0) == 0) return "shell";
        if (name.rfind("pdf_", 0) == 0 || name.rfind("search_", 0) == 0) return "docs";
        return "core";
    };
    std::map<std::string, int> toolEvents;
    for (const auto& event : snapshot.recentAudit) {
        ++toolEvents[event.tool];
    }
    for (const auto& name : services_.tools().toolNames()) {
        if (!toolFilter_.empty()) {
            const auto wide = widen(name);
            if (wide.find(toolFilter_) == std::wstring::npos) {
                continue;
            }
        }
        Ui::SurfaceToolCard tool;
        tool.name = widen(name);
        tool.pack = widen(packOf(name));
        tool.shortLabel = tool.name.substr(0, (std::min)(tool.name.size(), static_cast<std::size_t>(8)));
        tool.events = toolEvents[name];
        tool.activity = (std::min)(100.0f, static_cast<float>(tool.events) * 20.0f);
        tool.loadTier = tool.events >= 5 ? 2 : (tool.events > 0 ? 1 : 0);
        tool.status = tool.events > 0 ? L"active" : L"idle";
        tool.health = tool.events > 0 ? L"READY" : L"IDLE";
        frame.tools.push_back(std::move(tool));
    }

    const auto sessions = services_.store().sessionList();
    for (const auto& agent : services_.catalog().all()) {
        Ui::SurfaceAgentCard card;
        card.id = widen(agent.id);
        card.name = widen(agent.displayName);
        card.summary = widen(agent.description);
        card.status = L"standby";
        card.health = L"SB";
        for (const auto& session : sessions) {
            if (session.agentID == agent.id && session.status == Domain::SessionStatus::Open) {
                card.live = true;
                card.status = L"session open";
                card.health = L"ON";
                card.activity = 80.0f;
            }
        }
        frame.agents.push_back(std::move(card));
    }

    const auto plugin = lmStudio_.status();
    frame.pluginOk = plugin.ok;
    if (!plugin.checks.empty()) {
        std::wstring detail;
        int okBits = 0;
        for (const auto& check : plugin.checks) {
            if (check.ok) {
                ++okBits;
            }
            if (!detail.empty()) {
                detail += L"  ·  ";
            }
            detail += widen(check.name) + (check.ok ? L" ok" : L" fail");
        }
        frame.pluginStatus = detail;
    }

    auto addMcp = [&](const wchar_t* label, const wchar_t* role, bool live, const std::wstring& extra) {
        Ui::SurfaceMcpCard card;
        card.label = label;
        card.role = role;
        card.live = live;
        card.status = live ? L"live" : L"idle";
        card.health = live ? L"healthy" : L"waiting";
        card.activity = live ? 70.0f : 8.0f;
        card.meta = extra;
        frame.mcpCards.push_back(std::move(card));
    };
    addMcp(L"forge-conductor", L"primary · stdio", snapshot.primaryAlive,
        snapshot.primaryAlive ? L"heartbeat present" : L"LM Studio starts on demand");
    addMcp(L"forge-conductor-fallback", L"fallback · stdio", snapshot.fallbackAlive,
        snapshot.fallbackAlive ? L"failover live" : L"redundancy idle");
    addMcp(L"comfy-control", L"comfy · stdio", comfyAlive,
        comfyAlive ? L"ComfyUI plugin live" : L"prepare-only video setup");
    for (const auto& presence : snapshot.presence) {
        if (presence.hostKind != "mcp-stdio" && presence.hostKind != "mcp-stdio-fallback"
            && presence.hostKind != "mcp-stdio-comfy") {
            Ui::SurfaceMcpCard extra;
            extra.label = widen(presence.hostKind);
            extra.role = L"presence";
            extra.live = true;
            extra.status = L"live";
            extra.health = L"healthy";
            extra.activity = 40.0f;
            extra.meta = L"pid " + std::to_wstring(presence.pid);
            frame.mcpCards.push_back(std::move(extra));
        }
    }

    auto orchCard = [](const wchar_t* title, const wchar_t* state, const wchar_t* detail, float frac,
                        float r, float g, float b) {
        Ui::SurfaceOrchCard card;
        card.title = title;
        card.state = state;
        card.detail = detail;
        card.fraction = frac;
        card.r = r;
        card.g = g;
        card.b = b;
        return card;
    };
    frame.orch.push_back(orchCard(L"MANAGER", frame.managerAlive ? L"UP" : L"DOWN",
        L"task scheduler", frame.managerAlive ? 1.f : 0.f,
        frame.managerAlive ? 0.28f : 1.f, frame.managerAlive ? 0.88f : 0.38f, frame.managerAlive ? 0.48f : 0.32f));
    frame.orch.push_back(orchCard(L"MCP PROCS", frame.liveMcp > 0 ? L"ACTIVE" : L"IDLE",
        frame.liveMcp > 0 ? L"local stdio" : L"on demand",
        (std::min)(1.f, static_cast<float>(frame.liveMcp) / 2.f), 0.08f, 0.90f, 0.96f));
    frame.orch.push_back(orchCard(L"SERVE", frame.serveCount > 0 ? L"ACTIVE" : L"IDLE",
        L"LM Studio roles", (std::min)(1.f, static_cast<float>(frame.serveCount) / 2.f), 0.22f, 0.95f, 0.68f));
    frame.orch.push_back(orchCard(L"STATUS", doctor.ok ? L"OK" : L"FAIL",
        L"doctor", doctor.ok ? 1.f : 0.2f, doctor.ok ? 0.28f : 1.f, doctor.ok ? 0.88f : 0.38f,
        doctor.ok ? 0.48f : 0.32f));

    const auto cfg = services_.config().model();
    auto add = [&](const std::wstring& a, const std::wstring& b, const std::wstring& c) {
        frame.rows.push_back(Ui::SurfaceRow{a, b, c});
    };
    add(L"running", manager_.isRunning() ? L"yes" : L"no", L"headless manager");
    add(L"start_with_windows", manager_.startWithWindows() ? L"yes" : L"no", L"Task Scheduler");
    add(L"auto_restart", cfg.managerAutoRestart ? L"yes" : L"no", L"watchdog");
    add(L"watchdog_sec", std::to_wstring(cfg.managerWatchdogSec), L"config");
    add(L"session_idle_ttl", std::to_wstring(cfg.sessionIdleTtlSec), L"seconds");
    add(L"shell_timeout", std::to_wstring(cfg.shellTimeoutSec), L"seconds");
    add(L"home", services_.paths().home().wstring(), L"data directory");

    {
        std::ifstream stream(services_.paths().masterLog());
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        const auto begin = lines.size() > 80 ? lines.size() - 80 : 0;
        for (std::size_t i = begin; i < lines.size(); ++i) {
            try {
                const auto json = nlohmann::json::parse(lines[i]);
                frame.diagnostics.push_back(Ui::SurfaceRow{
                    widen(json.value("event", "")),
                    widen(json.value("level", "info")),
                    widen(json.value("ts", ""))});
            } catch (...) {
                frame.diagnostics.push_back(Ui::SurfaceRow{widen(lines[i]), L"info", L""});
            }
        }
        if (frame.diagnostics.empty()) {
            for (const auto& check : doctor.checks) {
                frame.diagnostics.push_back(Ui::SurfaceRow{
                    widen(check.name), check.ok ? L"info" : L"error", widen(check.detail)});
            }
        }
        if (comfy_) {
            const auto comfyDoctor = comfy_->doctor();
            for (const auto& check : comfyDoctor.checks) {
                frame.diagnostics.push_back(Ui::SurfaceRow{
                    L"comfy." + widen(check.name), check.ok ? L"info" : L"error", widen(check.detail)});
            }
        }
    }

    if (comfy_) {
        frame.comfyPrepareOnly = true;
        if (const auto session = comfy_->lastPrepareSession()) {
            frame.comfyReady = session->ready;
            frame.comfyPrepareOnly = session->executionPolicy != "full";
            frame.comfyUserMessage = widen(session->userMessage);
            for (const auto& step : session->nextSteps) {
                frame.comfyNextSteps.push_back(widen(step));
            }
        }
    }

    std::wostringstream status;
    status << L"Forge Conductor for Windows  v" << widen(Domain::kVersion)
           << L"   CPU " << static_cast<int>(frame.cpu) << L"%"
           << L"   RAM " << static_cast<int>(frame.ram) << L"%"
           << L"   GPU " << static_cast<int>(frame.gpu) << L"%"
           << L"   MCP " << (frame.mcpAlive ? L"live" : L"idle")
           << L"   " << telemetry_.measuredHz() << L" Hz";
    if (!notice_.empty()) {
        status << L"   " << notice_;
    }
    frame.status = status.str();
    return frame;
}

void OperatorWindow::pruneIdleSessions() {
    int n = 0;
    for (const auto& session : services_.store().sessionList()) {
        if (session.status == Domain::SessionStatus::Idle || session.status == Domain::SessionStatus::Open) {
            try {
                services_.sessions().complete(session.id, "pruned from operator");
                ++n;
            } catch (...) {
            }
        }
    }
    notice_ = L"Pruned " + std::to_wstring(n) + L" sessions.";
}

void OperatorWindow::exportDiagnostics() {
    std::error_code ec;
    std::filesystem::create_directories(services_.paths().exportsDir(), ec);
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const auto jsonPath = services_.paths().exportsDir() / ("diagnostics-" + std::string(stamp) + ".json");
    const auto mdPath = services_.paths().exportsDir() / ("diagnostics-" + std::string(stamp) + ".md");

    std::ifstream in(services_.paths().masterLog());
    nlohmann::json array = nlohmann::json::array();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            array.push_back(nlohmann::json::parse(line));
        } catch (...) {
        }
    }
    {
        std::ofstream out(jsonPath, std::ios::binary);
        out << array.dump(2);
    }
    {
        std::ofstream out(mdPath);
        out << "# Forge Conductor diagnostics\n\n";
        out << "Home: " << services_.paths().home().string() << "\n";
        out << "Records: " << array.size() << "\n\n";
        for (const auto& row : array) {
            out << "- **" << row.value("level", "info") << "** `" << row.value("event", "")
                << "` " << row.value("ts", "") << "\n";
        }
    }
    notice_ = L"Exported " + jsonPath.wstring();
}

void OperatorWindow::render() {
    const auto snapshot = telemetry_.currentFrame();
    cpuHist_.push_back(static_cast<float>(snapshot.system.cpu.totalPercent));
    ramHist_.push_back(static_cast<float>(snapshot.system.ram.usedPercent));
    gpuHist_.push_back(snapshot.system.gpus.empty()
        ? 0.f
        : static_cast<float>(snapshot.system.gpus.front().utilizationPercent));
    if (cpuHist_.size() > 180) {
        cpuHist_.erase(cpuHist_.begin());
        ramHist_.erase(ramHist_.begin());
        gpuHist_.erase(gpuHist_.begin());
    }
    if (surface_.isAttached()) {
        surface_.render(buildFrame());
    }
}

int runOperatorGui(
    HINSTANCE instance,
    Orchestration::ForgeServices& services,
    Telemetry::TelemetryService& telemetry,
    LmStudio::LmStudioDeployService& lmStudio,
    Manager::ManagerController& manager,
    Comfy::ComfyControl* comfy) {
    OperatorWindow window(instance, services, telemetry, lmStudio, manager, comfy);
    return window.run();
}

} // namespace Forge::App
