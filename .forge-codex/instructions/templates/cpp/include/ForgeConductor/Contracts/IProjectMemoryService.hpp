#pragma once

#include "Result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace forge
{
    struct ProjectDescriptor final
    {
        std::string id;
        std::string displayName;
        std::optional<std::string> repositoryIdentity;
        std::vector<std::wstring> aliases;
    };

    struct MemoryWrite final
    {
        std::string kind;
        std::string title;
        std::string summary;
        std::optional<std::string> body;
        std::vector<std::string> tags;
        std::optional<std::string> sessionId;
        double importance{};
        double confidence{};
        std::optional<std::string> idempotencyKey;
    };

    class IProjectMemoryService
    {
    public:
        virtual ~IProjectMemoryService() = default;

        virtual Result<ProjectDescriptor> initialize(
            std::wstring const& projectPath,
            std::optional<std::string> const& requestedProjectId,
            std::optional<std::string> const& displayName,
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<std::string> remember(
            std::string const& projectId,
            MemoryWrite const& write,
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<std::vector<std::string>> rememberBatch(
            std::string const& projectId,
            std::span<MemoryWrite const> writes,
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<void*> closeProject(std::string const& projectId) = 0;
        virtual Result<void*> resetProjectMemory(std::string const& projectId, std::string const& confirmation) = 0;
        virtual void shutdown() noexcept = 0;
    };
}
