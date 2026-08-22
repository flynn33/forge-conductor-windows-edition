// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeDomain/Version.h"
#include "ForgeMcp/McpServer.h"
#include "ForgeOrchestration/ForgeServices.h"
#include "ForgeOrchestration/ToolRouter.h"
#include "ForgeOrchestration/PdfWriter.h"
#include "ForgePersistence/AppPaths.h"
#include "ForgeRuntime/CapabilityPolicy.h"
#include "ForgeRuntime/ManifestLoader.h"
#include "ForgeRuntime/SemVer.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

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

} // namespace

int main() {
    std::cout << "Forge Conductor tests\n";

    const auto v = Forge::Runtime::SemVer::parse("0.8.0");
    expect(v.major == 0 && v.minor == 8 && v.patch == 0, "semver_parse");
    expect(std::string(Forge::Domain::kVersion) == "0.1.0", "product_version");

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

    app->shutdown();
    std::error_code ec;
    std::filesystem::remove_all(home, ec);

    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
