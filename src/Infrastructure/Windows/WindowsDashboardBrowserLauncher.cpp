#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBrowserLauncher.h"

#include "Detail/UtfConversion.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <climits>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::string_view HelperMode{"--internal-open-dashboard-uri"};
constexpr std::string_view ExpectedManagerLeaf{"ForgeConductor.Manager.exe"};
constexpr std::string_view ExpectedHelperLeaf{"forge-conductor.exe"};
constexpr std::chrono::milliseconds DiagnosticTimeout{
    std::chrono::seconds{2}};

enum class LaunchState : std::uint8_t {
    Open,
    Launching,
    Complete,
    Closing,
    Closed,
};

[[nodiscard]] Domain::Error launcherError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Result<void> validateEndpoint(
    const std::string_view host,
    const std::uint16_t port)
{
    if (host != "127.0.0.1" && host != "::1") {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard browser launcher requires a canonical loopback host."));
    }
    if (port == 0U) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard browser launcher requires a nonzero port."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::string authenticatedUri(
    const std::string_view host,
    const std::uint16_t port,
    const Domain::Sha256Digest& bearerToken)
{
    std::string uri = host == "::1" ? "http://[::1]:"
                                      : "http://127.0.0.1:";
    uri += std::to_string(port);
    uri += "/#token=";
    uri += bearerToken.value();
    return uri;
}

struct PathParts final {
    std::string_view parent;
    std::string_view leaf;
};

[[nodiscard]] std::optional<PathParts> splitPath(
    const Domain::PathText& path) noexcept
{
    const auto separator = path.value().find_last_of("\\/");
    if (separator == std::string::npos || separator < 2U ||
        separator + 1U >= path.value().size()) {
        return std::nullopt;
    }
    return PathParts{
        std::string_view{path.value()}.substr(0U, separator),
        std::string_view{path.value()}.substr(separator + 1U)};
}

[[nodiscard]] bool ordinalEqualInsensitive(
    const std::string_view left,
    const std::string_view right) noexcept
{
    try {
        auto leftWide = Detail::strictUtf8ToUtf16(left);
        auto rightWide = Detail::strictUtf8ToUtf16(right);
        if (!leftWide || !rightWide ||
            leftWide.value().size() > static_cast<std::size_t>(INT_MAX) ||
            rightWide.value().size() > static_cast<std::size_t>(INT_MAX)) {
            return false;
        }
        return ::CompareStringOrdinal(
                   leftWide.value().data(),
                   static_cast<int>(leftWide.value().size()),
                   rightWide.value().data(),
                   static_cast<int>(rightWide.value().size()), TRUE) ==
               CSTR_EQUAL;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::optional<Domain::Error> validateConfiguration(
    const WindowsDashboardBrowserLaunchConfiguration& configuration) noexcept
{
    try {
        const auto manager = splitPath(configuration.managerExecutable);
        const auto helper = splitPath(configuration.helperExecutable);
        if (!manager || !helper ||
            !ordinalEqualInsensitive(manager->leaf, ExpectedManagerLeaf) ||
            !ordinalEqualInsensitive(helper->leaf, ExpectedHelperLeaf) ||
            !ordinalEqualInsensitive(manager->parent, helper->parent)) {
            return launcherError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard activation helper is not the exact CLI sibling of the Manager executable.");
        }
        const auto& authority = configuration.helperAuthority;
        if (authority.trustedRoots().size() != 1U ||
            !ordinalEqualInsensitive(
                authority.trustedRoots().front().value(), helper->parent) ||
            authority.intent() != Domain::FileAccess::Execute ||
            !authority.shellEnabled() ||
            authority.grants().size() != 1U ||
            authority.grants().front() != Domain::FileAccess::Execute ||
            !authority.denials().empty()) {
            return launcherError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard activation helper authority is not restricted to its exact executable directory.");
        }
        if (configuration.helperTimeout <=
                std::chrono::milliseconds::zero() ||
            configuration.helperTimeout >
                WindowsDashboardBrowserLauncher::MaximumHelperTimeout) {
            return launcherError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard activation helper timeout is outside its bounded range.");
        }
        return std::nullopt;
    } catch (...) {
        return launcherError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard activation helper configuration could not be validated safely.");
    }
}

[[nodiscard]] Domain::Result<void> evaluateProcessResult(
    Domain::Result<Domain::ProcessResult> result)
{
    if (!result) {
        if (result.error().code == Domain::ErrorCodes::Cancelled) {
            return Domain::Result<void>::failure(launcherError(
                Domain::ErrorCodes::Cancelled,
                "The bounded dashboard activation helper was cancelled."));
        }
        if (result.error().code == Domain::ErrorCodes::DeadlineExceeded) {
            return Domain::Result<void>::failure(launcherError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The bounded dashboard activation helper deadline expired.",
                true));
        }
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "The bounded dashboard activation helper could not be run.",
            result.error().retryable));
    }
    const auto& process = result.value();
    if (!process.terminationConfirmed) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard activation helper termination was not confirmed."));
    }
    if (process.timedOut) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The dashboard activation helper exceeded its bounded timeout.",
            true));
    }
    if (process.cancelled) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::Cancelled,
            "The dashboard activation helper was cancelled."));
    }
    if (process.exitCode != 0) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The Windows Shell did not open the Manager dashboard.",
            true));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::string_view normalizeImmediateDiagnosticCode(
    const std::string_view code) noexcept
{
    if (code == Domain::ErrorCodes::Cancelled ||
        code == Domain::ErrorCodes::Conflict ||
        code == Domain::ErrorCodes::DeadlineExceeded ||
        code == Domain::ErrorCodes::IntegrityFailure ||
        code == Domain::ErrorCodes::InternalFailure ||
        code == Domain::ErrorCodes::InvalidRequest ||
        code == Domain::ErrorCodes::TransportClosed) {
        return code;
    }
    return Domain::ErrorCodes::InternalFailure;
}

class ImmediateBrowserDiagnosticExecutor final {
public:
    static constexpr std::size_t MaximumPendingDiagnosticCount = 8U;

    ImmediateBrowserDiagnosticExecutor(
        Contracts::IClock& clock,
        Contracts::IDiagnosticSink& diagnostics)
        : clock_{clock},
          diagnostics_{diagnostics},
          worker_{[this](const std::stop_token cancellation) noexcept {
              run(cancellation);
          }}
    {
    }

    ~ImmediateBrowserDiagnosticExecutor() noexcept { shutdown(); }

    ImmediateBrowserDiagnosticExecutor(
        const ImmediateBrowserDiagnosticExecutor&) = delete;
    ImmediateBrowserDiagnosticExecutor& operator=(
        const ImmediateBrowserDiagnosticExecutor&) = delete;

    [[nodiscard]] bool enqueueFailure(
        const std::string_view code,
        const Domain::OperationContext& source) noexcept
    {
        try {
            PendingDiagnostic pending{
                std::string{normalizeImmediateDiagnosticCode(code)},
                Domain::OperationContext{
                    source.operationId,
                    clock_.monotonicNow() + DiagnosticTimeout,
                    {},
                    source.correlationId}};
            {
                const std::lock_guard lock{mutex_};
                if (closing_ ||
                    pending_.size() >= MaximumPendingDiagnosticCount) {
                    return false;
                }
                pending_.push_back(std::move(pending));
            }
            changed_.notify_all();
            return true;
        } catch (...) {
            return false;
        }
    }

    void beginShutdown() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                closing_ = true;
                if (worker_.joinable()) {
                    worker_.request_stop();
                }
            }
            changed_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool isCurrentWorker() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return workerActive_ &&
                workerThreadId_ == std::this_thread::get_id();
        } catch (...) {
            return false;
        }
    }

    void shutdown() noexcept
    {
        beginShutdown();
        std::jthread claimedWorker;
        try {
            std::unique_lock lock{mutex_};
            if (workerActive_ &&
                workerThreadId_ == std::this_thread::get_id()) {
                return;
            }
            while (joinOwned_) {
                changed_.wait(
                    lock, [this]() noexcept { return !joinOwned_; });
            }
            if (!worker_.joinable()) {
                closed_ = true;
                return;
            }
            joinOwned_ = true;
            claimedWorker = std::move(worker_);
        } catch (...) {
            return;
        }
        changed_.notify_all();

        if (claimedWorker.joinable()) {
            try {
                claimedWorker.join();
            } catch (...) {
                claimedWorker.request_stop();
            }
        }

        try {
            const std::lock_guard lock{mutex_};
            workerActive_ = false;
            workerThreadId_ = {};
            joinOwned_ = false;
            closed_ = true;
        } catch (...) {
            return;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool closedForTesting() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return closed_ && !worker_.joinable() && !joinOwned_;
        } catch (...) {
            return false;
        }
    }

private:
    struct PendingDiagnostic final {
        std::string code;
        Domain::OperationContext context;
    };

    void run(const std::stop_token cancellation) noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                workerThreadId_ = std::this_thread::get_id();
            }
            changed_.notify_all();
            std::stop_callback cancellationWake{
                cancellation, [this]() noexcept { changed_.notify_all(); }};

            for (;;) {
                std::optional<PendingDiagnostic> pending;
                {
                    std::unique_lock lock{mutex_};
                    changed_.wait(lock, [this, &cancellation]() noexcept {
                        return closing_ || cancellation.stop_requested() ||
                            !pending_.empty();
                    });
                    if (!pending_.empty()) {
                        pending.emplace(std::move(pending_.front()));
                        pending_.pop_front();
                    } else if (closing_ || cancellation.stop_requested()) {
                        break;
                    }
                }

                if (pending) {
                    static_cast<void>(diagnostics_.record(
                        Domain::DiagnosticEnvelope{
                            clock_.utcNow(),
                            "manager_dashboard_browser_open_failed",
                            Domain::DiagnosticSeverity::Warn,
                            "manager",
                            ::GetCurrentProcessId(),
                            Domain::DiagnosticCategory::Manager,
                            {{"code", pending->code}}},
                        pending->context));
                }
            }
        } catch (...) {
        }

        try {
            const std::lock_guard lock{mutex_};
            workerActive_ = false;
            workerThreadId_ = {};
        } catch (...) {
        }
        changed_.notify_all();
    }

    Contracts::IClock& clock_;
    Contracts::IDiagnosticSink& diagnostics_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<PendingDiagnostic> pending_;
    bool closing_{};
    bool joinOwned_{};
    bool closed_{};
    bool workerActive_{true};
    std::thread::id workerThreadId_{};
    std::jthread worker_;
};

} // namespace

class WindowsDashboardBrowserLauncher::Impl final {
public:
    Impl(
        Contracts::IClock& clock,
        Contracts::IDiagnosticSink& diagnostics,
        std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
        WindowsDashboardBrowserLaunchConfiguration configuration,
        Domain::Sha256Digest bearerToken)
        : clock_{clock},
          diagnostics_{diagnostics},
          processSupervisor_{std::move(processSupervisor)},
          configuration_{std::move(configuration)},
          bearerToken_{std::move(bearerToken)},
          configurationError_{validateConfiguration(configuration_)},
          immediateDiagnostics_{clock_, diagnostics_}
    {
        if (!processSupervisor_ && !configurationError_) {
            configurationError_ = launcherError(
                Domain::ErrorCodes::IntegrityFailure,
                "The dashboard browser launcher has no process supervisor.");
        }
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<void> launch(
        const std::string_view host,
        const std::uint16_t port,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (context.isCancellationRequested()) {
                return rejectWithDiagnostic(launcherError(
                    Domain::ErrorCodes::Cancelled,
                    "Dashboard browser launch was cancelled."),
                    context);
            }
            if (context.isExpired(clock_.monotonicNow())) {
                return rejectWithDiagnostic(launcherError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "Dashboard browser launch deadline expired."),
                    context);
            }
            auto endpoint = validateEndpoint(host, port);
            if (!endpoint) {
                return rejectWithDiagnostic(
                    std::move(endpoint).error(), context);
            }
            if (configurationError_) {
                return rejectWithDiagnostic(*configurationError_, context);
            }

            Domain::ProcessRequest request{
                configuration_.helperExecutable,
                {std::string{HelperMode}},
                configuration_.helperAuthority.trustedRoots().front(),
                {},
                true,
                configuration_.helperTimeout,
                1'024U,
                1'024U,
                authenticatedUri(host, port, bearerToken_)};
            Domain::OperationContext ownedContext{context};

            std::optional<Domain::Error> immediateFailure;
            {
                const std::lock_guard lock{stateMutex_};
                if (state_ == LaunchState::Closed ||
                    state_ == LaunchState::Closing) {
                    immediateFailure = launcherError(
                        Domain::ErrorCodes::TransportClosed,
                        "The dashboard browser launcher is closed.");
                } else if (state_ != LaunchState::Open) {
                    immediateFailure = launcherError(
                        Domain::ErrorCodes::Conflict,
                        "The dashboard browser launch was already attempted.");
                } else {
                    state_ = LaunchState::Launching;
                    workerActive_ = true;
                    activeOperation_.emplace(context.operationId);
                    try {
                        worker_ = std::jthread{
                            [this, request = std::move(request),
                             ownedContext = std::move(ownedContext)](
                                const std::stop_token ownerCancellation)
                                mutable noexcept {
                                publishWorkerThread();
                                runWorker(
                                    std::move(request),
                                    std::move(ownedContext),
                                    ownerCancellation);
                            }};
                    } catch (...) {
                        activeOperation_.reset();
                        workerActive_ = false;
                        state_ = LaunchState::Complete;
                        immediateFailure = launcherError(
                            Domain::ErrorCodes::InternalFailure,
                            "The dashboard browser activation worker could not start.",
                            true);
                    }
                }
            }
            if (immediateFailure) {
                return rejectWithDiagnostic(
                    std::move(*immediateFailure), context);
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return rejectWithDiagnostic(
                launcherError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard browser launch could not be admitted safely."),
                context);
        }
    }

    void beginShutdown() noexcept
    {
        std::optional<Domain::OperationId> operation;
        try {
            {
                const std::lock_guard lock{stateMutex_};
                shutdownRequested_ = true;
                if (state_ == LaunchState::Open) {
                    state_ = LaunchState::Closed;
                } else if (state_ != LaunchState::Closed) {
                    state_ = LaunchState::Closing;
                }
                if (worker_.joinable()) {
                    worker_.request_stop();
                }
                operation = activeOperation_;
            }
            if (operation && processSupervisor_) {
                processSupervisor_->cancel(*operation);
            }
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        beginShutdown();
        immediateDiagnostics_.beginShutdown();
        if (launchWorkerIsCurrent() ||
            immediateDiagnostics_.isCurrentWorker()) {
            return;
        }
        shutdownLaunchWorker();
        immediateDiagnostics_.shutdown();
    }

    [[nodiscard]] bool closedForTesting() const noexcept
    {
        bool launchWorkerClosed{};
        try {
            const std::lock_guard lock{stateMutex_};
            launchWorkerClosed = state_ == LaunchState::Closed &&
                !worker_.joinable() && !shutdownJoinOwned_;
        } catch (...) {
            return false;
        }
        return launchWorkerClosed && immediateDiagnostics_.closedForTesting();
    }

    [[nodiscard]] bool waitUntilJoinOwnedForTesting(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{stateMutex_};
            return stateChanged_.wait_for(
                lock, timeout,
                [this]() noexcept { return shutdownJoinOwned_; });
        } catch (...) {
            return false;
        }
    }

private:
    [[nodiscard]] bool launchWorkerIsCurrent() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return workerActive_ &&
                workerThreadId_ == std::this_thread::get_id();
        } catch (...) {
            return false;
        }
    }

    void shutdownLaunchWorker() noexcept
    {

        std::jthread claimedWorker;
        try {
            std::unique_lock lock{stateMutex_};
            if (state_ == LaunchState::Closed && !worker_.joinable()) {
                return;
            }
            if (workerActive_ &&
                workerThreadId_ == std::this_thread::get_id()) {
                // A callback on the activation worker may signal shutdown, but
                // only a distinct process owner may claim and join this thread.
                return;
            }
            while (shutdownJoinOwned_) {
                stateChanged_.wait(
                    lock,
                    [this]() noexcept { return !shutdownJoinOwned_; });
            }
            if (state_ == LaunchState::Closed && !worker_.joinable()) {
                return;
            }
            if (worker_.joinable()) {
                shutdownJoinOwned_ = true;
                worker_.request_stop();
                claimedWorker = std::move(worker_);
            } else {
                state_ = LaunchState::Closed;
                activeOperation_.reset();
                workerActive_ = false;
                workerThreadId_ = {};
                lock.unlock();
                stateChanged_.notify_all();
                return;
            }
        } catch (...) {
            return;
        }
        stateChanged_.notify_all();

        {
            std::jthread joiningWorker = std::move(claimedWorker);
            if (joiningWorker.joinable()) {
                try {
                    joiningWorker.join();
                } catch (...) {
                    joiningWorker.request_stop();
                }
            }
        }

        try {
            const std::lock_guard lock{stateMutex_};
            state_ = LaunchState::Closed;
            activeOperation_.reset();
            workerActive_ = false;
            workerThreadId_ = {};
            shutdownJoinOwned_ = false;
        } catch (...) {
            return;
        }
        stateChanged_.notify_all();
    }

    [[nodiscard]] Domain::Result<void> rejectWithDiagnostic(
        Domain::Error error,
        const Domain::OperationContext& context) noexcept
    {
        try {
            Domain::Error returnedError{error};
            static_cast<void>(immediateDiagnostics_.enqueueFailure(
                returnedError.code, context));
            return Domain::Result<void>::failure(std::move(returnedError));
        } catch (...) {
            return Domain::Result<void>::failure(launcherError(
                Domain::ErrorCodes::InternalFailure,
                "The immediate dashboard activation failure could not be reported safely."));
        }
    }

    [[nodiscard]] Domain::OperationContext diagnosticContext(
        const Domain::OperationContext& source) const
    {
        return Domain::OperationContext{
            source.operationId,
            clock_.monotonicNow() + DiagnosticTimeout,
            {},
            source.correlationId};
    }

    void publishWorkerThread() noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            workerThreadId_ = std::this_thread::get_id();
        } catch (...) {
        }
    }

    void completeWorker() noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            activeOperation_.reset();
            workerActive_ = false;
            state_ = shutdownRequested_ ? LaunchState::Closing
                                        : LaunchState::Complete;
        } catch (...) {
        }
        stateChanged_.notify_all();
    }

    void runWorker(
        Domain::ProcessRequest request,
        Domain::OperationContext context,
        const std::stop_token ownerCancellation) noexcept
    {
        Domain::Result<void> launched = Domain::Result<void>::failure(
            launcherError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard activation helper failed safely."));
        bool cancellationExpected{};
        try {
            std::stop_source combinedCancellation;
            std::stop_callback ownerRelay{
                ownerCancellation,
                [&combinedCancellation]() noexcept {
                    combinedCancellation.request_stop();
                }};
            std::stop_callback callerRelay{
                context.cancellation,
                [&combinedCancellation]() noexcept {
                    combinedCancellation.request_stop();
                }};
            const Domain::OperationContext processContext{
                context.operationId,
                context.deadline,
                combinedCancellation.get_token(),
                context.correlationId};
            launched = evaluateProcessResult(processSupervisor_->run(
                request, configuration_.helperAuthority, processContext));
            cancellationExpected =
                ownerCancellation.stop_requested() ||
                context.isCancellationRequested();
        } catch (...) {
            launched = Domain::Result<void>::failure(launcherError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard activation helper boundary failed safely."));
            cancellationExpected = ownerCancellation.stop_requested();
        }

        try {
            context = diagnosticContext(context);
        } catch (...) {
        }
        if (launched) {
            recordSuccess(context);
        } else if (!cancellationExpected ||
                   launched.error().code != Domain::ErrorCodes::Cancelled) {
            recordFailure(launched.error(), context);
        }

        completeWorker();
    }

    void recordFailure(
        const Domain::Error& error,
        const Domain::OperationContext& context) noexcept
    {
        try {
            static_cast<void>(diagnostics_.record(
                Domain::DiagnosticEnvelope{
                    clock_.utcNow(),
                    "manager_dashboard_browser_open_failed",
                    Domain::DiagnosticSeverity::Warn,
                    "manager",
                    ::GetCurrentProcessId(),
                    Domain::DiagnosticCategory::Manager,
                    {{"code", error.code}}},
                context));
        } catch (...) {
        }
    }

    void recordSuccess(
        const Domain::OperationContext& context) noexcept
    {
        try {
            static_cast<void>(diagnostics_.record(
                Domain::DiagnosticEnvelope{
                    clock_.utcNow(),
                    "manager_dashboard_browser_opened",
                    Domain::DiagnosticSeverity::Info,
                    "manager",
                    ::GetCurrentProcessId(),
                    Domain::DiagnosticCategory::Manager,
                    {}},
                context));
        } catch (...) {
        }
    }

    Contracts::IClock& clock_;
    Contracts::IDiagnosticSink& diagnostics_;
    const std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor_;
    const WindowsDashboardBrowserLaunchConfiguration configuration_;
    const Domain::Sha256Digest bearerToken_;
    std::optional<Domain::Error> configurationError_;
    ImmediateBrowserDiagnosticExecutor immediateDiagnostics_;

    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    LaunchState state_{LaunchState::Open};
    bool shutdownRequested_{};
    bool shutdownJoinOwned_{};
    bool workerActive_{};
    std::thread::id workerThreadId_{};
    std::optional<Domain::OperationId> activeOperation_;
    std::jthread worker_;
};

WindowsDashboardBrowserLauncher::WindowsDashboardBrowserLauncher(
    Contracts::IClock& clock,
    Contracts::IDiagnosticSink& diagnostics,
    std::shared_ptr<Contracts::IProcessSupervisor> processSupervisor,
    WindowsDashboardBrowserLaunchConfiguration configuration,
    Domain::Sha256Digest bearerToken)
    : implementation_{std::make_unique<Impl>(
          clock,
          diagnostics,
          std::move(processSupervisor),
          std::move(configuration),
          std::move(bearerToken))}
{
}

WindowsDashboardBrowserLauncher::~WindowsDashboardBrowserLauncher() noexcept
{
    shutdown();
}

Domain::Result<void> WindowsDashboardBrowserLauncher::launch(
    const std::string_view dashboardHost,
    const std::uint16_t dashboardPort,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<void>::failure(launcherError(
            Domain::ErrorCodes::TransportClosed,
            "The dashboard browser launcher is unavailable."));
    }
    return implementation_->launch(
        dashboardHost, dashboardPort, context);
}

void WindowsDashboardBrowserLauncher::beginShutdown() noexcept
{
    if (implementation_) {
        implementation_->beginShutdown();
    }
}

void WindowsDashboardBrowserLauncher::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

bool WindowsDashboardBrowserLauncher::closedForTesting() const noexcept
{
    return implementation_ && implementation_->closedForTesting();
}

bool WindowsDashboardBrowserLauncher::waitUntilJoinOwnedForTesting(
    const std::chrono::milliseconds timeout) noexcept
{
    return implementation_ &&
           implementation_->waitUntilJoinOwnedForTesting(timeout);
}

} // namespace ForgeConductor::Infrastructure::Windows
