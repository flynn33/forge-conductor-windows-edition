#include "ForgeConductor/Contracts/IManagerStartupService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerStartupService.h"
#include "Infrastructure/Windows/Detail/IWindowsTaskSchedulerStartupPlatform.h"
#include "Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.h"
#include "Infrastructure/TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

struct WindowsManagerStartupServiceTestAccess final {
    [[nodiscard]] static std::unique_ptr<WindowsManagerStartupService> create(
        WindowsManagerStartupServiceOptions options,
        std::shared_ptr<IWindowsTaskSchedulerStartupPlatform> platform)
    {
        return std::unique_ptr<WindowsManagerStartupService>{
            new WindowsManagerStartupService{
                std::move(options), std::move(platform)}};
    }
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

namespace Windows = Infrastructure::Windows;
namespace Detail = Infrastructure::Windows::Detail;

using Interface = Contracts::IManagerStartupService;
using Service = Windows::WindowsManagerStartupService;

using namespace std::chrono_literals;

using InspectMember = Domain::Result<Domain::ManagerStartupStatus> (
    Service::*)(
        const Domain::ManagerStartupDefinition&,
        const Domain::OperationContext&) noexcept;
using MutationMember = Domain::Result<Domain::ManagerStartupOutcome> (
    Service::*)(
        const Domain::ManagerStartupDefinition&,
        const Domain::OperationContext&) noexcept;
using EnablementMember = Domain::Result<Domain::ManagerStartupOutcome> (
    Service::*)(
        const Domain::ManagerStartupDefinition&,
        bool,
        const Domain::OperationContext&) noexcept;
using CancelMember = void (Service::*)(
    const Domain::OperationId&) noexcept;
using ShutdownMember = void (Service::*)() noexcept;

static_assert(std::is_final_v<Service>);
static_assert(std::is_final_v<Windows::WindowsManagerStartupServiceOptions>);
static_assert(std::is_base_of_v<Interface, Service>);
static_assert(std::has_virtual_destructor_v<Interface>);
static_assert(!std::is_copy_constructible_v<Service>);
static_assert(!std::is_copy_assignable_v<Service>);
static_assert(!std::is_move_constructible_v<Service>);
static_assert(!std::is_move_assignable_v<Service>);
static_assert(std::is_same_v<decltype(&Service::inspect), InspectMember>);
static_assert(
    std::is_same_v<decltype(&Service::registerAtLogon), MutationMember>);
static_assert(std::is_same_v<decltype(&Service::repair), MutationMember>);
static_assert(std::is_same_v<decltype(&Service::setEnabled), EnablementMember>);
static_assert(std::is_same_v<decltype(&Service::startNow), MutationMember>);
static_assert(std::is_same_v<decltype(&Service::remove), MutationMember>);
static_assert(std::is_same_v<decltype(&Service::cancel), CancelMember>);
static_assert(std::is_same_v<decltype(&Service::shutdown), ShutdownMember>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::ManagerStartupDefinition startupDefinition()
{
    return Domain::ManagerStartupDefinition{
        path("C:\\Forge Conductor\\ForgeConductor.Manager.exe"),
        path("C:\\Forge Conductor\\Home")};
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operation,
    const Domain::MonotonicTimePoint deadline,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operation),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>(
            "manager-startup-service-facade-test")};
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operation)
{
    return context(operation, std::chrono::steady_clock::now() + 5min);
}

[[nodiscard]] Windows::WindowsManagerStartupServiceOptions options(
    std::string suffix)
{
    Windows::WindowsManagerStartupServiceOptions value;
    value.purposeSuffix = std::move(suffix);
    return value;
}

class FakeStartupPlatform final
    : public Detail::IWindowsTaskSchedulerStartupPlatform {
public:
    void makeExact(const bool enabled) noexcept
    {
        exists_ = true;
        enabled_ = enabled;
        running_ = false;
        drifted_ = false;
    }

    void makeDrifted(const bool enabled) noexcept
    {
        makeExact(enabled);
        drifted_ = true;
    }

    [[nodiscard]] Domain::Result<Detail::ManagerStartupResolvedRegistration>
    resolve(
        const Domain::ManagerStartupDefinition& expected,
        const std::string_view purposeSuffix,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++resolveCalls;
            lastPurposeSuffix = std::string{purposeSuffix};
            if (resolveFailure.has_value()) {
                return Domain::Result<
                    Detail::ManagerStartupResolvedRegistration>::failure(
                    *resolveFailure);
            }
            auto identity = Windows::WindowsCurrentUserIdentity::load();
            if (!identity) {
                return Domain::Result<
                    Detail::ManagerStartupResolvedRegistration>::failure(
                    std::move(identity).error());
            }
            return Detail::ManagerStartupDefinitionBuilder::build(
                expected,
                identity.value(),
                purposeSuffix);
        } catch (const std::exception& exception) {
            return Domain::Result<
                Detail::ManagerStartupResolvedRegistration>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Detail::ManagerStartupResolvedRegistration>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The service facade fake could not resolve its registration."));
        }
    }

    [[nodiscard]] Domain::Result<Manager::ManagerStartupTaskObservation>
    inspect(
        const Detail::ManagerStartupResolvedRegistration& registration,
        const Domain::OperationContext&) noexcept override
    {
        try {
            ++inspectCalls;
            if (inspectFailure.has_value()) {
                return Domain::Result<
                    Manager::ManagerStartupTaskObservation>::failure(
                    *inspectFailure);
            }
            if (!exists_) {
                return Domain::Result<
                    Manager::ManagerStartupTaskObservation>::success({});
            }

            Manager::ManagerStartupTaskObservation observation;
            observation.exists = true;
            observation.launchProjectionComplete = true;
            observation.registrationIdentity =
                "ForgeConductor.Manager.Startup.ServiceFacadeTest";
            observation.definition = registration.definition;
            if (drifted_) {
                observation.definition->actions.front().arguments +=
                    " --service-facade-drift";
            }
            observation.enabled = enabled_;
            observation.running = running_;
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::success(
                std::move(observation));
        } catch (const std::exception& exception) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The service facade fake could not inspect its registration."));
        }
    }

    [[nodiscard]] Domain::Result<void> registerCanonical(
        const Detail::ManagerStartupResolvedRegistration&,
        const Detail::ManagerStartupRegistrationMutation mutation,
        const bool enabled,
        const Domain::OperationContext&) noexcept override
    {
        ++registerCalls;
        if (mutationFailure.has_value()) {
            return Domain::Result<void>::failure(*mutationFailure);
        }
        if ((mutation ==
                 Detail::ManagerStartupRegistrationMutation::CreateMissing &&
             exists_) ||
            (mutation ==
                 Detail::ManagerStartupRegistrationMutation::ReplaceOwned &&
             !exists_)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The service facade fake received the wrong registration mutation."));
        }
        if (!firstRegistrationMutation.has_value()) {
            firstRegistrationMutation = mutation;
        }
        lastRegistrationMutation = mutation;
        exists_ = true;
        enabled_ = enabled;
        running_ = false;
        drifted_ = false;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> setEnabled(
        const Detail::ManagerStartupResolvedRegistration&,
        const bool enabled,
        const Domain::OperationContext&) noexcept override
    {
        ++setEnabledCalls;
        if (mutationFailure.has_value()) {
            return Domain::Result<void>::failure(*mutationFailure);
        }
        enabled_ = enabled;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> startNow(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        ++startCalls;
        if (mutationFailure.has_value()) {
            return Domain::Result<void>::failure(*mutationFailure);
        }
        running_ = true;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        ++removeCalls;
        if (mutationFailure.has_value()) {
            return Domain::Result<void>::failure(*mutationFailure);
        }
        exists_ = false;
        enabled_ = false;
        running_ = false;
        drifted_ = false;
        return Domain::Result<void>::success();
    }

    std::size_t resolveCalls{};
    std::size_t inspectCalls{};
    std::size_t registerCalls{};
    std::size_t setEnabledCalls{};
    std::size_t startCalls{};
    std::size_t removeCalls{};
    std::string lastPurposeSuffix;
    std::optional<Detail::ManagerStartupRegistrationMutation>
        firstRegistrationMutation;
    std::optional<Detail::ManagerStartupRegistrationMutation>
        lastRegistrationMutation;
    std::optional<Domain::Error> resolveFailure;
    std::optional<Domain::Error> inspectFailure;
    std::optional<Domain::Error> mutationFailure;

private:
    bool exists_{};
    bool enabled_{};
    bool running_{};
    bool drifted_{};
};

class BlockingInspectPlatform final
    : public Detail::IWindowsTaskSchedulerStartupPlatform {
public:
    [[nodiscard]] Domain::Result<Detail::ManagerStartupResolvedRegistration>
    resolve(
        const Domain::ManagerStartupDefinition& expected,
        const std::string_view purposeSuffix,
        const Domain::OperationContext&) noexcept override
    {
        try {
            auto identity = Windows::WindowsCurrentUserIdentity::load();
            if (!identity) {
                return Domain::Result<
                    Detail::ManagerStartupResolvedRegistration>::failure(
                    std::move(identity).error());
            }
            return Detail::ManagerStartupDefinitionBuilder::build(
                expected, identity.value(), purposeSuffix);
        } catch (const std::exception& exception) {
            return Domain::Result<
                Detail::ManagerStartupResolvedRegistration>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Detail::ManagerStartupResolvedRegistration>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The blocking service fake could not resolve its registration."));
        }
    }

    [[nodiscard]] Domain::Result<Manager::ManagerStartupTaskObservation>
    inspect(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            std::stop_callback wake{
                operationContext.cancellation,
                [this]() noexcept { changed_.notify_all(); }};
            std::unique_lock lock{mutex_};
            ++entered_;
            changed_.notify_all();
            changed_.wait(lock, [this, &operationContext]() noexcept {
                return released_ ||
                    operationContext.isCancellationRequested();
            });
            if (operationContext.isCancellationRequested()) {
                ++cancellationObservations_;
                return Domain::Result<
                    Manager::ManagerStartupTaskObservation>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The blocking startup inspection was cancelled."));
            }
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::success({});
        } catch (const std::exception& exception) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    exception.what()));
        } catch (...) {
            return Domain::Result<
                Manager::ManagerStartupTaskObservation>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The blocking service fake could not inspect its registration."));
        }
    }

    [[nodiscard]] Domain::Result<void> registerCanonical(
        const Detail::ManagerStartupResolvedRegistration&,
        Detail::ManagerStartupRegistrationMutation,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> setEnabled(
        const Detail::ManagerStartupResolvedRegistration&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> startNow(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Detail::ManagerStartupResolvedRegistration&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool waitForEntered(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            timeout,
            [this, count]() noexcept { return entered_ >= count; });
    }

    void release() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            released_ = true;
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::size_t cancellationObservations() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return cancellationObservations_;
        } catch (...) {
            return 0U;
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t entered_{};
    std::size_t cancellationObservations_{};
    bool released_{};
};

[[nodiscard]] std::unique_ptr<Service> createService(
    std::string purposeSuffix,
    std::shared_ptr<Detail::IWindowsTaskSchedulerStartupPlatform> platform)
{
    return Detail::WindowsManagerStartupServiceTestAccess::create(
        options(std::move(purposeSuffix)),
        std::move(platform));
}

void serviceRetainsFinalInterfaceShape()
{
    require(
        std::is_base_of_v<Interface, Service> &&
            std::is_final_v<Service>,
        "the Windows Manager startup facade lost its final interface shape");
}

void inspectMapsStatusAndPurposeSuffix()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    platform->makeExact(true);
    auto service = createService("service-test", platform);

    const auto status = take(service->inspect(
        startupDefinition(),
        context("20000000-0000-4000-8000-000000000001")));

    require(
        status.state == Domain::ManagerStartupState::Ready &&
            status.registered && status.enabled &&
            status.definitionMatches,
        "inspect did not map the worker status response through the public facade");
    require(
        platform->resolveCalls == 1U &&
            platform->inspectCalls == 1U &&
            platform->lastPurposeSuffix == "service-test",
        "the facade did not carry its purpose suffix through per-operation resolution");
}

void publicMutationsMapOutcomes()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    auto service = createService("mutation-test", platform);
    const auto expected = startupDefinition();

    const auto registered = take(service->registerAtLogon(
        expected,
        context("20000000-0000-4000-8000-000000000002")));
    platform->makeDrifted(false);
    const auto repaired = take(service->repair(
        expected,
        context("20000000-0000-4000-8000-000000000003")));
    const auto enabled = take(service->setEnabled(
        expected,
        true,
        context("20000000-0000-4000-8000-000000000004")));
    const auto started = take(service->startNow(
        expected,
        context("20000000-0000-4000-8000-000000000005")));
    const auto removed = take(service->remove(
        expected,
        context("20000000-0000-4000-8000-000000000006")));

    require(
        registered.changed &&
            registered.status.state == Domain::ManagerStartupState::Ready &&
            platform->registerCalls == 2U &&
            platform->firstRegistrationMutation ==
                Detail::ManagerStartupRegistrationMutation::CreateMissing,
        "registerAtLogon did not map its mutation outcome");
    require(
        repaired.changed &&
            repaired.status.state == Domain::ManagerStartupState::Disabled &&
            platform->lastRegistrationMutation ==
                Detail::ManagerStartupRegistrationMutation::ReplaceOwned,
        "repair did not map its ReplaceOwned outcome");
    require(
        enabled.changed &&
            enabled.status.state == Domain::ManagerStartupState::Ready &&
            platform->setEnabledCalls == 1U,
        "setEnabled did not map its mutation outcome");
    require(
        started.changed && started.status.running &&
            platform->startCalls == 1U,
        "startNow did not map its mutation outcome");
    require(
        removed.changed &&
            removed.status.state == Domain::ManagerStartupState::Missing &&
            platform->removeCalls == 1U,
        "remove did not map its mutation outcome");
}

void nullPlatformInitializationFailsDeterministically()
{
    auto service = createService("null-platform-test", nullptr);
    const auto expected = startupDefinition();
    const auto first = service->inspect(
        expected,
        context("20000000-0000-4000-8000-000000000007"));
    const auto second = service->registerAtLogon(
        expected,
        context("20000000-0000-4000-8000-000000000008"));

    requireError(
        first,
        Domain::ErrorCodes::IntegrityFailure,
        "a null Task Scheduler platform initialized the public facade");
    requireError(
        second,
        Domain::ErrorCodes::IntegrityFailure,
        "a null Task Scheduler platform produced a nondeterministic mutation error");
    require(
        first.error() == second.error(),
        "null-platform initialization did not retain one deterministic error");
    service->shutdown();
    service->shutdown();
}

void shutdownIsIdempotentAndClosesAdmission()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    auto service = createService("shutdown-test", platform);

    service->shutdown();
    service->shutdown();
    service->cancel(parse<Domain::OperationId>(
        "20000000-0000-4000-8000-000000000009"));
    const auto result = service->inspect(
        startupDefinition(),
        context("20000000-0000-4000-8000-000000000010"));

    requireError(
        result,
        Domain::ErrorCodes::TransportClosed,
        "the public facade admitted work after idempotent shutdown");
    require(
        platform->resolveCalls == 0U && platform->inspectCalls == 0U,
        "closed facade admission reached the Task Scheduler platform");
}

void preCancelledAndExpiredContextsDoNotReachThePlatform()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    auto service = createService("context-test", platform);
    const auto expected = startupDefinition();
    std::stop_source cancellation;
    static_cast<void>(cancellation.request_stop());

    const auto cancelled = service->inspect(
        expected,
        context(
            "20000000-0000-4000-8000-000000000011",
            std::chrono::steady_clock::now() + 5min,
            cancellation.get_token()));
    const auto expired = service->inspect(
        expected,
        context(
            "20000000-0000-4000-8000-000000000012",
            std::chrono::steady_clock::now() - 1ms));

    requireError(
        cancelled,
        Domain::ErrorCodes::Cancelled,
        "a pre-cancelled service request reached the Task Scheduler platform");
    requireError(
        expired,
        Domain::ErrorCodes::DeadlineExceeded,
        "an expired service request reached the Task Scheduler platform");
    require(
        platform->resolveCalls == 0U && platform->inspectCalls == 0U,
        "invalid operation contexts crossed the service worker boundary");
}

void platformFailuresPropagateThroughTheService()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    auto service = createService("platform-error-test", platform);
    const auto expected = startupDefinition();

    const auto resolveFailure = Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "scripted startup resolution failure");
    platform->resolveFailure = resolveFailure;
    const auto resolveResult = service->inspect(
        expected,
        context("20000000-0000-4000-8000-000000000013"));
    requireError(
        resolveResult,
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "the service did not propagate a startup resolution failure");
    require(
        resolveResult.error() == resolveFailure,
        "the service rewrote the startup resolution failure");

    platform->resolveFailure.reset();
    const auto inspectFailure = Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        "scripted startup inspection failure");
    platform->inspectFailure = inspectFailure;
    const auto inspectResult = service->inspect(
        expected,
        context("20000000-0000-4000-8000-000000000014"));
    requireError(
        inspectResult,
        Domain::ErrorCodes::IntegrityFailure,
        "the service did not propagate a startup inspection failure");
    require(
        inspectResult.error() == inspectFailure,
        "the service rewrote the startup inspection failure");

    platform->inspectFailure.reset();
    const auto mutationFailure = Domain::makeError(
        Domain::ErrorCodes::OwnershipConflict,
        "scripted startup mutation failure");
    platform->mutationFailure = mutationFailure;
    const auto mutationResult = service->registerAtLogon(
        expected,
        context("20000000-0000-4000-8000-000000000015"));
    requireError(
        mutationResult,
        Domain::ErrorCodes::OwnershipConflict,
        "the service did not propagate a startup mutation failure");
    require(
        mutationResult.error() == mutationFailure,
        "the service rewrote the startup mutation failure");
}

void invalidPurposeSuffixFailsBeforeTaskSchedulerInspection()
{
    auto platform = std::make_shared<FakeStartupPlatform>();
    auto service = createService("unsafe.suffix", platform);

    const auto result = service->inspect(
        startupDefinition(),
        context("20000000-0000-4000-8000-000000000016"));

    requireError(
        result,
        Domain::ErrorCodes::InvalidRequest,
        "the service accepted an unsafe startup task purpose suffix");
    require(
        platform->resolveCalls == 1U && platform->inspectCalls == 0U &&
            platform->lastPurposeSuffix == "unsafe.suffix",
        "an invalid purpose suffix crossed the resolved-registration boundary");
}

void activeCallerCancellationPropagatesToThePlatform()
{
    auto platform = std::make_shared<BlockingInspectPlatform>();
    auto service = createService("caller-cancel-test", platform);
    std::stop_source cancellation;

    auto operation = std::async(
        std::launch::async,
        [&service, token = cancellation.get_token()]() {
            return service->inspect(
                startupDefinition(),
                context(
                    "20000000-0000-4000-8000-000000000017",
                    std::chrono::steady_clock::now() + 5min,
                    token));
        });
    const bool entered = platform->waitForEntered(1U, 5s);
    const bool requested = cancellation.request_stop();
    platform->release();
    const auto result = operation.get();

    require(
        entered,
        "the active service request did not reach the blocking platform");
    require(
        requested,
        "the caller cancellation source was already stopped unexpectedly");
    requireError(
        result,
        Domain::ErrorCodes::Cancelled,
        "active caller-token cancellation did not cancel the service request");
    require(
        platform->cancellationObservations() == 1U,
        "active caller-token cancellation did not reach the platform context");
}

void queueCapacityAndShutdownLifetimeRemainBounded()
{
    auto platform = std::make_shared<BlockingInspectPlatform>();
    auto service = createService("queue-shutdown-test", platform);

    auto active = std::async(std::launch::async, [&service]() {
        return service->inspect(
            startupDefinition(),
            context("20000000-0000-4000-8000-000000000018"));
    });
    const bool activeEntered = platform->waitForEntered(1U, 5s);

    auto second = std::async(std::launch::async, [&service]() {
        return service->inspect(
            startupDefinition(),
            context("20000000-0000-4000-8000-000000000019"));
    });
    auto third = std::async(std::launch::async, [&service]() {
        return service->inspect(
            startupDefinition(),
            context("20000000-0000-4000-8000-000000000020"));
    });

    const auto admissionDeadline = std::chrono::steady_clock::now() + 5s;
    bool overloadSurfaced{};
    while (std::chrono::steady_clock::now() < admissionDeadline) {
        overloadSurfaced =
            second.wait_for(0ms) == std::future_status::ready ||
            third.wait_for(0ms) == std::future_status::ready;
        if (overloadSurfaced) {
            break;
        }
        std::this_thread::yield();
    }

    auto shutdown = std::async(
        std::launch::async,
        [&service]() noexcept { service->shutdown(); });
    static_cast<void>(shutdown.wait_for(100ms));
    platform->release();
    shutdown.get();

    const auto activeResult = active.get();
    const auto secondResult = second.get();
    const auto thirdResult = third.get();
    const auto hasCode = [](
                             const Domain::Result<
                                 Domain::ManagerStartupStatus>& result,
                             const std::string_view code) noexcept {
        return !result && result.error().code == code;
    };
    const std::size_t overloadCount =
        static_cast<std::size_t>(hasCode(
            secondResult, Domain::ErrorCodes::LimitExceeded)) +
        static_cast<std::size_t>(hasCode(
            thirdResult, Domain::ErrorCodes::LimitExceeded));
    const std::size_t queuedCancellationCount =
        static_cast<std::size_t>(hasCode(
            secondResult, Domain::ErrorCodes::Cancelled)) +
        static_cast<std::size_t>(hasCode(
            thirdResult, Domain::ErrorCodes::Cancelled));

    require(
        activeEntered,
        "the queue-cap test did not establish an active platform call");
    require(
        overloadSurfaced,
        "the service did not surface its bounded queue cap promptly");
    requireError(
        activeResult,
        Domain::ErrorCodes::Cancelled,
        "shutdown did not cancel and drain the active service call");
    require(
        overloadCount == 1U && queuedCancellationCount == 1U,
        "the service did not retain exactly one active and one queued operation");
    require(
        platform->cancellationObservations() == 1U,
        "shutdown did not propagate cancellation to exactly the active platform call");
}

} // namespace

void registerWindowsManagerStartupServiceTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup.service.shape",
        serviceRetainsFinalInterfaceShape);
    addTest(
        tests,
        "manager_startup.service.inspect",
        inspectMapsStatusAndPurposeSuffix);
    addTest(
        tests,
        "manager_startup.service.mutations",
        publicMutationsMapOutcomes);
    addTest(
        tests,
        "manager_startup.service.null-platform",
        nullPlatformInitializationFailsDeterministically);
    addTest(
        tests,
        "manager_startup.service.shutdown",
        shutdownIsIdempotentAndClosesAdmission);
    addTest(
        tests,
        "manager_startup.service.context",
        preCancelledAndExpiredContextsDoNotReachThePlatform);
    addTest(
        tests,
        "manager_startup.service.platform-errors",
        platformFailuresPropagateThroughTheService);
    addTest(
        tests,
        "manager_startup.service.invalid-suffix",
        invalidPurposeSuffixFailsBeforeTaskSchedulerInspection);
    addTest(
        tests,
        "manager_startup.service.active-cancellation",
        activeCallerCancellationPropagatesToThePlatform);
    addTest(
        tests,
        "manager_startup.service.queue-shutdown",
        queueCapacityAndShutdownLifetimeRemainBounded);
}

} // namespace ForgeConductor::Tests
