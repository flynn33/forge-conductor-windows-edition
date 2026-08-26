#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"

#include "NativeToolValidation.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

[[nodiscard]] Domain::Result<void> validateCommandEnvelope(
    const Domain::ProcessRequest& request,
    const Domain::PathText& exactPowerShellExecutable) noexcept
{
    try {
        if (request.executable != exactPowerShellExecutable) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request executable does not match the injected PowerShell path."));
        }
        if (request.arguments.size() != 1U || request.arguments.front().empty()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request must contain exactly one nonempty command string."));
        }
        const auto& command = request.arguments.front();
        if (command.size() > WindowsShellService::MaximumCommandBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The PowerShell command exceeds 4096 UTF-8 bytes."));
        }
        if (command.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(command)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The PowerShell command must be valid NUL-free UTF-8."));
        }
        if (!request.workingDirectory) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The shell request requires an authorized working directory."));
        }
        if (request.timeout <= std::chrono::milliseconds::zero()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The PowerShell timeout must be positive."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The PowerShell command envelope could not be validated."));
    }
}

void enforceOutputBounds(
    Domain::ProcessResult& result,
    const std::size_t maximumStdoutBytes,
    const std::size_t maximumStderrBytes)
{
    const auto truncate = [](std::string& value, const std::size_t maximumBytes) {
        if (value.size() <= maximumBytes) {
            return false;
        }
        std::size_t boundary = maximumBytes;
        while (boundary > 0U && boundary < value.size() &&
               (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
            --boundary;
        }
        value.resize(boundary);
        return true;
    };
    result.stdoutTruncated =
        truncate(result.stdoutUtf8, maximumStdoutBytes) ||
        result.stdoutTruncated;
    result.stderrTruncated =
        truncate(result.stderrUtf8, maximumStderrBytes) ||
        result.stderrTruncated;
}

} // namespace

class WindowsShellService::Impl final {
public:
    struct ActiveOperation final {
        Domain::OperationId operationId;
        std::shared_ptr<std::stop_source> cancellation;
        bool cancellationRequested{};
    };

    Impl(
        Domain::PathText powerShellExecutable,
        Contracts::WorkspaceAuthority executionAuthority,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor)
        : powerShellExecutable{std::move(powerShellExecutable)},
          executionAuthority{std::move(executionAuthority)},
          processSupervisor{std::move(processSupervisor)}
    {
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<std::stop_source>> admit(
        const Domain::OperationId& operationId) noexcept
    {
        try {
            auto cancellation = std::make_shared<std::stop_source>();
            std::scoped_lock lock{stateMutex};
            if (shutdownRequested) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The shell service is shutting down."));
            }
            if (std::find_if(
                    activeOperations.begin(), activeOperations.end(),
                    [&](const ActiveOperation& active) {
                        return active.operationId == operationId;
                    }) != activeOperations.end()) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "The shell operation identifier is already active."));
            }
            if (activeOperations.size() >=
                Contracts::IProcessSupervisor::MaximumConcurrentOperations) {
                return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RateLimited,
                        "The shell operation concurrency bound has been reached.",
                        true));
            }
            activeOperations.push_back(ActiveOperation{
                operationId, cancellation, false});
            return Domain::Result<std::shared_ptr<std::stop_source>>::success(
                std::move(cancellation));
        } catch (...) {
            return Domain::Result<std::shared_ptr<std::stop_source>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The shell operation could not be admitted."));
        }
    }

    [[nodiscard]] bool release(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::scoped_lock lock{stateMutex};
            const auto match = std::find_if(
                activeOperations.begin(), activeOperations.end(),
                [&](const ActiveOperation& active) {
                    return active.operationId == operationId;
                });
            if (match == activeOperations.end()) {
                return true;
            }
            const auto cancelled = shutdownRequested ||
                match->cancellationRequested ||
                match->cancellation->stop_requested();
            activeOperations.erase(match);
            return cancelled;
        } catch (...) {
            return true;
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        std::shared_ptr<std::stop_source> cancellation;
        try {
            {
                std::scoped_lock lock{stateMutex};
                const auto match = std::find_if(
                    activeOperations.begin(), activeOperations.end(),
                    [&](const ActiveOperation& active) {
                        return active.operationId == operationId;
                    });
                if (match != activeOperations.end()) {
                    match->cancellationRequested = true;
                    cancellation = match->cancellation;
                }
            }
        } catch (...) {
            return;
        }
        if (cancellation) {
            cancellation->request_stop();
        }
        if (cancellation && processSupervisor) {
            processSupervisor->cancel(operationId);
        }
    }

    void shutdown() noexcept
    {
        std::vector<ActiveOperation> operations;
        try {
            {
                std::scoped_lock lock{stateMutex};
                if (shutdownRequested) {
                    return;
                }
                shutdownRequested = true;
                for (auto& operation : activeOperations) {
                    operation.cancellationRequested = true;
                }
                operations = activeOperations;
            }
            for (const auto& operation : operations) {
                operation.cancellation->request_stop();
            }
            if (processSupervisor) {
                for (const auto& operation : operations) {
                    processSupervisor->cancel(operation.operationId);
                }
            }
        } catch (...) {
        }
    }

    const Domain::PathText powerShellExecutable;
    const Contracts::WorkspaceAuthority executionAuthority;
    const std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor;
    std::mutex stateMutex;
    std::vector<ActiveOperation> activeOperations;
    bool shutdownRequested{};
};

WindowsShellService::WindowsShellService(
    Domain::PathText powerShellExecutable,
    Contracts::WorkspaceAuthority executionAuthority,
    std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor)
    : implementation_{std::make_shared<Impl>(
          std::move(powerShellExecutable),
          std::move(executionAuthority),
          std::move(processSupervisor))}
{
}

WindowsShellService::~WindowsShellService()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<Domain::ProcessResult> WindowsShellService::execute(
    const Domain::ProcessRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    if (!implementation) {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The PowerShell service is no longer available."));
    }
    bool admitted{};
    try {
        auto active = Detail::checkContext(context, "Shell operation");
        if (!active) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(active).error());
        }
        auto boundAuthority =
            Detail::validateBoundAuthority(
                implementation->executionAuthority, authority);
        if (!boundAuthority) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(boundAuthority).error());
        }
        if (!authority.shellEnabled()) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::ShellDisabled,
                    "PowerShell execution is disabled by workspace authority."));
        }
        if (!Detail::containsAccess(
                authority.grants(), Domain::FileAccess::Execute) ||
            Detail::containsAccess(
                authority.denials(), Domain::FileAccess::Execute)) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "Workspace authority does not grant PowerShell execution."));
        }
        if (!implementation->processSupervisor) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The shell service requires a process supervisor owner."));
        }
        auto executable = Detail::validateExecutable(
            implementation->powerShellExecutable,
            implementation->executionAuthority,
            "PowerShell");
        if (!executable) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(executable).error());
        }
        auto envelope = validateCommandEnvelope(
            request, implementation->powerShellExecutable);
        if (!envelope) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(envelope).error());
        }
        auto workingDirectory = Detail::validateWorkingDirectory(
            request.workingDirectory.value(), authority);
        if (!workingDirectory) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(workingDirectory).error());
        }
        auto executableWorkingDirectory = Detail::validateWorkingDirectory(
            request.workingDirectory.value(),
            implementation->executionAuthority);
        if (!executableWorkingDirectory) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(executableWorkingDirectory).error());
        }

        Domain::ProcessRequest normalized{
            implementation->powerShellExecutable};
        normalized.arguments = {
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            request.arguments.front()};
        normalized.workingDirectory = request.workingDirectory;
        normalized.environment = request.environment;
        normalized.inheritEnvironment = request.inheritEnvironment;
        normalized.timeout = (std::min)(request.timeout, MaximumTimeout);
        normalized.maximumStdoutBytes =
            (std::min)(request.maximumStdoutBytes, MaximumOutputBytes);
        normalized.maximumStderrBytes =
            (std::min)(request.maximumStderrBytes, MaximumErrorBytes);

        auto admission = implementation->admit(context.operationId);
        if (!admission) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(admission).error());
        }
        auto operationCancellation = std::move(admission).value();
        admitted = true;
        std::stop_callback callerCancellationBridge{
            context.cancellation,
            [operationCancellation]() noexcept {
                operationCancellation->request_stop();
            }};
        const Domain::OperationContext supervisorContext{
            context.operationId,
            context.deadline,
            operationCancellation->get_token(),
            context.correlationId};
        auto outcome = implementation->processSupervisor->run(
            normalized, implementation->executionAuthority, supervisorContext);
        const auto cancelledLocally =
            implementation->release(context.operationId);
        admitted = false;
        if (!outcome) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(outcome).error());
        }
        auto result = std::move(outcome).value();
        if (cancelledLocally && !result.cancelled) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The PowerShell operation was cancelled before completion."));
        }
        enforceOutputBounds(
            result,
            normalized.maximumStdoutBytes,
            normalized.maximumStderrBytes);
        return Domain::Result<Domain::ProcessResult>::success(std::move(result));
    } catch (...) {
        if (admitted) {
            static_cast<void>(implementation->release(context.operationId));
        }
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The PowerShell request could not be executed."));
    }
}

void WindowsShellService::cancel(
    const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancel(operationId);
    }
}

void WindowsShellService::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::NativeTools::Windows
