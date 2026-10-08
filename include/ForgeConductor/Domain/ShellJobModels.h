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

struct CMakeTestRequest final {
    PathText buildDirectory;
    bool build{};
    std::optional<std::string> target;
    std::optional<std::string> filter;
    std::optional<std::string> configuration;
    std::chrono::milliseconds timeout{1'800'000};
};

struct CMakeTestCounts final {
    std::uint64_t tests{};
    std::uint64_t passed{};
    std::uint64_t failed{};
    std::uint64_t skipped{};
    std::uint64_t disabled{};
    bool operator==(const CMakeTestCounts&) const = default;
};

struct CMakeTestFailure final {
    std::string name;
    std::string status;
    std::string message;
    std::string output;
    bool outputTruncated{};
};

struct CMakeTestMetadata final {
    std::string buildDirectory;
    bool buildRequested{};
    std::optional<std::string> target;
    std::optional<std::string> filter;
    std::optional<std::string> configuration;
    std::string reportPath;
    std::string reportSha256;
    std::uint64_t reportBytes{};
    std::optional<ProcessResult> buildResult;
    std::optional<ProcessResult> testResult;
    std::optional<CMakeTestCounts> counts;
    std::optional<Error> reportError;
    bool reportUnverified{};
};

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
    std::optional<CMakeTestMetadata> cmakeTest;
};

struct CMakeTestRunStatus final {
    ShellJobSnapshot job;
    std::vector<CMakeTestFailure> failures;
    std::uint64_t failureOffset{};
    std::uint64_t nextFailureOffset{};
    std::uint64_t totalFailures{};
    bool hasMore{};
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
