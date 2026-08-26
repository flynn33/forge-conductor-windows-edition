#pragma once

#include "Result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace forge
{
    enum class ContinuityState
    {
        idle,
        checkpointPreparing,
        checkpointPersisted,
        successorCreating,
        successorCreated,
        bootstrapSending,
        acknowledged,
        predecessorSealing,
        completed,
        retryWait,
        failedRecoverable,
        cancelling,
        cancelled
    };

    struct Handoff final
    {
        std::string handoffId;
        std::string operationId;
        std::string projectId;
        std::string predecessorSessionId;
        std::optional<std::string> successorSessionId;
        std::string canonicalJson;
        std::string sha256;
    };

    class IContinuityCoordinator
    {
    public:
        virtual ~IContinuityCoordinator() = default;

        virtual Result<Handoff> checkpoint(
            std::string const& projectId,
            std::string const& predecessorSessionId,
            std::string const& mission,
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<void*> requestRollover(
            std::string const& projectId,
            std::string const& operationId,
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<void*> recoverIncompleteOperations(
            std::chrono::steady_clock::time_point deadline) = 0;

        virtual Result<void*> resetProjectContinuity(
            std::string const& projectId,
            std::string const& confirmation) = 0;

        virtual void shutdown() noexcept = 0;
    };
}
