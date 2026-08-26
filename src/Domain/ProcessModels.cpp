#include "ForgeConductor/Domain/ProcessModels.h"

namespace ForgeConductor::Domain {

Result<void> validateProcessRequest(
    const ProcessRequest& request,
    const ResourceBudgets& budgets)
{
    const auto maximumTimeout = std::chrono::seconds{budgets.shellTimeoutSecondsMaximum};
    if (request.timeout <= std::chrono::milliseconds::zero() ||
        request.timeout > maximumTimeout) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Process timeout must be positive and within the active resource budget."));
    }
    if (request.maximumStdoutBytes > budgets.toolStdoutBytesMaximum ||
        request.maximumStderrBytes > budgets.toolStderrBytesMaximum) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Process output limits exceed the active resource budget."));
    }
    if (request.arguments.size() > MaximumProcessArgumentCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Process argument count exceeds 256."));
    }
    if (request.environment.size() > MaximumProcessEnvironmentVariableCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Process environment variable count exceeds 128."));
    }

    std::size_t argumentBytes = 0U;
    for (const auto& argument : request.arguments) {
        if (argument.find('\0') != std::string::npos) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Process arguments must not contain NUL."));
        }
        if (argument.size() > MaximumProcessArgumentBytes) {
            return Result<void>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "A process argument exceeds 4096 UTF-8 bytes."));
        }
        if (argument.size() > MaximumProcessArgumentsBytes - argumentBytes) {
            return Result<void>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "Process arguments exceed 15360 aggregate UTF-8 bytes."));
        }
        argumentBytes += argument.size();
    }

    std::size_t environmentBytes = 0U;
    for (const auto& variable : request.environment) {
        if (variable.name.empty() || variable.name.find('=') != std::string::npos ||
            variable.name.find('\0') != std::string::npos ||
            variable.value.find('\0') != std::string::npos) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Environment variable names must be nonempty, omit '=', and names and values "
                "must not contain NUL."));
        }
        if (variable.name.size() > MaximumProcessEnvironmentNameBytes ||
            variable.value.size() > MaximumProcessEnvironmentValueBytes) {
            return Result<void>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "A process environment name or value exceeds its UTF-8 byte limit."));
        }
        const auto variableBytes = variable.name.size() + variable.value.size();
        if (variableBytes > MaximumProcessEnvironmentBytes - environmentBytes) {
            return Result<void>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "Process environment exceeds 24576 aggregate UTF-8 payload bytes."));
        }
        environmentBytes += variableBytes;
    }

    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
