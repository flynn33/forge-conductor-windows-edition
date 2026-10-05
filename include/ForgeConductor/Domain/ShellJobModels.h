#pragma once

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/ProcessModels.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

enum class ShellJobState { Running, Completed, Failed, Cancelled, TimedOut };

struct ShellJobSnapshot final {
    std::string jobId;
    ShellJobState state{ShellJobState::Running};
    std::string command;
    std::string cwd;
    std::uint32_t timeoutSeconds{};
    std::optional<ProcessResult> result;
    std::optional<Error> error;
    std::chrono::milliseconds elapsed{};
    std::uint32_t processId{};
    std::uint64_t processCreationTime{};
    std::string stdoutPath;
    std::string stderrPath;
    std::string receiptPath;
    std::string logHash;
    bool logTruncated{};
    std::vector<std::string> arguments;
    bool memoryAttached{};
    std::optional<Error> memoryAttachError;
    std::optional<bool> processAlive;
};

struct ShellJobLogPage final {
    std::string text;
    std::string path;
    std::uint64_t offset{};
    std::uint64_t nextOffset{};
    std::uint64_t totalBytes{};
    bool hasMore{};
    bool textLossy{};
};

} // namespace ForgeConductor::Domain
