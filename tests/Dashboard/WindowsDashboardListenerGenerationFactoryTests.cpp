#include "Infrastructure/Windows/Detail/WindowsDashboardListenerGenerationFactory.h"

#include "Infrastructure/Windows/Detail/DashboardAdmissionController.h"
#include "Infrastructure/Windows/Detail/DashboardConnectionResponseCatalog.h"
#include "Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.h"
#include "Infrastructure/Windows/Detail/DashboardFixedIocpKeyAuthority.h"
#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"
#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"
#include "Infrastructure/Windows/Detail/DashboardListenerCompletionKeyLease.h"
#include "Infrastructure/Windows/Detail/DashboardListenerGeneration.h"
#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <WS2tcpip.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Factory = Detail::WindowsDashboardListenerGenerationFactory;
using Generation = Detail::DashboardListenerGeneration;
using GenerationInterface = Detail::IDashboardListenerGeneration;
using TransitionGate = Detail::DashboardListenerGenerationTransitionGate;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Factory>);
static_assert(std::is_base_of_v<
              Detail::IDashboardListenerGenerationFactory,
              Factory>);
static_assert(!std::is_copy_constructible_v<Factory>);
static_assert(!std::is_move_constructible_v<Factory>);
static_assert(noexcept(std::declval<Factory&>().clearBinding()));

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
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

void takeVoid(Domain::Result<void> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const bool retryable,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, "wrong stable error code");
    require(result.error().retryable == retryable,
            "wrong retryable classification");
}

class FrozenClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return Domain::MonotonicTimePoint{1s};
    }
};

class FixedUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        return Domain::Uuid::parse(
            "11111111-1111-4111-8111-111111111111");
    }
};

class ActiveOperationalState final
    : public Detail::IDashboardOperationalStateSource {
public:
    [[nodiscard]] bool operationalServiceActive() const noexcept override
    {
        return true;
    }
};

class QuietKernelSink final : public Detail::IDashboardIocpCompletionSink {
public:
    void consume(Detail::DashboardIoCompletionPacket, DWORD) noexcept override
    {
        ++completionCount;
    }

    void fatal(DWORD) noexcept override { ++fatalCount; }

    std::size_t completionCount{};
    std::size_t fatalCount{};
};

class QuietDeadlineSink final
    : public Windows::IWindowsDashboardDeadlineSink {
public:
    void signal(Windows::WindowsDashboardDeadline) noexcept override
    {
        ++signalCount;
    }

    std::size_t signalCount{};
};

class QuietOverloadResponder final
    : public Detail::IDashboardAdmissionOverloadResponder {
public:
    void respond(Detail::DashboardAdmissionOverloadWork) noexcept override
    {
        ++responseCount;
    }

    [[nodiscard]] std::size_t cancelGeneration(std::uint64_t) noexcept override
    {
        return 0U;
    }

    void drainTerminalGenerationNotifications() noexcept override {}

    std::size_t responseCount{};
};

class RecordingApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange> prepare(
        Dashboard::DashboardHttpRequest,
        bool,
        Domain::OperationContext) noexcept override
    {
        ++prepareCalls;
        return Domain::Result<
            Dashboard::DashboardPreparedExchange>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The factory test application must not be invoked."));
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        ++postDeliveryCalls;
        return Domain::Result<void>::success();
    }

    std::size_t prepareCalls{};
    std::size_t postDeliveryCalls{};
};

[[nodiscard]] std::uint16_t reserveUnusedIpv4Port(
    const std::optional<std::uint16_t> excluded = std::nullopt)
{
    for (std::size_t attempt{}; attempt < 16U; ++attempt) {
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            fail("could not create a port-reservation socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if (::bind(
                socket,
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
            ::closesocket(socket);
            fail("could not reserve a loopback test port");
        }

        int length = static_cast<int>(sizeof(address));
        if (::getsockname(
                socket,
                reinterpret_cast<sockaddr*>(&address),
                &length) == SOCKET_ERROR) {
            ::closesocket(socket);
            fail("could not inspect a reserved loopback test port");
        }
        const auto port = ntohs(address.sin_port);
        ::closesocket(socket);
        if (port != 0U && (!excluded.has_value() || port != *excluded)) {
            return port;
        }
    }
    fail("could not reserve distinct loopback test ports");
}

[[nodiscard]] int bindProbe(const std::uint16_t port)
{
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        fail("could not create a loopback bind probe");
    }
    const BOOL exclusive = TRUE;
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            static_cast<int>(sizeof(exclusive))) == SOCKET_ERROR) {
        ::closesocket(socket);
        fail("could not configure a loopback bind probe");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const int result = ::bind(
        socket,
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address)));
    const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    ::closesocket(socket);
    return error;
}

[[nodiscard]] bool portIsExclusivelyOwned(const std::uint16_t port)
{
    const auto error = bindProbe(port);
    return error == WSAEADDRINUSE || error == WSAEACCES;
}

struct FactoryFixture final {
    FactoryFixture()
        : clock{std::make_shared<FrozenClock>()},
          uuid{std::make_shared<FixedUuidGenerator>()},
          operationalState{std::make_shared<ActiveOperationalState>()},
          kernelSink{std::make_shared<QuietKernelSink>()},
          deadlineSink{std::make_shared<QuietDeadlineSink>()},
          overloadResponder{std::make_shared<QuietOverloadResponder>()},
          authority{take(Detail::DashboardFixedIocpKeyAuthority::create())},
          winsock{take(Detail::DashboardWinsockRuntime::create())},
          connectionRegistry{take(
              Detail::DashboardConnectionRegistry::create(
                  authority.deadline()))},
          runtimeServices{take(
              Detail::DashboardConnectionRuntimeServices::create(
                  clock, uuid, operationalState, authority))},
          completionKeyLeases{take(
              Detail::DashboardListenerCompletionKeyLeasePool::create(
                  authority))},
          admissionController{take(
              Detail::DashboardAdmissionController::create())},
          responseCatalog{take(
              Detail::DashboardConnectionResponseCatalog::create())},
          kernel{take(Detail::DashboardIocpWorkerKernel::create(
              take(Detail::DashboardIoCompletionPort::create()),
              kernelSink))},
          deadlineScheduler{take(
              Windows::WindowsDashboardDeadlineScheduler::create(
                  clock, deadlineSink))},
          handlerExecutor{take(
              Windows::WindowsDashboardHandlerExecutor::create())},
          factory{take(Factory::create(
              *winsock,
              *kernel,
              *deadlineScheduler,
              *handlerExecutor,
              *runtimeServices,
              *admissionController,
              *completionKeyLeases,
              connectionRegistry,
              overloadResponder,
              *responseCatalog))}
    {
    }

    ~FactoryFixture() noexcept
    {
        factory->clearBinding();
        factory.reset();
        handlerExecutor->shutdown();
        deadlineScheduler->shutdown();
        kernel->shutdown();
    }

    [[nodiscard]] Domain::DashboardConfig configuration(
        const std::uint16_t port,
        const std::chrono::seconds refresh = 8s) const
    {
        return Domain::DashboardConfig{"127.0.0.1", port, refresh};
    }

    std::shared_ptr<FrozenClock> clock;
    std::shared_ptr<FixedUuidGenerator> uuid;
    std::shared_ptr<ActiveOperationalState> operationalState;
    std::shared_ptr<QuietKernelSink> kernelSink;
    std::shared_ptr<QuietDeadlineSink> deadlineSink;
    std::shared_ptr<QuietOverloadResponder> overloadResponder;
    Detail::DashboardFixedIocpKeyAuthority authority;
    std::unique_ptr<Detail::DashboardWinsockRuntime> winsock;
    std::shared_ptr<Detail::DashboardConnectionRegistry> connectionRegistry;
    std::unique_ptr<Detail::DashboardConnectionRuntimeServices>
        runtimeServices;
    std::unique_ptr<Detail::DashboardListenerCompletionKeyLeasePool>
        completionKeyLeases;
    std::unique_ptr<Detail::DashboardAdmissionController>
        admissionController;
    std::unique_ptr<Detail::DashboardConnectionResponseCatalog>
        responseCatalog;
    std::unique_ptr<Detail::DashboardIocpWorkerKernel> kernel;
    std::unique_ptr<Windows::WindowsDashboardDeadlineScheduler>
        deadlineScheduler;
    std::unique_ptr<Windows::WindowsDashboardHandlerExecutor>
        handlerExecutor;
    std::shared_ptr<Factory> factory;
};

void rejectsMissingBindingAndInvalidPublicationWithoutMutation()
{
    FactoryFixture fixture;
    const auto emptySnapshot = take(fixture.factory->bindingSnapshot());
    require(!emptySnapshot.has_value(),
            "a new factory unexpectedly exposed a binding");

    auto gate = std::make_shared<TransitionGate>();
    requireError(
        fixture.factory->prepareGeneration(gate),
        Domain::ErrorCodes::Conflict,
        true,
        "preparation without a binding did not fail closed");
    requireError(
        fixture.factory->prepareGeneration(nullptr),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "preparation accepted a null transition gate");

    const auto port = reserveUnusedIpv4Port();
    auto application = std::make_shared<RecordingApplication>();
    auto invalidHost = fixture.configuration(port);
    invalidHost.host = "localhost";
    requireError(
        fixture.factory->publishBinding(invalidHost, application),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "publication accepted a nonliteral loopback host");
    requireError(
        fixture.factory->publishBinding(
            fixture.configuration(port, 0s), application),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "publication accepted a nonpositive refresh interval");
    requireError(
        fixture.factory->publishBinding(
            fixture.configuration(port), nullptr),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "publication accepted a null application policy");
    require(!take(fixture.factory->bindingSnapshot()).has_value(),
            "invalid publication mutated the empty binding");
}

void validatesRetainedDependencies()
{
    FactoryFixture fixture;
    requireError(
        Factory::create(
            *fixture.winsock,
            *fixture.kernel,
            *fixture.deadlineScheduler,
            *fixture.handlerExecutor,
            *fixture.runtimeServices,
            *fixture.admissionController,
            *fixture.completionKeyLeases,
            nullptr,
            fixture.overloadResponder,
            *fixture.responseCatalog),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "factory creation accepted a null connection registry");
    requireError(
        Factory::create(
            *fixture.winsock,
            *fixture.kernel,
            *fixture.deadlineScheduler,
            *fixture.handlerExecutor,
            *fixture.runtimeServices,
            *fixture.admissionController,
            *fixture.completionKeyLeases,
            fixture.connectionRegistry,
            nullptr,
            *fixture.responseCatalog),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "factory creation accepted a null overload responder");
}

void replacementAndClearPreservePreparedGenerationSnapshots()
{
    FactoryFixture fixture;
    const auto portA = reserveUnusedIpv4Port();
    const auto portB = reserveUnusedIpv4Port(portA);
    auto applicationA = std::make_shared<RecordingApplication>();
    auto applicationB = std::make_shared<RecordingApplication>();
    std::weak_ptr<RecordingApplication> weakApplicationA = applicationA;
    std::weak_ptr<RecordingApplication> weakApplicationB = applicationB;

    const auto configurationA = fixture.configuration(portA, 7s);
    takeVoid(fixture.factory->publishBinding(
        configurationA, applicationA));
    auto snapshotA = take(fixture.factory->bindingSnapshot());
    require(snapshotA.has_value() &&
                snapshotA->configuration() == configurationA &&
                snapshotA->applicationIdentity() == applicationA.get() &&
                snapshotA->publicationSequence() == 1U,
            "first immutable binding snapshot was incorrect");

    auto generationA = fixture.factory->prepareGeneration(
        std::make_shared<TransitionGate>());
    require(static_cast<bool>(generationA),
            "first production generation preparation failed");
    std::shared_ptr<GenerationInterface> preparedA =
        std::move(generationA).value();
    auto concreteA = std::dynamic_pointer_cast<Generation>(preparedA);
    require(concreteA != nullptr &&
                concreteA->snapshot().lifecycle() ==
                    Detail::DashboardListenerGenerationLifecycle::Prepared &&
                concreteA->snapshot().completionCount() == 0U,
            "preparation started native admission before publication");
    require(portIsExclusivelyOwned(portA),
            "prepared generation did not retain its exact listener binding");

    auto rejectedReplacement = fixture.configuration(portB, 9s);
    rejectedReplacement.host = "localhost";
    requireError(
        fixture.factory->publishBinding(
            rejectedReplacement, applicationB),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "invalid replacement binding was published");
    const auto unchanged = take(fixture.factory->bindingSnapshot());
    require(unchanged.has_value() &&
                unchanged->configuration() == configurationA &&
                unchanged->applicationIdentity() == applicationA.get() &&
                unchanged->publicationSequence() == 1U,
            "invalid replacement mutated the prior publication");

    const auto configurationB = fixture.configuration(portB, 11s);
    takeVoid(fixture.factory->publishBinding(
        configurationB, applicationB));
    applicationA.reset();
    snapshotA.reset();
    require(!weakApplicationA.expired(),
            "replacement released the prepared generation application policy");

    const auto snapshotB = take(fixture.factory->bindingSnapshot());
    require(snapshotB.has_value() &&
                snapshotB->configuration() == configurationB &&
                snapshotB->applicationIdentity() == applicationB.get() &&
                snapshotB->publicationSequence() == 2U,
            "replacement binding snapshot was not published atomically");
    require(portIsExclusivelyOwned(portA),
            "replacement mutated the already prepared listener binding");

    auto preparedB = take(fixture.factory->prepareGeneration(
        std::make_shared<TransitionGate>()));
    auto concreteB = std::dynamic_pointer_cast<Generation>(preparedB);
    require(concreteB != nullptr &&
                concreteB->snapshot().lifecycle() ==
                    Detail::DashboardListenerGenerationLifecycle::Prepared &&
                concreteB->snapshot().completionCount() == 0U,
            "replacement preparation started native admission");
    require(preparedA->registrationId() != preparedB->registrationId() &&
                preparedA->completionKey() != preparedB->completionKey(),
            "prepared generations did not retain exact distinct identities");
    require(portIsExclusivelyOwned(portB),
            "replacement generation did not bind its published endpoint");
    requireError(
        fixture.completionKeyLeases->tryAcquire(),
        Domain::ErrorCodes::Conflict,
        true,
        "two prepared generations did not retain both fixed key leases");

    fixture.factory->clearBinding();
    applicationB.reset();
    require(!take(fixture.factory->bindingSnapshot()).has_value(),
            "clear did not close future binding observation");
    requireError(
        fixture.factory->prepareGeneration(
            std::make_shared<TransitionGate>()),
        Domain::ErrorCodes::Conflict,
        true,
        "clear did not close future generation preparation");
    require(portIsExclusivelyOwned(portA) &&
                portIsExclusivelyOwned(portB) &&
                !weakApplicationA.expired() &&
                !weakApplicationB.expired(),
            "clear mutated an already prepared generation snapshot");

    concreteA.reset();
    preparedA.reset();
    require(weakApplicationA.expired() && bindProbe(portA) == 0,
            "first prepared generation did not clean up exact ownership");
    {
        auto releasedLease = take(
            fixture.completionKeyLeases->tryAcquire());
        require(releasedLease.completionKey() ==
                    fixture.authority.listenerSlotA(),
                "first generation released the wrong fixed key lease");
    }

    concreteB.reset();
    preparedB.reset();
    require(weakApplicationB.expired() && bindProbe(portB) == 0,
            "replacement generation did not clean up exact ownership");
    require(fixture.kernelSink->completionCount == 0U &&
                fixture.deadlineSink->signalCount == 0U &&
                fixture.overloadResponder->responseCount == 0U,
            "preparation admitted native, deadline, or overload work");
}

} // namespace

int main()
{
    try {
        rejectsMissingBindingAndInvalidPublicationWithoutMutation();
        validatesRetainedDependencies();
        replacementAndClearPreservePreparedGenerationSnapshots();
        std::cout
            << "Windows dashboard listener-generation factory tests passed ("
            << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Windows dashboard listener-generation factory tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
