// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeComfy/ComfyControl.h"
#include "ForgeDomain/Version.h"
#include "ForgeLmStudio/LmStudioDeploy.h"
#include "ForgeManager/ManagerController.h"
#include "ForgeMcp/McpServer.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/ToolRouter.h"
#include "ForgeOrchestration/PdfWriter.h"
#include "ForgePersistence/AppPaths.h"
#include "ForgePlatform/HttpClient.h"
#include "ForgePlatform/HttpUrl.h"
#include "ForgePlatform/InstanceLock.h"
#include "ForgeRuntime/CapabilityPolicy.h"
#include "ForgeRuntime/ManifestLoader.h"
#include "ForgeRuntime/SemVer.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;
int g_passed = 0;

void expect(bool cond, const char* name) {
    if (cond) {
        ++g_passed;
        std::cout << "  PASS  " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  FAIL  " << name << "\n";
    }
}

std::filesystem::path tempHome() {
    auto path = std::filesystem::temp_directory_path() / ("forge-test-" + Forge::Domain::makeUuid());
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path findAgents() {
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        const auto candidate = dir / "resources" / "Agents";
        if (std::filesystem::exists(candidate)) return candidate;
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

class ScriptedHttp final : public Forge::Platform::IHttpClient {
public:
    struct Exchange {
        std::string method;
        std::string pathContains;
        int status{200};
        std::string body;
    };
    std::vector<Exchange> exchanges;
    std::vector<std::string> seen;

    Forge::Platform::HttpResponse request(
        const std::string& method,
        const std::string& url,
        const std::string& jsonBody) override {
        seen.push_back(method + " " + url);
        (void)jsonBody;
        Forge::Platform::HttpResponse response;
        if (exchanges.empty()) {
            response.status = 500;
            response.error = "unexpected " + method + " " + url;
            return response;
        }
        const auto expected = exchanges.front();
        exchanges.erase(exchanges.begin());
        if (method != expected.method || url.find(expected.pathContains) == std::string::npos) {
            response.status = 500;
            response.error = "mismatch, got " + method + " " + url;
            return response;
        }
        response.status = expected.status;
        response.body = expected.body;
        return response;
    }
};

class FakeLauncher final : public Forge::Comfy::IProcessLauncher {
public:
    Forge::Comfy::ProcessIdentity last;
    int starts{0};
    Forge::Comfy::ProcessIdentity start(
        const std::filesystem::path& executable,
        const std::vector<std::string>&,
        const std::filesystem::path& workingDirectory) override {
        ++starts;
        last.pid = 4242;
        last.executable = executable.string();
        last.commandLine = executable.string();
        last.createTime = 1001;
        last.workingDirectory = workingDirectory;
        return last;
    }
};

class FakeInspector final : public Forge::Comfy::IProcessInspector {
public:
    Forge::Comfy::ProcessIdentity live;
    bool reuse{false};
    std::optional<Forge::Comfy::ProcessIdentity> snapshot(std::int32_t pid) const override {
        auto copy = live;
        copy.pid = pid;
        if (reuse) {
            copy.createTime = live.createTime + 1;
        }
        return copy;
    }
    bool terminate(const Forge::Comfy::ProcessIdentity& identity) override {
        const auto current = snapshot(identity.pid);
        if (!current) {
            return false;
        }
        if (identity.createTime != 0 && current->createTime != identity.createTime) {
            throw std::runtime_error("process identity reused");
        }
        return true;
    }
};

} // namespace

int main() {
    std::cout << "Forge Conductor tests\n";

    const auto v = Forge::Runtime::SemVer::parse("0.8.0");
    expect(v.major == 0 && v.minor == 8 && v.patch == 0, "semver_parse");
    expect(std::string(Forge::Domain::kVersion) == "0.8.0", "product_version");

    Forge::Runtime::DefaultCommunicationGuard guard;
    expect(!guard.allow("", "x"), "guard_empty");
    expect(!guard.allow("a", "a"), "guard_self");
    expect(!guard.allow("a", "forge.internal.x"), "guard_reserved");
    expect(guard.allow("a", "b"), "guard_ok");

    const auto home = tempHome();
    Forge::Persistence::AppPaths paths(home);
    paths.ensureLayout();
    expect(std::filesystem::exists(paths.configJSON()), "home_layout");

    auto agents = findAgents();
    auto app = Forge::Orchestration::ForgeServices::bootstrap(home, agents);
    expect(app->catalog().all().size() >= 5, "agent_catalog");
    const auto doctor = app->doctor();
    expect(doctor.ok, "doctor_ok");

    const auto names = app->tools().toolNames();
    expect(names.size() == 34, "tool_count_34");

    Forge::Domain::ClientID client{"test-client"};
    auto set = app->tools().call("memory_set", {{"key", "project/demo"}, {"body", "hello"}}, client);
    expect(set.ok, "memory_set");
    auto get = app->tools().call("memory_get", {{"key", "project/demo"}}, client);
    expect(get.ok && get.payload["body"] == "hello", "memory_get");

    auto denied = app->tools().call("shell_exec", {{"command", "echo hi"}}, client);
    expect(!denied.ok && denied.code == "active_session_required", "shell_requires_session");

    auto start = app->tools().call("agent_run_start", {
        {"agent_id", "explore"},
        {"goal", "map"},
        {"cwd", home.string()},
    }, client);
    expect(start.ok, "agent_start");

    auto cp = app->tools().call("session_checkpoint", {{"goal", "g"}, {"narrative", "n"}}, client);
    expect(cp.ok, "checkpoint");
    auto ctx = app->tools().call("context_get", {}, client);
    expect(ctx.ok, "context_get");

    Forge::Mcp::McpServer server(*app, "primary");
    const auto init = server.handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    expect(init.find("forge-conductor") != std::string::npos, "mcp_initialize");
    expect(init.find('\n') == std::string::npos, "mcp_one_line");
    const auto list = server.handleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    expect(list.find("fs_read") != std::string::npos, "mcp_tools_list");

    Forge::Orchestration::PdfWriter pdf;
    const auto pdfPath = home / "sample.pdf";
    pdf.writeText(pdfPath, "Forge", "Hello");
    expect(std::filesystem::exists(pdfPath), "pdf_write");

    const auto loader = Forge::Runtime::ManifestLoader{};
    auto dir = std::filesystem::current_path();
    std::filesystem::path manifests;
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(dir / "resources" / "Manifests")) {
            manifests = dir / "resources" / "Manifests";
            break;
        }
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    if (!manifests.empty()) {
        const auto loaded = loader.loadDirectory(manifests.string());
        expect(loaded.size() >= 7, "manifests_loaded");
        expect(loaded.front().isSchemaValid(), "manifest_schema");
    }

    {
        const auto id = Forge::Domain::makeUuid();
        std::wstring wide = L"Local\\ForgeConductor.TestLock.";
        wide.append(id.begin(), id.end());
        Forge::Platform::InstanceLock first(wide);
        expect(first.isPrimary(), "instance_lock_primary");
        Forge::Platform::InstanceLock second(wide);
        expect(!second.isPrimary(), "instance_lock_secondary");
    }

    expect(Forge::Platform::isLoopbackHost("127.0.0.1"), "loopback_ipv4");
    expect(Forge::Platform::isLoopbackHost("localhost"), "loopback_name");
    expect(!Forge::Platform::isLoopbackHost("8.8.8.8"), "not_loopback");
    expect(Forge::Platform::validateLoopbackHttpBaseUrl("http://127.0.0.1:8188/") == "http://127.0.0.1:8188",
        "base_url_strip");
    bool threw = false;
    try {
        const auto rejected = Forge::Platform::validateLoopbackHttpBaseUrl("http://192.168.1.5:8188");
        (void)rejected;
    } catch (...) {
        threw = true;
    }
    expect(threw, "reject_lan_url");

    const auto lmHome = tempHome();
    const auto fakeExe = home / "ForgeConductor.exe";
    {
        std::ofstream out(fakeExe, std::ios::binary);
        out << "x";
    }
    Forge::LmStudio::LmStudioDeployService deploy(paths, fakeExe, lmHome);
    auto deployReport = deploy.deploy();
    expect(deployReport.ok, "deploy_ok");
    const auto mcpPath = lmHome / "mcp.json";
    nlohmann::json mcpJson;
    {
        std::ifstream in(mcpPath);
        in >> mcpJson;
    }
    expect(mcpJson["mcpServers"].contains("forge-conductor"), "deploy_primary");
    expect(mcpJson["mcpServers"].contains("forge-conductor-fallback"), "deploy_fallback");
    expect(mcpJson["mcpServers"].contains("comfy-control"), "deploy_comfy");
    expect(mcpJson.dump().find("controller.token") == std::string::npos, "deploy_no_token");
    expect(std::filesystem::exists(lmHome / "extensions" / "plugins" / "mcp" / "comfy-control" / "manifest.json"),
        "deploy_comfy_plugin");
    auto deployStatus = deploy.status();
    expect(deployStatus.ok, "deploy_status_ok");
    mcpJson["mcpServers"]["unrelated"] = {{"command", "echo"}};
    {
        std::ofstream out(mcpPath);
        out << mcpJson.dump(2);
    }
    deploy.deploy();
    {
        std::ifstream in(mcpPath);
        in >> mcpJson;
    }
    expect(mcpJson["mcpServers"].contains("unrelated"), "deploy_preserves_unrelated");

    auto http = std::make_shared<ScriptedHttp>();
    http->exchanges = {
        {"GET", "/api/system_stats", 200, R"({"system":{"os":"win"}})"},
        {"GET", "/api/object_info", 200, R"({"LoadImage":{"input":{"required":{}}},"UNETLoader":{"input":{"required":{}}}})"},
        {"GET", "/api/system_stats", 200, R"({"system":{"os":"win"}})"},
        {"GET", "/api/object_info", 200, R"({"LoadImage":{"input":{"required":{}}},"UNETLoader":{"input":{"required":{}}}})"},
    };
    auto launcher = std::make_shared<FakeLauncher>();
    auto inspector = std::make_shared<FakeInspector>();
    Forge::Comfy::ComfySettings settings;
    settings.executionPolicy = "prepare_only";
    settings.comfyRoot = home / "missing-comfy";
    settings.comfyPython = home / "missing-python.exe";
    Forge::Comfy::ComfyControl comfy(paths, settings, http, launcher, inspector);
    auto missingDoctor = comfy.doctor();
    expect(!missingDoctor.ok, "comfy_doctor_missing_root");

    const auto comfyRoot = home / "ComfyUI";
    std::filesystem::create_directories(comfyRoot / "user" / "default" / "workflows");
    {
        std::ofstream out(comfyRoot / "main.py");
        out << "print('ok')\n";
        std::ofstream wf(comfyRoot / "user" / "default" / "workflows" / "02_Mythic_I2V_Wan22_5B.json");
        wf << R"({"nodes":[]})";
        std::ofstream py(home / "python.exe", std::ios::binary);
        py << "x";
    }
    settings.comfyRoot = comfyRoot;
    settings.comfyPython = home / "python.exe";
    http->exchanges = {
        {"GET", "/api/system_stats", 200, R"({"system":{"os":"win"}})"},
        {"GET", "/api/object_info", 200,
            R"({"LoadImage":{},"UNETLoader":{},"WanImageToVideo":{}})"},
        {"GET", "/api/system_stats", 200, R"({"system":{"os":"win"}})"},
        {"GET", "/api/object_info", 200,
            R"({"LoadImage":{},"UNETLoader":{},"WanImageToVideo":{}})"},
    };
    Forge::Comfy::ComfyControl live(paths, settings, http, launcher, inspector);
    auto prep = live.call("comfy_prepare_video", R"({"prompt":"a lantern over water","start_comfy":true})");
    expect(prep.ok, "prepare_video_ok");
    expect(prep.payload["result"].find("next_steps") != std::string::npos, "prepare_has_next_steps");
    expect(prep.payload["result"].find("\"submitted\":false") != std::string::npos, "prepare_not_submitted");
    expect(live.lastPrepareSession().has_value(), "prepare_session_persisted");
    auto exec = live.call("comfy_workflow", R"({"action":"execute","arguments":{"workflow_id":"x"}})");
    expect(!exec.ok && exec.code == "prepare_only", "execute_rejected");
    auto simple = live.call("comfy_render_simple", R"({"prompt":"nope"})");
    expect(!simple.ok && simple.code == "prepare_only", "render_simple_omitted_rejected");
    auto vram = live.call("comfy_system", R"({"action":"vram_acquire"})");
    expect(!vram.ok && vram.code == "prepare_only", "vram_rejected");

    Forge::Comfy::ProcessIdentity owned;
    owned.pid = 4242;
    owned.executable = settings.comfyPython.string();
    owned.createTime = 1001;
    inspector->live = owned;
    inspector->reuse = true;
    bool reuseThrew = false;
    try {
        inspector->terminate(owned);
    } catch (...) {
        reuseThrew = true;
    }
    expect(reuseThrew, "pid_reuse_refused");

    Forge::Mcp::McpServer comfyServer(*app, "comfy", &live);
    const auto comfyInit = comfyServer.handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25"}})");
    expect(comfyInit.find("comfy-control") != std::string::npos, "mcp_comfy_name");
    const auto comfyList = comfyServer.handleLine(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    expect(comfyList.find("comfy_prepare_video") != std::string::npos, "mcp_comfy_prepare_tool");
    expect(comfyList.find("fs_read") == std::string::npos, "mcp_comfy_hides_fs");
    const auto primaryList = server.handleLine(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    expect(primaryList.find("fs_read") != std::string::npos, "mcp_primary_keeps_fs");

    if (const char* liveDeployEnv = std::getenv("FORGE_LIVE_DEPLOY"); liveDeployEnv && std::string(liveDeployEnv) == "1") {
        std::filesystem::path exe = std::filesystem::current_path() / "ForgeConductor.exe";
        if (!std::filesystem::exists(exe)) {
            exe = std::filesystem::path("D:/Projects/Forge-Conductor-Windows/out/Debug/x64/ForgeConductorApp/ForgeConductor.exe");
        }
        Forge::LmStudio::LmStudioDeployService liveDeploy(Forge::Persistence::AppPaths(std::nullopt), exe);
        const auto report = liveDeploy.deploy();
        expect(report.ok, "live_deploy_ok");
        const auto status = liveDeploy.status();
        expect(status.ok, "live_deploy_status");
        expect(std::filesystem::exists(liveDeploy.mcpJsonPath()), "live_mcp_json");
    }

    if (const char* liveMgrEnv = std::getenv("FORGE_LIVE_MANAGER"); liveMgrEnv && std::string(liveMgrEnv) == "1") {
        std::filesystem::path exe = std::filesystem::path(
            "D:/Projects/Forge-Conductor-Windows/out/Debug/x64/ForgeConductorApp/ForgeConductor.exe");
        const auto mgrHome = tempHome();
        Forge::Persistence::AppPaths mgrPaths(mgrHome);
        mgrPaths.ensureLayout();
        Forge::Manager::ManagerController manager(mgrPaths, exe);
        manager.start();
        expect(manager.isRunning(), "live_manager_start");
        manager.setStartWithWindows(true);
        expect(manager.startWithWindows(), "live_manager_login_on");
        manager.setStartWithWindows(false);
        expect(!manager.startWithWindows(), "live_manager_login_off");
        manager.stop();
        expect(!manager.isRunning(), "live_manager_stop");
        std::error_code mgrEc;
        std::filesystem::remove_all(mgrHome, mgrEc);
    }

    if (const char* liveComfyEnv = std::getenv("FORGE_LIVE_COMFY"); liveComfyEnv && std::string(liveComfyEnv) == "1") {
        const auto liveHome = tempHome();
        Forge::Persistence::AppPaths livePaths(liveHome);
        livePaths.ensureLayout();
        auto ctrl = Forge::Comfy::ComfyControl::create(livePaths);
        auto livePrep = ctrl->call("comfy_prepare_video", R"({"prompt":"a lantern over black water","start_comfy":true})");
        expect(livePrep.ok, "live_prepare_video");
        expect(livePrep.payload["result"].find("next_steps") != std::string::npos, "live_prepare_next_steps");
        expect(livePrep.payload["result"].find("\"submitted\":false") != std::string::npos, "live_prepare_not_submitted");
        auto session = ctrl->lastPrepareSession();
        expect(session.has_value(), "live_prepare_session");
        ctrl->call("comfy_system", R"({"action":"stop"})");
        std::error_code liveEc;
        std::filesystem::remove_all(liveHome, liveEc);
    }

    app->shutdown();
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::remove_all(lmHome, ec);

    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
