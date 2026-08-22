// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeMcp/McpServer.h"

#include "ForgeOrchestration/ToolRouter.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

namespace Forge::Mcp {
namespace {

nlohmann::json ok(const nlohmann::json& id, nlohmann::json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json err(const nlohmann::json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

std::map<std::string, std::string> flatten(const nlohmann::json& value) {
    std::map<std::string, std::string> out;
    if (!value.is_object()) {
        return out;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.value().is_string()) {
            out[it.key()] = it.value().get<std::string>();
        } else if (it.value().is_number() || it.value().is_boolean()) {
            out[it.key()] = it.value().dump();
        } else {
            out[it.key()] = it.value().dump();
        }
    }
    return out;
}

} // namespace

McpServer::McpServer(Orchestration::ForgeServices& services, std::string role)
    : services_(services)
    , clientID_(Domain::ClientID::generate())
    , role_(std::move(role)) {
    if (const char* env = std::getenv("FORGE_MCP_ROLE"); env && *env) {
        role_ = env;
    }
    if (const char* env = std::getenv("FORGE_DEPLOYMENT_ID"); env && *env) {
        deploymentID_ = env;
    }
}

std::string McpServer::negotiateProtocolVersion(const std::string& requested) {
    if (requested == "2024-11-05" || requested == "2025-03-26" || requested == "2025-11-25") {
        return requested;
    }
    return "2025-11-25";
}

void McpServer::refreshPresence() {
    try {
#ifdef _WIN32
        const auto pid = static_cast<std::int32_t>(GetCurrentProcessId());
#else
        const std::int32_t pid = 0;
#endif
        const auto hostKind = role_ == "fallback" ? "mcp-stdio-fallback" : "mcp-stdio";
        services_.store().presenceUpsert(clientID_.rawValue, hostKind, pid, services_.paths().home().string());
    } catch (...) {
    }
}

std::string McpServer::handleLine(const std::string& line) {
    if (line.empty()) {
        return {};
    }
    return handleObject(line);
}

std::string McpServer::handleObject(const std::string& jsonText) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(jsonText);
    } catch (...) {
        return err(nullptr, -32700, "Parse error").dump();
    }
    const auto id = message.contains("id") ? message["id"] : nlohmann::json(nullptr);
    const bool notification = id.is_null();
    if (!message.contains("method")) {
        return notification ? std::string{} : err(id, -32600, "Invalid Request: missing method").dump();
    }
    const auto method = message["method"].get<std::string>();
    if (method.rfind("notifications/", 0) == 0) {
        return {};
    }
    try {
        if (method == "initialize") {
            refreshPresence();
            const auto requested = message.value("params", nlohmann::json::object())
                                       .value("protocolVersion", "");
            const auto negotiated = negotiateProtocolVersion(requested);
            const auto serverName = role_ == "fallback" ? "forge-conductor-fallback" : "forge-conductor";
            nlohmann::json result;
            result["protocolVersion"] = negotiated;
            result["capabilities"] = {{"tools", {{"listChanged", false}}}};
            result["serverInfo"] = {
                {"name", serverName},
                {"version", Domain::kVersion},
            };
            return ok(id, result).dump();
        }
        if (method == "tools/list") {
            nlohmann::json tools = nlohmann::json::array();
            for (const auto& name : services_.tools().toolNames()) {
                tools.push_back({
                    {"name", name},
                    {"description", name},
                    {"inputSchema", {{"type", "object"}, {"additionalProperties", true}}},
                });
            }
            return ok(id, {{"tools", tools}}).dump();
        }
        if (method == "tools/call") {
            const auto params = message.value("params", nlohmann::json::object());
            const auto name = params.value("name", "");
            const auto args = flatten(params.value("arguments", nlohmann::json::object()));
            const auto result = services_.tools().call(name, args, clientID_);
            nlohmann::json payload = {{"ok", result.ok}};
            for (const auto& [k, v] : result.payload) {
                payload[k] = v;
            }
            if (!result.ok) {
                payload["code"] = result.code;
                payload["message"] = result.message;
            }
            nlohmann::json content = nlohmann::json::array();
            content.push_back({{"type", "text"}, {"text", payload.dump()}});
            return ok(id, {{"content", content}, {"isError", !result.ok}}).dump();
        }
        if (method == "ping") {
            return ok(id, nlohmann::json::object()).dump();
        }
        return err(id, -32601, "Method not found: " + method).dump();
    } catch (const std::exception& ex) {
        return err(id, -32603, ex.what()).dump();
    }
}

int McpServer::runStdio() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    refreshPresence();
    services_.diagnostics().info("mcp_serve_start", {
        {"client_id", clientID_.rawValue},
        {"role", role_},
        {"deployment_id", deploymentID_},
    });

    std::string pending;
    char buffer[4096];
    while (true) {
        if (!std::fgets(buffer, sizeof(buffer), stdin)) {
            break;
        }
        pending.append(buffer);
        auto nl = pending.find('\n');
        while (nl != std::string::npos) {
            auto line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("Content-Length:", 0) == 0) {
                // Ignore LSP-style headers; wait for the blank line + body in subsequent reads.
                nl = pending.find('\n');
                continue;
            }
            const auto response = handleLine(line);
            if (!response.empty()) {
                std::fwrite(response.data(), 1, response.size(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
            nl = pending.find('\n');
        }
    }
    try {
        services_.store().presenceDelete(clientID_.rawValue);
    } catch (...) {
    }
    return 0;
}

} // namespace Forge::Mcp
