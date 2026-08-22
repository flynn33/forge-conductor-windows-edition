// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ForgeDomain/Clock.h"
#include "ForgeDomain/Ports.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace Forge::Persistence {

class SQLiteStore final : public Domain::IPresenceStore,
                          public Domain::ISessionStore,
                          public Domain::IAuditReader {
public:
    SQLiteStore(std::filesystem::path path, std::shared_ptr<Domain::IClock> clock);
    ~SQLiteStore() override;

    SQLiteStore(const SQLiteStore&) = delete;
    SQLiteStore& operator=(const SQLiteStore&) = delete;

    void close();

    std::vector<Domain::PresenceRecord> presenceRecords() override;
    void presenceUpsert(const std::string& clientID, const std::string& hostKind, std::int32_t pid, const std::string& cwd) override;
    void presenceDelete(const std::string& clientID) override;
    int presencePrune(double maxAgeSec) override;

    std::vector<Domain::AgentSession> sessionList(const std::optional<std::string>& agentID = std::nullopt) override;
    void sessionUpsert(const Domain::AgentSession& session) override;

    std::vector<Domain::AuditEvent> auditRecent(int limit) override;
    void auditInsert(const Domain::AuditEvent& event);

    void memorySet(const Domain::MemoryNote& note);
    std::optional<Domain::MemoryNote> memoryGet(const std::string& key);
    std::vector<Domain::MemoryNote> memoryList(const std::optional<std::string>& prefix, bool includeSystem);
    void memoryDelete(const std::string& key);
    std::vector<Domain::MemoryNote> memorySearch(const std::string& query);
    int memoryCount() const;

    void handoffUpsert(const Domain::HandoffPacket& packet);
    std::optional<Domain::HandoffPacket> handoffLatest(bool resumeReadyOnly = false);
    std::optional<Domain::HandoffPacket> handoffGet(const std::string& id);
    std::vector<Domain::HandoffPacket> handoffList(int limit);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void exec(const std::string& sql);
    void migrate();

    std::filesystem::path path_;
    std::shared_ptr<Domain::IClock> clock_;
    sqlite3* db_{nullptr};
    mutable std::mutex mutex_;
};

} // namespace Forge::Persistence
