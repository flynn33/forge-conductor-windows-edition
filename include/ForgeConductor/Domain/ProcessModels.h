#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string_view>
#include <string>
#include <vector>

namespace ForgeConductor::Domain {

inline constexpr std::size_t MaximumProcessArgumentCount = 256U;
inline constexpr std::size_t MaximumProcessArgumentBytes = 4U * 1024U;
inline constexpr std::size_t MaximumProcessArgumentsBytes = 15U * 1024U;
inline constexpr std::size_t MaximumProcessEnvironmentVariableCount = 128U;
inline constexpr std::size_t MaximumProcessEnvironmentNameBytes = 128U;
inline constexpr std::size_t MaximumProcessEnvironmentValueBytes = 4U * 1024U;
inline constexpr std::size_t MaximumProcessEnvironmentBytes = 24U * 1024U;
inline constexpr std::size_t MaximumProcessStdinBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t MaximumConcurrentProcessOperations = 64U;
inline constexpr std::size_t MaximumProcessCommandLineUtf16CodeUnitsIncludingTerminator = 32'767U;
inline constexpr std::size_t MaximumProcessEnvironmentBlockUtf16CodeUnitsIncludingTerminators =
    32'767U;

// Aggregate byte limits cover raw UTF-8 argument and explicit environment
// payloads. The Windows adapter must also bound the final quoted UTF-16 command
// line and environment block.

struct EnvironmentVariable final {
    std::string name;
    std::string value;
};

class IProcessOutputObserver {
public:
    virtual ~IProcessOutputObserver() = default;
    virtual void onStarted(std::uint32_t processId, std::uint64_t creationTime) noexcept = 0;
    virtual void onOutput(std::string_view bytes, bool stderrStream) noexcept = 0;
};

struct ProcessRequest final {
    PathText executable;
    std::vector<std::string> arguments;
    std::optional<PathText> workingDirectory;
    std::vector<EnvironmentVariable> environment;
    bool inheritEnvironment{false};
    std::chrono::milliseconds timeout{30'000};
    std::size_t maximumStdoutBytes{80'000};
    std::size_t maximumStderrBytes{20'000};
    std::string stdinUtf8;
    // Set only by the tracked shell-job owner, never by ordinary shell execution.
    bool managedJob{false};
    std::shared_ptr<IProcessOutputObserver> outputObserver;
};

struct ProcessResult final {
    std::int32_t exitCode{};
    std::string stdoutUtf8;
    std::string stderrUtf8;
    bool timedOut{};
    bool cancelled{};
    bool stdoutTruncated{};
    bool stderrTruncated{};
    bool terminationConfirmed{true};
    std::chrono::milliseconds elapsed{};
};

[[nodiscard]] Result<void> validateProcessRequest(const ProcessRequest& request,
                                                  const ResourceBudgets& budgets);

} // namespace ForgeConductor::Domain
