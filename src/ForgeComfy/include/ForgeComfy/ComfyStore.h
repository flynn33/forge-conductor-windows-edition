// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeComfy/ComfyProcess.h"
#include "ForgeDomain/Models.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace Forge::Comfy {

struct WorkflowRecord final {
    std::string id;
    std::string name;
    int revision{1};
    std::string apiJson;
    std::string apiHash;
    std::string createdAt;
    std::string updatedAt;
};

class ComfyStore final {
public:
    explicit ComfyStore(std::filesystem::path path);
    ~ComfyStore();

    ComfyStore(const ComfyStore&) = delete;
    ComfyStore& operator=(const ComfyStore&) = delete;

    void close();

    WorkflowRecord saveWorkflow(const std::string& name, const std::string& apiJson, const std::string& nowIso);
    std::vector<WorkflowRecord> listWorkflows();
    std::optional<WorkflowRecord> getWorkflow(const std::string& id);

    void saveProcess(const std::string& id, const std::string& kind, const ProcessIdentity& identity);
    std::optional<ProcessIdentity> loadProcess(const std::string& id);
    void deleteProcess(const std::string& id);
    std::vector<std::pair<std::string, ProcessIdentity>> listProcesses();

    void savePrepareSession(const Domain::PrepareSession& session);
    std::optional<Domain::PrepareSession> lastPrepareSession() const;

private:
    void exec(const std::string& sql);
    void migrate();

    std::filesystem::path path_;
    sqlite3* db_{nullptr};
    mutable std::mutex mutex_;
};

[[nodiscard]] std::string sha256Hex(const std::string& data);

} // namespace Forge::Comfy
