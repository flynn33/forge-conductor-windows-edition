#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeServer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

void requireSuccess(
    Domain::Result<void> result,
    const std::string& message)
{
    if (!result) {
        fail(message + ": " + result.error().code + ": " +
             result.error().message);
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const std::string& message)
{
    require(!result, message + " unexpectedly succeeded");
    require(result.error().code == code, message + " returned " +
        result.error().code + " instead of " + std::string{code});
}

[[nodiscard]] std::string uuidText(const std::uint32_t suffix)
{
    constexpr char Digits[] = "0123456789abcdef";
    std::string value{"00000000-0000-4000-8000-000000000000"};
    auto remaining = suffix;
    for (std::size_t index{}; index < 8U; ++index) {
        value[value.size() - 1U - index] = Digits[remaining & 0xFU];
        remaining >>= 4U;
    }
    return value;
}

[[nodiscard]] Domain::OperationContext context(
    const std::uint32_t suffix,
    const std::chrono::milliseconds lifetime = 5s)
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse(uuidText(suffix))),
        std::chrono::steady_clock::now() + lifetime,
        std::stop_token{},
        take(Domain::CorrelationId::parse(
            "manager-pipe-roundtrip-" + std::to_string(suffix)))};
}

[[nodiscard]] Domain::Sha256Digest nonce(const char character)
{
    return take(Domain::Sha256Digest::parse(std::string(64U, character)));
}

[[nodiscard]] Domain::ManagerStatus statusValue()
{
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        4242U,
        std::nullopt,
        std::nullopt,
        7U,
        std::nullopt,
        true,
        3s,
        false,
        "127.0.0.1",
        7788U,
        8s,
        take(Domain::PathText::create("C:\\ManagerPipeRoundTrip")),
        "0.9.0-alpha"};
}

class RoundTripController final : public Contracts::IManagerController {
public:
    RoundTripController()
        : status_{statusValue()}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext&) noexcept override
    {
        return statusResult();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            return Domain::Result<Domain::ManagerControllerSnapshot>::success(
                Domain::ManagerControllerSnapshot{status_, shutdownRequested_});
        } catch (...) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip snapshot fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            if (blockNextStatus_) {
                statusEntered_ = true;
                condition_.notify_all();
                const std::stop_callback cancelled{
                    operation.cancellation,
                    [this] { condition_.notify_all(); }};
                condition_.wait(lock, [&] {
                    return !blockNextStatus_ ||
                        operation.isCancellationRequested();
                });
                if (operation.isCancellationRequested()) {
                    blockNextStatus_ = false;
                    cancellationObserved_ = true;
                    condition_.notify_all();
                    return Domain::Result<Domain::ManagerStatus>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Cancelled,
                            "The round-trip controller observed cancellation."));
                }
            }
            return Domain::Result<Domain::ManagerStatus>::success(status_);
        } catch (...) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip status fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            return Domain::Result<Domain::ManagerSettings>::success(settings_);
        } catch (...) {
            return Domain::Result<Domain::ManagerSettings>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip settings fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            lastControlAction_ = request.action;
            return Domain::Result<Domain::ManagerStatus>::success(status_);
        } catch (...) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip control fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto updated = Domain::applyManagerSettingsPatch(settings_, patch);
            if (!updated) {
                return Domain::Result<Domain::ManagerSettings>::failure(
                    std::move(updated).error());
            }
            settings_ = std::move(updated).value();
            lastApplyImmediately_ = applyImmediately;
            return Domain::Result<Domain::ManagerSettings>::success(settings_);
        } catch (...) {
            return Domain::Result<Domain::ManagerSettings>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip settings update fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdownRequested_ = true;
            condition_.notify_all();
            return Domain::Result<Domain::ManagerControllerSnapshot>::success(
                Domain::ManagerControllerSnapshot{status_, true});
        } catch (...) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip shutdown fake failed."));
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        ++closeCalls_;
        condition_.notify_all();
    }

    void armBlockedStatus() noexcept
    {
        std::lock_guard lock{mutex_};
        blockNextStatus_ = true;
        statusEntered_ = false;
        cancellationObserved_ = false;
    }

    [[nodiscard]] bool waitForStatusEntered(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [&] { return statusEntered_; });
    }

    [[nodiscard]] bool waitForCancellation(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [&] { return cancellationObserved_; });
    }

    [[nodiscard]] bool waitForShutdownRequest(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [&] { return shutdownRequested_; });
    }

    [[nodiscard]] Domain::ManagerControlAction lastControlAction() const noexcept
    {
        std::lock_guard lock{mutex_};
        return lastControlAction_;
    }

    [[nodiscard]] bool lastApplyImmediately() const noexcept
    {
        std::lock_guard lock{mutex_};
        return lastApplyImmediately_;
    }

    [[nodiscard]] std::size_t closeCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return closeCalls_;
    }

private:
    [[nodiscard]] Domain::Result<Domain::ManagerStatus> statusResult() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return Domain::Result<Domain::ManagerStatus>::success(status_);
        } catch (...) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The round-trip status fake failed."));
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Domain::ManagerStatus status_;
    Domain::ManagerSettings settings_;
    Domain::ManagerControlAction lastControlAction_{
        Domain::ManagerControlAction::Start};
    bool lastApplyImmediately_{};
    bool blockNextStatus_{};
    bool statusEntered_{};
    bool cancellationObserved_{};
    bool shutdownRequested_{};
    std::size_t closeCalls_{};
};

class RunningServer final {
public:
    explicit RunningServer(
        std::unique_ptr<Infrastructure::WindowsManagerNamedPipeServer> server)
        : server_{std::move(server)},
          completionFuture_{completion_.get_future()}
    {
    }

    ~RunningServer() noexcept
    {
        server_->shutdown();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    void start(Domain::OperationContext operation)
    {
        thread_ = std::jthread{[this, operation = std::move(operation)](
                                   std::stop_token) mutable noexcept {
            completion_.set_value(server_->run(operation));
        }};
    }

    [[nodiscard]] Domain::Result<void> await(
        const std::chrono::milliseconds timeout)
    {
        if (completionFuture_.wait_for(timeout) != std::future_status::ready) {
            fail("The manager named-pipe server did not stop within its bound.");
        }
        auto result = completionFuture_.get();
        if (thread_.joinable()) {
            thread_.join();
        }
        return result;
    }

private:
    std::unique_ptr<Infrastructure::WindowsManagerNamedPipeServer> server_;
    std::promise<Domain::Result<void>> completion_;
    std::future<Domain::Result<void>> completionFuture_;
    std::jthread thread_;
};

[[nodiscard]] std::wstring uniquePipeName()
{
    return L"\\\\.\\pipe\\ForgeConductor.Manager.RoundTrip." +
        std::to_wstring(::GetCurrentProcessId()) + L"." +
        std::to_wstring(::GetTickCount64());
}

void requirePipeNameReleased(const std::wstring& pipeName)
{
    const HANDLE probe = ::CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1U,
        4096U,
        4096U,
        0U,
        nullptr);
    require(
        probe != nullptr && probe != INVALID_HANDLE_VALUE,
        "The manager named-pipe server left a pipe instance behind.");
    require(::CloseHandle(probe) != FALSE, "The pipe residue probe did not close.");
}

void testAuthenticatedNamedPipeRoundTrip()
{
    auto clock = std::make_shared<Infrastructure::SystemClock>();
    auto controller = std::make_shared<RoundTripController>();
    Manager::ManagerTransportLimits limits;
    limits.maximumRequestLifetime = 30s;
    limits.connectTimeout = 1s;
    limits.shutdownDrainTimeout = 2s;

    auto dispatcher = std::make_shared<Manager::ManagerRequestDispatcher>(
        controller, clock, limits);
    const auto pipeName = uniquePipeName();
    const auto validNonce = nonce('a');

    Infrastructure::WindowsManagerNamedPipeServerOptions options;
    options.pipeName = pipeName;
    options.limits = limits;
    options.workerCount = 4U;
    options.ingressTimeout = 1s;

    RunningServer server{take(
        Infrastructure::WindowsManagerNamedPipeServer::create(
            clock,
            dispatcher,
            take(Infrastructure::WindowsCurrentUserIdentity::load()),
            validNonce,
            options))};
    server.start(context(1U, 30s));

    auto client = take(Infrastructure::WindowsManagerNamedPipeClient::create(
        clock, pipeName, validNonce, limits));

    const auto status = take(client->status(context(2U)));
    require(
        status.ok && status.isManager && status.processId == 4242U &&
            status.dashboardPort == 7788U,
        "The manager status did not complete a typed pipe round trip.");

    const auto settings = take(client->settings(context(3U)));
    require(
        settings.dashboardHost == "127.0.0.1" &&
            settings.dashboardPort == 7788U,
        "The manager settings did not complete a typed pipe round trip.");

    const auto controlled = take(client->control(
        Domain::ManagerControlRequest{Domain::ManagerControlAction::Restart},
        context(4U)));
    require(controlled.processId == 4242U, "The control result was malformed.");
    require(
        controller->lastControlAction() ==
            Domain::ManagerControlAction::Restart,
        "The manager control payload was not dispatched.");

    Domain::ManagerSettingsPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(8899U);
    const auto updated = take(client->updateSettings(patch, true, context(5U)));
    require(
        updated.dashboardPort == 8899U &&
            controller->lastApplyImmediately(),
        "The manager settings update was not dispatched.");

    auto wrongNonceClient = take(
        Infrastructure::WindowsManagerNamedPipeClient::create(
            clock, pipeName, nonce('b'), limits));
    requireError(
        wrongNonceClient->status(context(6U)),
        Domain::ErrorCodes::Unauthorized,
        "The wrong manager nonce");
    require(
        take(client->status(context(7U))).processId == 4242U,
        "A rejected nonce damaged later authenticated requests.");
    wrongNonceClient->shutdown();

    controller->armBlockedStatus();
    const auto cancellationStart = std::chrono::steady_clock::now();
    const auto cancelled = client->status(context(8U, 200ms));
    const auto cancellationElapsed = std::chrono::steady_clock::now() -
        cancellationStart;
    requireError(
        cancelled,
        Domain::ErrorCodes::DeadlineExceeded,
        "The bounded manager request");
    require(
        cancellationElapsed < 750ms,
        "Best-effort cancellation extended the public caller deadline.");
    require(
        controller->waitForStatusEntered(2s),
        "The cancellation fixture did not enter the controller.");
    require(
        controller->waitForCancellation(3s),
        "The asynchronous manager cancellation was not observed.");
    require(
        dispatcher->waitUntilIdle(2s),
        "The cancelled manager request did not drain.");

    requireSuccess(
        client->requestShutdown(context(9U, 5s)),
        "The remote manager shutdown acknowledgement was lost");
    require(
        controller->waitForShutdownRequest(2s),
        "The remote shutdown did not reach the manager controller.");
    requireSuccess(
        server.await(5s),
        "The manager server did not exit cleanly after remote shutdown");

    client->shutdown();
    dispatcher->shutdown();
    require(
        controller->closeCalls() == 1U,
        "The manager controller did not close exactly once.");
    requirePipeNameReleased(pipeName);
}

} // namespace

int main()
{
    try {
        testAuthenticatedNamedPipeRoundTrip();
        std::cout << "Manager named-pipe round-trip tests passed: 1 group\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager named-pipe round-trip tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
