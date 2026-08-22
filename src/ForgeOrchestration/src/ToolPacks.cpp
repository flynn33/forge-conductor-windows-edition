// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/PdfWriter.h"
#include "ForgeOrchestration/ProcessRunner.h"
#include "ForgeOrchestration/ToolRouter.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Forge::Orchestration {
namespace {

class AgentToolPack final : public Domain::IToolPack {
public:
    explicit AgentToolPack(ForgeServices* services) : services_(services) {}
    std::vector<std::string> toolNames() const override {
        return {"forge_status", "agent_list", "agent_get", "agent_context", "agent_recommend",
                "agent_run_start", "agent_run_status", "agent_run_complete"};
    }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID& clientID) override {
        if (!services_) return std::nullopt;
        if (name == "forge_status") {
            auto result = Domain::ToolResult::success({
                {"version", Domain::kVersion},
                {"product", Domain::kProductName},
                {"home", services_->paths().home().string()},
                {"tools", std::to_string(services_->tools().toolNames().size())},
                {"memory_note_count", std::to_string(services_->store().memoryCount())},
            });
            return result;
        }
        if (name == "agent_list") {
            std::ostringstream ids;
            for (const auto& agent : services_->catalog().all()) {
                if (!ids.str().empty()) ids << ',';
                ids << agent.id;
            }
            return Domain::ToolResult::success({{"agents", ids.str()}});
        }
        if (name == "agent_get" || name == "agent_context") {
            const auto spec = services_->catalog().get(arg(arguments, "agent_id"));
            if (!spec) return Domain::ToolResult::failure("agent_not_found", "Unknown agent", true);
            return Domain::ToolResult::success({
                {"agent_id", spec->id},
                {"display_name", spec->displayName},
                {"description", spec->description},
            });
        }
        if (name == "agent_recommend") {
            const auto spec = services_->catalog().recommend(arg(arguments, "task"));
            return Domain::ToolResult::success({{"agent_id", spec.id}, {"display_name", spec.displayName}});
        }
        if (name == "agent_run_start") {
            const auto id = arg(arguments, "agent_id");
            const auto goal = arg(arguments, "goal");
            const auto cwd = arg(arguments, "cwd");
            if (id.empty() || cwd.empty()) {
                return Domain::ToolResult::failure("missing_argument", "agent_id and cwd required", true);
            }
            const auto session = services_->sessions().start(id, goal, cwd, clientID);
            return Domain::ToolResult::success({{"session_id", session.id}, {"agent_id", id}, {"cwd", cwd}});
        }
        if (name == "agent_run_status") {
            const auto sid = arg(arguments, "session_id");
            const auto session = services_->sessions().status(sid);
            if (!session) return Domain::ToolResult::failure("missing_session_id", "session not found", true);
            return Domain::ToolResult::success({{"session_id", session->id}, {"agent_id", session->agentID}});
        }
        if (name == "agent_run_complete") {
            const auto sid = arg(arguments, "session_id");
            if (sid.empty()) return Domain::ToolResult::failure("missing_session_id", "session_id required", true);
            const auto session = services_->sessions().complete(sid, arg(arguments, "report"));
            return Domain::ToolResult::success({{"session_id", session.id}, {"status", "completed"}});
        }
        return std::nullopt;
    }

private:
    ForgeServices* services_;
};

class MemoryToolPack final : public Domain::IToolPack {
public:
    explicit MemoryToolPack(ForgeServices* services) : services_(services) {}
    std::vector<std::string> toolNames() const override {
        return {"memory_set", "memory_get", "memory_list", "memory_delete", "memory_search"};
    }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        if (!services_) return std::nullopt;
        if (name == "memory_set") {
            const auto key = arg(arguments, "key");
            if (key.empty()) return Domain::ToolResult::failure("missing_key", "key required");
            Domain::MemoryNote note;
            note.key = key;
            note.body = arg(arguments, "body");
            note.createdAt = Domain::iso8601(Domain::SystemClock{}.now());
            note.updatedAt = note.createdAt;
            services_->store().memorySet(note);
            return Domain::ToolResult::success({{"key", key}});
        }
        if (name == "memory_get") {
            const auto note = services_->store().memoryGet(arg(arguments, "key"));
            if (!note) return Domain::ToolResult::failure("not_found", "memory key not found");
            return Domain::ToolResult::success({{"key", note->key}, {"body", note->body}});
        }
        if (name == "memory_list") {
            const auto prefix = arg(arguments, "prefix");
            auto notes = services_->store().memoryList(prefix.empty() ? std::nullopt : std::optional<std::string>(prefix), false);
            std::ostringstream keys;
            for (const auto& note : notes) {
                if (!keys.str().empty()) keys << ',';
                keys << note.key;
            }
            return Domain::ToolResult::success({{"keys", keys.str()}, {"count", std::to_string(notes.size())}});
        }
        if (name == "memory_delete") {
            services_->store().memoryDelete(arg(arguments, "key"));
            return Domain::ToolResult::success({{"deleted", "true"}});
        }
        if (name == "memory_search") {
            auto notes = services_->store().memorySearch(arg(arguments, "query"));
            return Domain::ToolResult::success({{"count", std::to_string(notes.size())}});
        }
        return std::nullopt;
    }

private:
    ForgeServices* services_;
};

class ContinuityToolPack final : public Domain::IToolPack {
public:
    explicit ContinuityToolPack(ForgeServices* services) : services_(services) {}
    std::vector<std::string> toolNames() const override {
        return {"session_checkpoint", "session_handoff", "context_get", "context_list"};
    }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID& clientID) override {
        if (!services_) return std::nullopt;
        if (name == "session_checkpoint" || name == "session_handoff") {
            const bool finalize = name == "session_handoff";
            const auto packet = services_->continuity().checkpoint(
                clientID, arg(arguments, "goal"), arg(arguments, "narrative"), {}, finalize);
            return Domain::ToolResult::success({
                {"id", packet.id},
                {"resume_ready", finalize ? "true" : "false"},
                {"resume_seed", packet.resumeSeed},
            });
        }
        if (name == "context_get") {
            const auto id = arg(arguments, "id");
            const auto packet = id.empty() ? services_->continuity().latest(false) : services_->continuity().get(id);
            if (!packet) return Domain::ToolResult::failure("not_found", "No handoff packet");
            services_->continuityAutomation().clearBlock(clientID);
            if (packet->cwd) {
                services_->sessions().adoptWorkspace(clientID, *packet->cwd);
            }
            return Domain::ToolResult::success({
                {"id", packet->id},
                {"goal", packet->goal},
                {"narrative", packet->narrative},
                {"resume_seed", packet->resumeSeed},
            });
        }
        if (name == "context_list") {
            const auto packets = services_->continuity().list(20);
            return Domain::ToolResult::success({{"count", std::to_string(packets.size())}});
        }
        return std::nullopt;
    }

private:
    ForgeServices* services_;
};

class FilesystemToolPack final : public Domain::IToolPack {
public:
    std::vector<std::string> toolNames() const override {
        return {"fs_read", "fs_write", "fs_edit", "fs_list", "fs_glob", "fs_mkdir", "fs_delete", "fs_move"};
    }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        const auto path = resolvePath(arg(arguments, "path"));
        if (name == "fs_read") {
            std::ifstream in(path, std::ios::binary);
            if (!in) return Domain::ToolResult::failure("not_found", "Not a readable file");
            std::ostringstream buffer;
            buffer << in.rdbuf();
            auto text = buffer.str();
            if (text.size() > 2 * 1024 * 1024) {
                return Domain::ToolResult::failure("file_too_large", "Text reads are limited to 2MB");
            }
            const auto offset = arg(arguments, "offset");
            if (!offset.empty()) {
                // 1-based line window
                std::istringstream lines(text);
                std::string line;
                std::ostringstream window;
                int index = 1;
                const int start = std::max(1, std::stoi(offset));
                const int length = arg(arguments, "length").empty()
                    ? (arg(arguments, "limit").empty() ? 200 : std::stoi(arg(arguments, "limit")))
                    : std::stoi(arg(arguments, "length"));
                while (std::getline(lines, line)) {
                    if (index >= start && index < start + length) {
                        window << line << '\n';
                    }
                    ++index;
                }
                return Domain::ToolResult::success({{"path", path.string()}, {"content", window.str()}});
            }
            return Domain::ToolResult::success({{"path", path.string()}, {"content", text}});
        }
        if (name == "fs_write") {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out << arg(arguments, "content");
            return Domain::ToolResult::success({{"path", path.string()}});
        }
        if (name == "fs_edit") {
            std::ifstream in(path, std::ios::binary);
            if (!in) return Domain::ToolResult::failure("not_found", "Not a readable file");
            std::ostringstream buffer;
            buffer << in.rdbuf();
            auto text = buffer.str();
            const auto find = arg(arguments, "old_string");
            const auto pos = text.find(find);
            if (pos == std::string::npos) {
                return Domain::ToolResult::failure("not_found", "old_string not found");
            }
            text.replace(pos, find.size(), arg(arguments, "new_string"));
            std::ofstream out(path, std::ios::binary);
            out << text;
            return Domain::ToolResult::success({{"path", path.string()}});
        }
        if (name == "fs_list") {
            if (!std::filesystem::exists(path)) {
                return Domain::ToolResult::failure("not_found", "Path not found");
            }
            std::ostringstream names;
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (!names.str().empty()) names << '\n';
                names << entry.path().filename().string();
            }
            return Domain::ToolResult::success({{"path", path.string()}, {"entries", names.str()}});
        }
        if (name == "fs_glob") {
            const auto pattern = arg(arguments, "pattern");
            std::ostringstream matches;
            if (std::filesystem::exists(path)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                    const auto nameOnly = entry.path().filename().string();
                    if (pattern.empty() || nameOnly.find(pattern) != std::string::npos) {
                        if (!matches.str().empty()) matches << '\n';
                        matches << entry.path().string();
                    }
                }
            }
            return Domain::ToolResult::success({{"matches", matches.str()}});
        }
        if (name == "fs_mkdir") {
            std::filesystem::create_directories(path);
            return Domain::ToolResult::success({{"path", path.string()}});
        }
        if (name == "fs_delete") {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            return Domain::ToolResult::success({{"path", path.string()}});
        }
        if (name == "fs_move") {
            const auto dest = resolvePath(arg(arguments, "destination"));
            std::filesystem::create_directories(dest.parent_path());
            std::filesystem::rename(path, dest);
            return Domain::ToolResult::success({{"path", dest.string()}});
        }
        return std::nullopt;
    }
};

class GitToolPack final : public Domain::IToolPack {
public:
    std::vector<std::string> toolNames() const override {
        return {"git_status", "git_diff", "git_log", "git_add", "git_commit"};
    }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        if (name != "git_status" && name != "git_diff" && name != "git_log" &&
            name != "git_add" && name != "git_commit") {
            return std::nullopt;
        }
        std::string command = "git ";
        if (name == "git_status") command += "status --short";
        if (name == "git_diff") command += "diff";
        if (name == "git_log") command += "log -n 15 --oneline";
        if (name == "git_add") command += "add -- " + arg(arguments, "path");
        if (name == "git_commit") command += "commit -m \"" + arg(arguments, "message") + "\"";
        const auto cwd = arg(arguments, "cwd");
        const auto result = ProcessRunner{}.run(command, cwd.empty() ? std::nullopt : std::optional<std::string>(cwd), 30);
        if (result.exitCode != 0) {
            return Domain::ToolResult::failure("git_failed", result.stderrText);
        }
        return Domain::ToolResult::success({{"output", result.stdoutText}});
    }
};

class ShellToolPack final : public Domain::IToolPack {
public:
    std::vector<std::string> toolNames() const override { return {"shell_exec"}; }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        if (name != "shell_exec") return std::nullopt;
        const auto command = arg(arguments, "command");
        if (command.empty()) return Domain::ToolResult::failure("missing_command", "command required");
        const auto cwd = arg(arguments, "cwd");
        const auto timeout = arg(arguments, "timeout_sec");
        const int seconds = timeout.empty() ? 30 : std::stoi(timeout);
        const auto result = ProcessRunner{}.run(
            command, cwd.empty() ? std::nullopt : std::optional<std::string>(cwd), seconds);
        if (result.timedOut) {
            return Domain::ToolResult::failure("timeout", "Command timed out");
        }
        auto tool = Domain::ToolResult::success({
            {"exit_code", std::to_string(result.exitCode)},
            {"stdout", result.stdoutText},
            {"stderr", result.stderrText},
        });
        if (result.exitCode != 0) {
            tool.ok = false;
            tool.code = "exit_code";
            tool.message = result.stderrText;
        }
        return tool;
    }
};

class DocsToolPack final : public Domain::IToolPack {
public:
    std::vector<std::string> toolNames() const override { return {"pdf_write", "pdf_from_file"}; }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        PdfWriter writer;
        if (name == "pdf_write") {
            const auto path = resolvePath(arg(arguments, "path"));
            writer.writeText(path, arg(arguments, "title"), arg(arguments, "body"));
            return Domain::ToolResult::success({{"path", path.string()}});
        }
        if (name == "pdf_from_file") {
            const auto source = resolvePath(arg(arguments, "source"));
            const auto dest = resolvePath(arg(arguments, "path"));
            writer.writeFromFile(source, dest);
            return Domain::ToolResult::success({{"path", dest.string()}});
        }
        return std::nullopt;
    }
};

class SearchToolPack final : public Domain::IToolPack {
public:
    std::vector<std::string> toolNames() const override { return {"search_text"}; }
    std::optional<Domain::ToolResult> handle(
        const std::string& name,
        const std::map<std::string, std::string>& arguments,
        const Domain::ClientID&) override {
        if (name != "search_text") return std::nullopt;
        const auto root = resolvePath(arg(arguments, "path"));
        const auto query = arg(arguments, "query");
        if (query.empty()) return Domain::ToolResult::failure("missing_query", "query required");
        std::ostringstream hits;
        int count = 0;
        if (std::filesystem::exists(root)) {
            const auto searchFile = [&](const std::filesystem::path& file) {
                std::ifstream in(file, std::ios::binary);
                std::string line;
                int lineNo = 1;
                while (std::getline(in, line) && count < 200) {
                    if (line.find(query) != std::string::npos) {
                        hits << file.string() << ':' << lineNo << ':' << line << '\n';
                        ++count;
                    }
                    ++lineNo;
                }
            };
            if (std::filesystem::is_regular_file(root)) {
                searchFile(root);
            } else {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                    if (entry.is_regular_file()) {
                        searchFile(entry.path());
                    }
                }
            }
        }
        return Domain::ToolResult::success({{"count", std::to_string(count)}, {"hits", hits.str()}});
    }
};

} // namespace

std::unique_ptr<Domain::IToolPack> makeAgentToolPack(ForgeServices& services) {
    return std::make_unique<AgentToolPack>(&services);
}
std::unique_ptr<Domain::IToolPack> makeMemoryToolPack(ForgeServices& services) {
    return std::make_unique<MemoryToolPack>(&services);
}
std::unique_ptr<Domain::IToolPack> makeContinuityToolPack(ForgeServices& services) {
    return std::make_unique<ContinuityToolPack>(&services);
}
std::unique_ptr<Domain::IToolPack> makeFilesystemToolPack() {
    return std::make_unique<FilesystemToolPack>();
}
std::unique_ptr<Domain::IToolPack> makeGitToolPack() {
    return std::make_unique<GitToolPack>();
}
std::unique_ptr<Domain::IToolPack> makeShellToolPack() {
    return std::make_unique<ShellToolPack>();
}
std::unique_ptr<Domain::IToolPack> makeDocsToolPack() {
    return std::make_unique<DocsToolPack>();
}
std::unique_ptr<Domain::IToolPack> makeSearchToolPack() {
    return std::make_unique<SearchToolPack>();
}

} // namespace Forge::Orchestration
