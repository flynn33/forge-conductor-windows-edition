#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h"

#include "Detail/UtfConversion.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"

#include <Windows.h>
#include <Shellapi.h>
#include <TlHelp32.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_{value} {}
    ~UniqueHandle() noexcept
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(value_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}
    UniqueHandle& operator=(UniqueHandle&&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{};
};

[[nodiscard]] Domain::Error nativeError(
    const std::string_view action,
    const DWORD nativeCode)
{
    return Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        std::string{action} + " failed with Windows error " + std::to_string(nativeCode) + ".");
}

[[nodiscard]] bool sameWindowsPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                  right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

class WindowsLMStudioHostPlatform final : public Contracts::ILMStudioHostPlatform {
public:
    explicit WindowsLMStudioHostPlatform(Contracts::IClock& clock) noexcept
        : clock_{clock}
    {
    }

    [[nodiscard]] Domain::Result<bool> isRunning(
        const Domain::PathText& applicationExecutable,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            if (isCancelled(context)) {
                return Domain::Result<bool>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "LM Studio process inspection was cancelled."));
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return Domain::Result<bool>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "LM Studio process inspection deadline expired."));
            }
            auto expected = Detail::strictUtf8ToUtf16(applicationExecutable.value());
            if (!expected) {
                return Domain::Result<bool>::failure(std::move(expected).error());
            }
            UniqueHandle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
            if (!snapshot.valid()) {
                return Domain::Result<bool>::failure(
                    nativeError("LM Studio process enumeration", ::GetLastError()));
            }
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!::Process32FirstW(snapshot.get(), &entry)) {
                const auto error = ::GetLastError();
                if (error == ERROR_NO_MORE_FILES) {
                    return Domain::Result<bool>::success(false);
                }
                return Domain::Result<bool>::failure(
                    nativeError("LM Studio process enumeration", error));
            }
            do {
                if (isCancelled(context)) {
                    return Domain::Result<bool>::failure(Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "LM Studio process inspection was cancelled."));
                }
                UniqueHandle process{::OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
                if (!process.valid()) {
                    continue;
                }
                std::wstring image(32U * 1024U, L'\0');
                DWORD size = static_cast<DWORD>(image.size());
                if (::QueryFullProcessImageNameW(process.get(), 0, image.data(), &size)) {
                    image.resize(size);
                    if (sameWindowsPath(image, expected.value())) {
                        return Domain::Result<bool>::success(true);
                    }
                }
            } while (::Process32NextW(snapshot.get(), &entry));
            const auto terminalError = ::GetLastError();
            if (terminalError != ERROR_NO_MORE_FILES) {
                return Domain::Result<bool>::failure(
                    nativeError("LM Studio process enumeration", terminalError));
            }
            return Domain::Result<bool>::success(false);
        } catch (...) {
            return Domain::Result<bool>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio process state could not be inspected."));
        }
    }

    [[nodiscard]] Domain::Result<void> launch(
        const Domain::PathText& applicationExecutable,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock cancellationLock{cancellationMutex_};
            if (isCancelledLocked(context)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "LM Studio launch was cancelled."));
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "LM Studio launch deadline expired."));
            }
            auto executable = Detail::strictUtf8ToUtf16(applicationExecutable.value());
            if (!executable) {
                return Domain::Result<void>::failure(std::move(executable).error());
            }
            SHELLEXECUTEINFOW launch{};
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
            launch.lpVerb = L"open";
            launch.lpFile = executable.value().c_str();
            launch.nShow = SW_SHOWNORMAL;
            if (!::ShellExecuteExW(&launch)) {
                return Domain::Result<void>::failure(
                    nativeError("supported LM Studio launch", ::GetLastError()));
            }
            UniqueHandle launchedProcess{launch.hProcess};
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio could not be launched through ShellExecuteExW."));
        }
    }

    [[nodiscard]] Domain::Result<void> waitForObservation(
        const std::chrono::milliseconds maximumWait,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            if (isCancelled(context)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "LM Studio host observation was cancelled."));
            }
            std::stop_callback cancellationWake{
                context.cancellation,
                [this] { waitCondition_.notify_all(); }};
            std::unique_lock lock{waitMutex_};
            waitCondition_.wait_for(lock, maximumWait, [&] {
                return isCancelled(context);
            });
            if (isCancelled(context)) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "LM Studio host observation was cancelled."));
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "LM Studio host observation deadline expired."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio host observation wait failed."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            {
                std::scoped_lock lock{cancellationMutex_};
                cancelledOperation_ = operationId;
            }
            waitCondition_.notify_all();
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        {
            std::scoped_lock lock{cancellationMutex_};
            closed_ = true;
        }
        waitCondition_.notify_all();
    }

private:
    [[nodiscard]] bool isCancelledLocked(
        const Domain::OperationContext& context) const noexcept
    {
        return closed_ || context.isCancellationRequested() ||
            (cancelledOperation_ && cancelledOperation_.value() == context.operationId);
    }

    [[nodiscard]] bool isCancelled(
        const Domain::OperationContext& context) noexcept
    {
        std::scoped_lock lock{cancellationMutex_};
        return isCancelledLocked(context);
    }

    Contracts::IClock& clock_;
    std::mutex cancellationMutex_;
    bool closed_{};
    std::optional<Domain::OperationId> cancelledOperation_;
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
};

[[nodiscard]] Domain::Result<Domain::PathText> parentPath(
    const Domain::PathText& path) noexcept
{
    try {
        const auto separator = path.value().find_last_of("\\/");
        if (separator == std::string::npos || separator == 0U) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio configuration path has no usable parent directory."));
        }
        return Domain::PathText::create(path.value().substr(0U, separator));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio configuration parent path could not be derived."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> synchronizedConfigurationPath(
    const Domain::PathText& configurationPath) noexcept
{
    auto root = parentPath(configurationPath);
    if (!root) {
        return Domain::Result<Domain::PathText>::failure(std::move(root).error());
    }
    return Domain::PathText::create(
        root.value().value() + "\\.internal\\last-synced-mcp-state.json");
}

[[nodiscard]] std::string textOf(const std::vector<std::byte>& bytes)
{
    std::string text(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(text.data(), bytes.data(), bytes.size());
    }
    return text;
}

[[nodiscard]] std::optional<std::string> jsonString(
    const Json& object,
    const std::string_view name)
{
    const auto member = object.find(name);
    if (member == object.end() || !member->is_string()) {
        return std::nullopt;
    }
    return member->get<std::string>();
}

[[nodiscard]] bool synchronizedRole(
    const Json& servers,
    const std::string_view serverName,
    const std::string_view role,
    const Domain::DeploymentId& deploymentId,
    std::optional<std::string>& sharedCommand,
    std::optional<std::string>& sharedHome)
{
    const auto entry = servers.find(serverName);
    if (entry == servers.end() || !entry->is_object()) {
        return false;
    }
    const auto command = jsonString(*entry, "command");
    const auto args = entry->find("args");
    const auto environment = entry->find("env");
    if (!command || command->empty() || args == entry->end() || !args->is_array() ||
        args->size() != 1U || !(*args)[0].is_string() ||
        (*args)[0].get<std::string>() != "serve" ||
        environment == entry->end() || !environment->is_object()) {
        return false;
    }
    const auto configuredRole = jsonString(*environment, "FORGE_MCP_ROLE");
    const auto configuredRevision = jsonString(*environment, "FORGE_DEPLOYMENT_ID");
    const auto forgeHome = jsonString(*environment, "FORGE_CONDUCTOR_HOME");
    if (!configuredRole || *configuredRole != role ||
        !configuredRevision || *configuredRevision != deploymentId.value() ||
        !forgeHome || forgeHome->empty()) {
        return false;
    }
    if (sharedCommand && *sharedCommand != *command) {
        return false;
    }
    if (sharedHome && *sharedHome != *forgeHome) {
        return false;
    }
    sharedCommand = std::move(command);
    sharedHome = std::move(forgeHome);
    return true;
}

struct ForgeConfigurationState final {
    std::string command;
    std::string forgeHome;
    Domain::DeploymentId deploymentId;

    bool operator==(const ForgeConfigurationState&) const = default;
};

[[nodiscard]] std::optional<ForgeConfigurationState> forgeConfigurationState(
    const std::vector<std::byte>& content,
    const Domain::DeploymentId& deploymentId) noexcept
{
    try {
        if (content.empty() || content.size() > LMStudioConfigurationCodec::MaximumDocumentBytes) {
            return std::nullopt;
        }
        const auto strict = LMStudioConfigurationCodec::parse(content);
        if (!strict) {
            return std::nullopt;
        }
        const auto root = Json::parse(strict.value().sourceUtf8(), nullptr, true, false);
        if (!root.is_object()) {
            return std::nullopt;
        }
        const auto servers = root.find("mcpServers");
        if (servers == root.end() || !servers->is_object()) {
            return std::nullopt;
        }
        std::optional<std::string> sharedCommand;
        std::optional<std::string> sharedHome;
        if (!synchronizedRole(*servers, LMStudioFallbackServerId, "fallback",
                              deploymentId, sharedCommand, sharedHome) ||
            !synchronizedRole(*servers, LMStudioPrimaryServerId, "primary",
                              deploymentId, sharedCommand, sharedHome) ||
            !sharedCommand || !sharedHome) {
            return std::nullopt;
        }
        return ForgeConfigurationState{
            std::move(sharedCommand).value(),
            std::move(sharedHome).value(),
            deploymentId};
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool hasSynchronizedConfiguration(
    const std::vector<std::byte>& content,
    const ForgeConfigurationState& liveConfiguration) noexcept
{
    const auto synchronized = forgeConfigurationState(
        content, liveConfiguration.deploymentId);
    return synchronized && synchronized.value() == liveConfiguration;
}

} // namespace

class WindowsLMStudioHostActivator::Impl final {
public:
    Impl(
        Contracts::IWorkspaceAuthority& workspaceAuthorityValue,
        Contracts::IFileSystem& fileSystemValue,
        Contracts::IClock& clockValue,
        std::unique_ptr<Contracts::ILMStudioHostPlatform> hostPlatformValue,
        WindowsLMStudioHostActivatorOptions optionsValue)
        : workspaceAuthority{workspaceAuthorityValue},
          fileSystem{fileSystemValue},
          clock{clockValue},
          hostPlatform{std::move(hostPlatformValue)},
          options{optionsValue}
    {
    }

    [[nodiscard]] bool isCancelled(const Domain::OperationContext& context) noexcept
    {
        if (context.isCancellationRequested()) {
            return true;
        }
        {
            std::scoped_lock lock{lifecycleMutex};
            if (closed) {
                return true;
            }
        }
        std::scoped_lock cancellationLock{cancellationMutex};
        return cancelledOperation && cancelledOperation.value() == context.operationId;
    }

    [[nodiscard]] bool begin(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::scoped_lock lock{lifecycleMutex};
            if (closed || activeOperation) {
                return false;
            }
            activeOperation = operationId;
            return true;
        } catch (...) {
            return false;
        }
    }

    void complete() noexcept
    {
        try {
            {
                std::scoped_lock lock{lifecycleMutex};
                activeOperation.reset();
            }
            lifecycleChanged.notify_all();
        } catch (...) {
        }
    }

    Contracts::IWorkspaceAuthority& workspaceAuthority;
    Contracts::IFileSystem& fileSystem;
    Contracts::IClock& clock;
    std::unique_ptr<Contracts::ILMStudioHostPlatform> hostPlatform;
    const WindowsLMStudioHostActivatorOptions options;
    std::mutex lifecycleMutex;
    std::condition_variable lifecycleChanged;
    bool closed{};
    std::optional<Domain::OperationId> activeOperation;
    std::mutex cancellationMutex;
    std::optional<Domain::OperationId> cancelledOperation;
};

WindowsLMStudioHostActivator::WindowsLMStudioHostActivator(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    Contracts::IClock& clock,
    WindowsLMStudioHostActivatorOptions options)
    : WindowsLMStudioHostActivator(
          workspaceAuthority,
          fileSystem,
          clock,
          std::make_unique<WindowsLMStudioHostPlatform>(clock),
          options)
{
}

WindowsLMStudioHostActivator::WindowsLMStudioHostActivator(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    Contracts::IClock& clock,
    std::unique_ptr<Contracts::ILMStudioHostPlatform> hostPlatform,
    WindowsLMStudioHostActivatorOptions options)
    : implementation_{std::make_shared<Impl>(
          workspaceAuthority,
          fileSystem,
          clock,
          std::move(hostPlatform),
          options)}
{
}

WindowsLMStudioHostActivator::~WindowsLMStudioHostActivator() noexcept
{
    shutdown();
}

Domain::Result<Domain::LMStudioHostActivationResult> WindowsLMStudioHostActivator::activate(
    const Domain::LMStudioEnvironmentStatus& environment,
    const Domain::LMStudioHostActivationRequest& request,
    const Contracts::WorkspaceAuthority& executionAuthority,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    try {
        if (!implementation->begin(context.operationId)) {
            std::scoped_lock lock{implementation->lifecycleMutex};
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                implementation->closed
                    ? Domain::makeError(
                          Domain::ErrorCodes::Cancelled,
                          "LM Studio host activator is shut down.")
                    : Domain::makeError(
                          Domain::ErrorCodes::Conflict,
                          "Another LM Studio host activation is already in progress.",
                          true));
        }
        struct ActiveReset final {
            Impl& implementation;
            ~ActiveReset() noexcept { implementation.complete(); }
        } activeReset{*implementation};

        std::stop_callback callerCancellation{
            context.cancellation,
            [implementation, operationId = context.operationId] {
                {
                    std::scoped_lock lock{implementation->cancellationMutex};
                    implementation->cancelledOperation = operationId;
                }
                implementation->hostPlatform->cancel(operationId);
            }};

        if (implementation->isCancelled(context)) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "LM Studio host activation was cancelled."));
        }
        if (request.timeout <= std::chrono::milliseconds::zero() ||
            request.deploymentId.value().empty()) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "LM Studio activation requires a revision and positive timeout."));
        }
        if (implementation->options.observationInterval <=
                std::chrono::milliseconds::zero() ||
            implementation->options.maximumSynchronizedConfigurationBytes == 0U ||
            implementation->options.maximumSynchronizedConfigurationBytes >
                LMStudioConfigurationCodec::MaximumDocumentBytes) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "LM Studio activation options have invalid timing or document bounds."));
        }
        if (executionAuthority.intent() != Domain::FileAccess::Execute) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Unauthorized,
                                  "LM Studio activation requires Execute authority."));
        }
        if (!environment.lmStudioPresent || !environment.configurationPath ||
            !environment.applicationExecutable) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                                  "LM Studio application or configuration discovery evidence is missing."));
        }

        auto synchronizedPath = synchronizedConfigurationPath(
            environment.configurationPath.value());
        if (!synchronizedPath) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(synchronizedPath).error());
        }
        auto configurationRoot = parentPath(environment.configurationPath.value());
        if (!configurationRoot) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(configurationRoot).error());
        }
        auto executableAuthorization = implementation->workspaceAuthority.authorize(
            executionAuthority,
            Domain::PathAuthorizationRequest{
                environment.applicationExecutable.value(), std::nullopt,
                Domain::FileAccess::Execute, false},
            context);
        if (!executableAuthorization) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(executableAuthorization).error());
        }
        const auto canonicalExecutable =
            executableAuthorization.value().canonicalPath();

        auto liveAuthorization = implementation->workspaceAuthority.authorize(
            executionAuthority,
            Domain::PathAuthorizationRequest{
                environment.configurationPath.value(), configurationRoot.value(),
                Domain::FileAccess::Read, false},
            context);
        if (!liveAuthorization) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(liveAuthorization).error());
        }
        const auto readLiveConfiguration = [&]()
            -> Domain::Result<ForgeConfigurationState> {
            auto content = implementation->fileSystem.readFile(
                liveAuthorization.value(),
                implementation->options.maximumSynchronizedConfigurationBytes,
                context);
            if (!content) {
                return Domain::Result<ForgeConfigurationState>::failure(
                    std::move(content).error());
            }
            auto state = forgeConfigurationState(
                content.value(), request.deploymentId);
            if (!state) {
                return Domain::Result<ForgeConfigurationState>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The selected live LM Studio configuration does not contain both exact Forge roles at the requested revision."));
            }
            return Domain::Result<ForgeConfigurationState>::success(
                std::move(state).value());
        };
        auto liveConfiguration = readLiveConfiguration();
        if (!liveConfiguration) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(liveConfiguration).error());
        }

        auto running = implementation->hostPlatform->isRunning(
            canonicalExecutable, context);
        if (!running) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(running).error());
        }
        const bool runningBefore = running.value();
        bool launched{};
        if (!runningBefore) {
            if (implementation->isCancelled(context)) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "LM Studio host activation was cancelled before launch."));
            }
            auto launch = implementation->hostPlatform->launch(
                canonicalExecutable, context);
            if (!launch) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    std::move(launch).error());
            }
            launched = true;
        }

        const auto now = implementation->clock.monotonicNow();
        auto effectiveDeadline = context.deadline;
        if (!context.isExpired(now)) {
            const auto contextRemaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    context.deadline - now);
            if (request.timeout < contextRemaining) {
                effectiveDeadline = now + request.timeout;
            }
        }
        while (implementation->clock.monotonicNow() < effectiveDeadline) {
            if (implementation->isCancelled(context)) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    Domain::makeError(Domain::ErrorCodes::Cancelled,
                                      "LM Studio host activation was cancelled."));
            }
            liveConfiguration = readLiveConfiguration();
            if (!liveConfiguration) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    std::move(liveConfiguration).error());
            }
            auto readAuthorization = implementation->workspaceAuthority.authorize(
                executionAuthority,
                Domain::PathAuthorizationRequest{
                    synchronizedPath.value(), configurationRoot.value(),
                    Domain::FileAccess::Read, false},
                context);
            if (readAuthorization) {
                auto content = implementation->fileSystem.readFile(
                    readAuthorization.value(),
                    implementation->options.maximumSynchronizedConfigurationBytes,
                    context);
                if (content && hasSynchronizedConfiguration(
                                   content.value(), liveConfiguration.value())) {
                    return Domain::Result<Domain::LMStudioHostActivationResult>::success(
                        Domain::LMStudioHostActivationResult{
                            request.deploymentId,
                            runningBefore,
                            launched,
                            false,
                            true,
                            {},
                            launched
                                ? "LM Studio launched and synchronized both MCP registrations; hosted roles remain lazy until selected by a chat."
                                : "LM Studio hot-synchronized both MCP registrations; hosted roles remain lazy until selected by a chat."});
                }
                if (!content && content.error().code != Domain::ErrorCodes::RecordNotFound) {
                    return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                        std::move(content).error());
                }
            } else if (readAuthorization.error().code != Domain::ErrorCodes::RecordNotFound) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    std::move(readAuthorization).error());
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                effectiveDeadline - implementation->clock.monotonicNow());
            if (remaining <= std::chrono::milliseconds::zero()) {
                break;
            }
            auto waited = implementation->hostPlatform->waitForObservation(
                (std::min)(implementation->options.observationInterval, remaining), context);
            if (!waited) {
                return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                    std::move(waited).error());
            }
        }
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(Domain::makeError(
            Domain::ErrorCodes::AcknowledgementTimeout,
            runningBefore
                ? "LM Studio was running but did not acknowledge the exact committed MCP revision; no unsupported restart was attempted."
                : "LM Studio launched but did not acknowledge the exact committed MCP revision.",
            true));
    } catch (...) {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio host activation failed at its exception boundary."));
    }
}

void WindowsLMStudioHostActivator::cancel(
    const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    try {
        {
            std::scoped_lock lock{implementation->cancellationMutex};
            implementation->cancelledOperation = operationId;
        }
        implementation->hostPlatform->cancel(operationId);
    } catch (...) {
    }
}

void WindowsLMStudioHostActivator::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (!implementation) {
        return;
    }
    std::optional<Domain::OperationId> activeOperation;
    {
        std::scoped_lock lock{implementation->lifecycleMutex};
        implementation->closed = true;
        activeOperation = implementation->activeOperation;
    }
    if (activeOperation) {
        {
            std::scoped_lock lock{implementation->cancellationMutex};
            implementation->cancelledOperation = activeOperation;
        }
        implementation->hostPlatform->cancel(activeOperation.value());
    }
    implementation->hostPlatform->shutdown();
    std::unique_lock lock{implementation->lifecycleMutex};
    static_cast<void>(implementation->lifecycleChanged.wait_for(
        lock,
        std::chrono::seconds{30},
        [&] { return !implementation->activeOperation; }));
}

} // namespace ForgeConductor::Infrastructure::Windows
