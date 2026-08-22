// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Forge::Persistence {

class AppPaths final {
public:
    explicit AppPaths(std::optional<std::filesystem::path> home = std::nullopt);

    [[nodiscard]] const std::filesystem::path& home() const noexcept { return home_; }
    [[nodiscard]] std::filesystem::path storeSQLite() const { return home_ / "store.sqlite"; }
    [[nodiscard]] std::filesystem::path comfySQLite() const { return home_ / "comfy.sqlite"; }
    [[nodiscard]] std::filesystem::path configJSON() const { return home_ / "config.json"; }
    [[nodiscard]] std::filesystem::path auditJSONL() const { return home_ / "audit.jsonl"; }
    [[nodiscard]] std::filesystem::path agentsDir() const { return home_ / "agents"; }
    [[nodiscard]] std::filesystem::path logsDir() const { return home_ / "logs"; }
    [[nodiscard]] std::filesystem::path memoryDir() const { return home_ / "memory"; }
    [[nodiscard]] std::filesystem::path memoryHandoffsDir() const { return memoryDir() / "handoffs"; }
    [[nodiscard]] std::filesystem::path memoryCurrentTask() const { return memoryDir() / "current-task.md"; }
    [[nodiscard]] std::filesystem::path memoryNextChat() const { return memoryDir() / "NEXT-CHAT.md"; }
    [[nodiscard]] std::filesystem::path managerPid() const { return home_ / "manager.pid"; }
    [[nodiscard]] std::filesystem::path managerState() const { return home_ / "manager-state.json"; }
    [[nodiscard]] std::filesystem::path exportsDir() const { return home_ / "exports"; }
    [[nodiscard]] std::filesystem::path masterLog() const { return logsDir() / "diagnostics.jsonl"; }

    void ensureLayout() const;

private:
    std::filesystem::path home_;
};

} // namespace Forge::Persistence
