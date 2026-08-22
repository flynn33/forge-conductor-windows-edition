// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/AgentCatalog.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace Forge::Orchestration {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::vector<std::string> parseList(const std::string& block) {
    std::vector<std::string> items;
    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line)) {
        const auto pos = line.find("- ");
        if (pos == std::string::npos) {
            continue;
        }
        auto value = line.substr(pos + 2);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
            value.pop_back();
        }
        if (!value.empty()) {
            items.push_back(value);
        }
    }
    return items;
}

Domain::AgentSpec parsePlaybook(const std::filesystem::path& path) {
    const auto text = readFile(path);
    Domain::AgentSpec spec;
    spec.playbookMarkdown = text;
    spec.id = path.stem().string();
    spec.displayName = spec.id;

    const auto start = text.find("---");
    const auto end = text.find("---", start == std::string::npos ? 0 : start + 3);
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return spec;
    }
    const auto front = text.substr(start + 3, end - start - 3);
    auto take = [&](const char* key) -> std::string {
        const auto found = front.find(std::string(key) + ":");
        if (found == std::string::npos) {
            return {};
        }
        auto value = front.substr(found + std::string(key).size() + 1);
        const auto nl = value.find('\n');
        value = value.substr(0, nl);
        while (!value.empty() && (value.front() == ' ' || value.front() == '>' || value.front() == '"')) {
            value.erase(value.begin());
        }
        return value;
    };
    if (const auto id = take("id"); !id.empty()) spec.id = id;
    if (const auto name = take("display_name"); !name.empty()) spec.displayName = name;
    spec.description = take("description");

    const auto toolsAt = front.find("tools:");
    const auto forbiddenAt = front.find("tools_forbidden:");
    if (toolsAt != std::string::npos) {
        const auto stop = forbiddenAt == std::string::npos ? front.size() : forbiddenAt;
        spec.tools = parseList(front.substr(toolsAt, stop - toolsAt));
    }
    if (forbiddenAt != std::string::npos) {
        spec.toolsForbidden = parseList(front.substr(forbiddenAt));
    }
    const auto whenAt = front.find("when_to_use:");
    if (whenAt != std::string::npos) {
        spec.whenToUse = parseList(front.substr(whenAt));
    }
    return spec;
}

} // namespace

AgentCatalog::AgentCatalog(Persistence::AppPaths paths, std::filesystem::path bundledAgents)
    : paths_(std::move(paths))
    , bundledAgents_(std::move(bundledAgents)) {
    installBundledPlaybooks();
    if (std::filesystem::exists(paths_.agentsDir())) {
        for (const auto& entry : std::filesystem::directory_iterator(paths_.agentsDir())) {
            if (entry.path().extension() == ".md") {
                agents_.push_back(parsePlaybook(entry.path()));
            }
        }
    }
}

void AgentCatalog::installBundledPlaybooks() const {
    std::filesystem::create_directories(paths_.agentsDir());
    if (!std::filesystem::exists(bundledAgents_)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(bundledAgents_)) {
        if (entry.path().extension() != ".md") {
            continue;
        }
        const auto dest = paths_.agentsDir() / entry.path().filename();
        if (!std::filesystem::exists(dest)) {
            std::filesystem::copy_file(entry.path(), dest);
        }
    }
}

std::vector<Domain::AgentSpec> AgentCatalog::all() const { return agents_; }

std::optional<Domain::AgentSpec> AgentCatalog::get(const std::string& id) const {
    for (const auto& agent : agents_) {
        if (agent.id == id) {
            return agent;
        }
    }
    return std::nullopt;
}

Domain::AgentSpec AgentCatalog::recommend(const std::string& task) const {
    const auto lower = [&] {
        std::string value = task;
        for (auto& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }();
    for (const auto& agent : agents_) {
        for (const auto& hint : agent.whenToUse) {
            std::string h = hint;
            for (auto& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!h.empty() && lower.find(h.substr(0, std::min<std::size_t>(h.size(), 16))) != std::string::npos) {
                return agent;
            }
        }
        if (lower.find(agent.id) != std::string::npos) {
            return agent;
        }
    }
    if (const auto explore = get("explore")) {
        return *explore;
    }
    return agents_.empty() ? Domain::AgentSpec{} : agents_.front();
}

} // namespace Forge::Orchestration
