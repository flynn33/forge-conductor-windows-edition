#include "Infrastructure/TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeServer.h"
#include "../../src/Infrastructure/Windows/Detail/CurrentUserPipeSecurity.h"
#include "../../src/Infrastructure/Windows/Detail/ManagerPipeIo.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsCurrentUserIdentity;
using Infrastructure::Windows::SystemClock;
using Infrastructure::Windows::WindowsManagerNamedPipeClient;
using Infrastructure::Windows::WindowsManagerNamedPipeServer;
using Infrastructure::Windows::WindowsManagerNamedPipeServerOptions;
using Infrastructure::Windows::Detail::CurrentUserPipeSecurity;
using Infrastructure::Windows::Detail::DefaultManagerPipeMaximumPayloadBytes;
using Infrastructure::Windows::Detail::UniqueHandle;
using Infrastructure::Windows::Detail::connectManagerPipe;
using Infrastructure::Windows::Detail::openManagerPipe;
using Infrastructure::Windows::Detail::readManagerPipeFrame;
using Infrastructure::Windows::Detail::readManagerPipeResponseReceipt;
using Infrastructure::Windows::Detail::verifyNamedPipeClientSid;
using Infrastructure::Windows::Detail::writeManagerPipeFrame;
using Infrastructure::Windows::Detail::writeManagerPipeResponseReceipt;

static_assert(std::is_final_v<CurrentUserPipeSecurity>);
static_assert(!std::is_copy_constructible_v<CurrentUserPipeSecurity>);
static_assert(!std::is_move_constructible_v<CurrentUserPipeSecurity>);

std::atomic<unsigned long> nextPipeId{1U};

[[nodiscard]] std::wstring pipeName(const std::wstring_view purpose)
{
    return L"\\\\.\\pipe\\ForgeConductor.Manager.PipeIoTests." +
        std::to_wstring(::GetCurrentProcessId()) + L'.' +
        std::to_wstring(nextPipeId.fetch_add(1U, std::memory_order_relaxed)) +
        L'.' + std::wstring{purpose};
}

[[nodiscard]] Domain::OperationContext operationContext(
    const std::stop_token cancellation = {},
    const std::chrono::milliseconds lifetime = std::chrono::seconds{5})
{
    const TestContext base;
    return Domain::OperationContext{
        base.operationId,
        std::chrono::steady_clock::now() + lifetime,
        cancellation,
        base.correlationId};
}

[[nodiscard]] UniqueHandle createShutdownEvent()
{
    UniqueHandle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(static_cast<bool>(event),
            "the manager pipe test could not create its shutdown event");
    return event;
}

class NonCooperativeManagerController final
    : public Contracts::IManagerController {
public:
    NonCooperativeManagerController()
        : entered_{createShutdownEvent()}, release_{createShutdownEvent()}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerControllerSnapshot>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext&) noexcept override
    {
        static_cast<void>(::SetEvent(entered_.get()));
        // Deliberately ignore cancellation until the test releases this fake.
        static_cast<void>(::WaitForSingleObject(release_.get(), INFINITE));
        return unavailable<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerSettings>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerSettingsUpdateOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerControllerSnapshot>();
    }

    void shutdown() noexcept override { release(); }

    [[nodiscard]] bool waitUntilEntered(
        const std::chrono::milliseconds timeout) const noexcept
    {
        return ::WaitForSingleObject(
            entered_.get(), static_cast<DWORD>(timeout.count())) ==
            WAIT_OBJECT_0;
    }

    void release() noexcept
    {
        static_cast<void>(::SetEvent(release_.get()));
    }

private:
    template <typename Value>
    [[nodiscard]] static Domain::Result<Value> unavailable() noexcept
    {
        return Domain::Result<Value>::failure(Domain::makeError(
            Domain::ErrorCodes::TransportClosed,
            "The non-cooperative manager test controller has no result."));
    }

    UniqueHandle entered_;
    UniqueHandle release_;
};

struct ConnectedPipePair final {
    UniqueHandle server;
    UniqueHandle client;
    UniqueHandle shutdownEvent;
};

[[nodiscard]] ConnectedPipePair createConnectedPipePair(
    const WindowsCurrentUserIdentity& identity)
{
    auto security = take(CurrentUserPipeSecurity::create(identity));
    const std::wstring name = pipeName(L"connected");
    UniqueHandle server{::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1U, 64U * 1024U, 64U * 1024U, 0U,
        security->attributes())};
    require(static_cast<bool>(server),
            "the manager pipe test could not create its server instance");
    auto shutdownEvent = createShutdownEvent();

    const Domain::OperationContext connectContext = operationContext();
    std::optional<Domain::Result<void>> connected;
    std::jthread connector{[&] {
        connected.emplace(connectManagerPipe(
            server.get(), connectContext, shutdownEvent.get()));
    }};
    auto client = take(openManagerPipe(
        name, GENERIC_READ | GENERIC_WRITE, operationContext(),
        shutdownEvent.get()));
    connector.join();
    require(connected.has_value() && static_cast<bool>(connected.value()),
            "the manager pipe test server did not complete its connection");
    return ConnectedPipePair{
        std::move(server), std::move(client), std::move(shutdownEvent)};
}

[[nodiscard]] std::vector<std::byte> frameWithPayloadBytes(
    const std::size_t payloadBytes)
{
    require(payloadBytes <= DefaultManagerPipeMaximumPayloadBytes,
            "the manager pipe test requested an out-of-bound frame fixture");
    const auto encodedLength = static_cast<std::uint32_t>(payloadBytes);
    std::vector<std::byte> frame(payloadBytes + 4U);
    frame[0] = static_cast<std::byte>(encodedLength & 0xffU);
    frame[1] = static_cast<std::byte>((encodedLength >> 8U) & 0xffU);
    frame[2] = static_cast<std::byte>((encodedLength >> 16U) & 0xffU);
    frame[3] = static_cast<std::byte>((encodedLength >> 24U) & 0xffU);
    for (std::size_t index = 0U; index < payloadBytes; ++index) {
        frame[index + 4U] = static_cast<std::byte>(index & 0xffU);
    }
    return frame;
}

void writeRawPrefix(
    const HANDLE pipe,
    const std::array<std::byte, 4U>& prefix)
{
    UniqueHandle completedEvent{
        ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(static_cast<bool>(completedEvent),
            "the manager pipe test could not create a raw-write event");
    OVERLAPPED operation{};
    operation.hEvent = completedEvent.get();
    DWORD transferred = 0U;
    if (::WriteFile(
            pipe, prefix.data(), static_cast<DWORD>(prefix.size()),
            &transferred, &operation) == FALSE) {
        require(::GetLastError() == ERROR_IO_PENDING,
                "the manager pipe raw prefix write did not become pending");
        require(::WaitForSingleObject(completedEvent.get(), 5'000U) == WAIT_OBJECT_0,
                "the manager pipe raw prefix write did not complete");
        require(::GetOverlappedResult(
                    pipe, &operation, &transferred, FALSE) != FALSE,
                "the manager pipe raw prefix write could not be reaped");
    }
    require(transferred == prefix.size(),
            "the manager pipe raw prefix write was partial");
}

void currentUserDaclIsExactAndReusable()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto security = take(CurrentUserPipeSecurity::create(identity));
    SECURITY_ATTRIBUTES* const firstAttributes = security->attributes();
    SECURITY_ATTRIBUTES* const secondAttributes = security->attributes();
    require(firstAttributes == secondAttributes,
            "the manager pipe security attributes were not stable across calls");
    require(firstAttributes->nLength == sizeof(SECURITY_ATTRIBUTES) &&
                firstAttributes->bInheritHandle == FALSE &&
                firstAttributes->lpSecurityDescriptor != nullptr,
            "the manager pipe security attributes were not non-inheritable and complete");
    require(::IsValidSecurityDescriptor(
                firstAttributes->lpSecurityDescriptor) != FALSE,
            "the manager pipe security descriptor was invalid");

    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = TRUE;
    PACL dacl = nullptr;
    require(::GetSecurityDescriptorDacl(
                firstAttributes->lpSecurityDescriptor, &daclPresent, &dacl,
                &daclDefaulted) != FALSE,
            "the manager pipe test could not inspect the DACL");
    require(daclPresent != FALSE && daclDefaulted == FALSE && dacl != nullptr &&
                ::IsValidAcl(dacl) != FALSE && dacl->AceCount == 1U,
            "the manager pipe DACL was not one explicit current-user entry");

    void* rawAce = nullptr;
    require(::GetAce(dacl, 0U, &rawAce) != FALSE && rawAce != nullptr,
            "the manager pipe DACL did not expose its sole entry");
    const auto* const ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
    require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                ace->Mask == CurrentUserPipeSecurity::GrantedAccess,
            "the manager pipe DACL entry did not grant the expected pipe rights");
    const PSID aceSid = reinterpret_cast<PSID>(
        const_cast<DWORD*>(&ace->SidStart));
    require(::IsValidSid(aceSid) != FALSE &&
                ::GetLengthSid(aceSid) == identity.sidBytes().size(),
            "the manager pipe DACL entry did not contain a bounded valid SID");
    const std::span<const std::byte> aceSidBytes{
        reinterpret_cast<const std::byte*>(aceSid), identity.sidBytes().size()};
    require(std::ranges::equal(aceSidBytes, identity.sidBytes()),
            "the manager pipe DACL entry was not the exact current-user SID");

    for (const std::wstring_view purpose : {L"dacl-a", L"dacl-b"}) {
        const std::wstring name = pipeName(purpose);
        UniqueHandle pipe{::CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1U, 4'096U, 4'096U, 0U, security->attributes())};
        require(static_cast<bool>(pipe),
                "one reusable manager pipe security descriptor creation failed");
    }
}

void sameUserClientSidIsAccepted()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto pipes = createConnectedPipePair(identity);
    const auto frame = frameWithPayloadBytes(1U);
    take(writeManagerPipeFrame(
        pipes.client.get(), frame, operationContext(),
        pipes.shutdownEvent.get()));
    const auto received = take(readManagerPipeFrame(
        pipes.server.get(), operationContext(), pipes.shutdownEvent.get()));
    require(received == frame,
            "the manager pipe identity fixture did not exchange its exact frame");
    take(verifyNamedPipeClientSid(pipes.server.get(), identity.sidBytes()));
}

void wrongClientSidIsRejected()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto pipes = createConnectedPipePair(identity);
    const auto frame = frameWithPayloadBytes(1U);
    take(writeManagerPipeFrame(
        pipes.client.get(), frame, operationContext(),
        pipes.shutdownEvent.get()));
    static_cast<void>(take(readManagerPipeFrame(
        pipes.server.get(), operationContext(), pipes.shutdownEvent.get())));

    std::array<std::byte, SECURITY_MAX_SID_SIZE> wrongSidStorage{};
    DWORD wrongSidBytes = static_cast<DWORD>(wrongSidStorage.size());
    require(::CreateWellKnownSid(
                WinBuiltinGuestsSid, nullptr, wrongSidStorage.data(),
                &wrongSidBytes) != FALSE,
            "the manager pipe test could not create a distinct valid SID");
    requireError(
        verifyNamedPipeClientSid(
            pipes.server.get(),
            std::span<const std::byte>{wrongSidStorage}.first(wrongSidBytes)),
        Domain::ErrorCodes::Unauthorized,
        "the manager pipe accepted a client against the wrong expected SID");
}

void exactMaximumFrameRoundTrips()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto pipes = createConnectedPipePair(identity);
    const auto frame = frameWithPayloadBytes(
        DefaultManagerPipeMaximumPayloadBytes);
    const Domain::OperationContext readerContext = operationContext();
    std::optional<Domain::Result<std::vector<std::byte>>> received;
    std::jthread reader{[&] {
        received.emplace(readManagerPipeFrame(
            pipes.server.get(), readerContext, pipes.shutdownEvent.get()));
    }};
    const auto sent = writeManagerPipeFrame(
        pipes.client.get(), frame, operationContext(),
        pipes.shutdownEvent.get());
    if (!sent) {
        static_cast<void>(::SetEvent(pipes.shutdownEvent.get()));
    }
    reader.join();
    require(static_cast<bool>(sent),
            "the manager pipe rejected the exact maximum frame on write");
    require(received.has_value() && static_cast<bool>(received.value()),
            "the manager pipe rejected the exact maximum frame on read");
    require(received.value().value() == frame,
            "the manager pipe changed the exact maximum frame in transit");
}

void responseReceiptIsExactAndBounded()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    {
        auto pipes = createConnectedPipePair(identity);
        take(writeManagerPipeResponseReceipt(
            pipes.client.get(), operationContext(), pipes.shutdownEvent.get()));
        take(readManagerPipeResponseReceipt(
            pipes.server.get(), operationContext(), pipes.shutdownEvent.get()));
    }

    {
        auto pipes = createConnectedPipePair(identity);
        const auto wrongReceipt = frameWithPayloadBytes(
            Infrastructure::Windows::Detail::
                ManagerPipeResponseReceiptPayloadBytes);
        take(writeManagerPipeFrame(
            pipes.client.get(), wrongReceipt, operationContext(),
            pipes.shutdownEvent.get(),
            Infrastructure::Windows::Detail::
                ManagerPipeResponseReceiptPayloadBytes));
        requireError(
            readManagerPipeResponseReceipt(
                pipes.server.get(), operationContext(),
                pipes.shutdownEvent.get()),
            Domain::ErrorCodes::MalformedMessage,
            "the manager pipe accepted an invalid response receipt");
    }
}

void oversizeAndMalformedFramesAreRejectedBeforePayloadRead()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto pipes = createConnectedPipePair(identity);
    const std::uint32_t oversize = static_cast<std::uint32_t>(
        DefaultManagerPipeMaximumPayloadBytes + 1U);
    const std::array<std::byte, 4U> oversizePrefix{
        static_cast<std::byte>(oversize & 0xffU),
        static_cast<std::byte>((oversize >> 8U) & 0xffU),
        static_cast<std::byte>((oversize >> 16U) & 0xffU),
        static_cast<std::byte>((oversize >> 24U) & 0xffU)};

    requireError(
        writeManagerPipeFrame(
            pipes.client.get(), oversizePrefix, operationContext(),
            pipes.shutdownEvent.get()),
        Domain::ErrorCodes::PayloadTooLarge,
        "the manager pipe writer accepted an oversized declared payload");
    const std::array<std::byte, 3U> shortFrame{};
    requireError(
        writeManagerPipeFrame(
            pipes.client.get(), shortFrame, operationContext(),
            pipes.shutdownEvent.get()),
        Domain::ErrorCodes::MalformedMessage,
        "the manager pipe writer accepted a missing length prefix");
    const std::array<std::byte, 5U> mismatchedFrame{
        std::byte{2U}, std::byte{0U}, std::byte{0U}, std::byte{0U},
        std::byte{0x41U}};
    requireError(
        writeManagerPipeFrame(
            pipes.client.get(), mismatchedFrame, operationContext(),
            pipes.shutdownEvent.get()),
        Domain::ErrorCodes::MalformedMessage,
        "the manager pipe writer accepted a mismatched frame length");

    writeRawPrefix(pipes.client.get(), oversizePrefix);
    requireError(
        readManagerPipeFrame(
            pipes.server.get(), operationContext(), pipes.shutdownEvent.get()),
        Domain::ErrorCodes::PayloadTooLarge,
        "the manager pipe reader accepted an oversized prefix before payload allocation");
}

void cancellationDeadlineAndShutdownAlwaysLeaveIoReaped()
{
    const auto identity = take(WindowsCurrentUserIdentity::load());
    auto pipes = createConnectedPipePair(identity);

    for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
        std::stop_source cancellation;
        auto entered = createShutdownEvent();
        const Domain::OperationContext readerContext = operationContext(
            cancellation.get_token());
        std::optional<Domain::Result<std::vector<std::byte>>> result;
        std::jthread reader{[&] {
            require(::SetEvent(entered.get()) != FALSE,
                    "the cancellation test could not signal reader entry");
            result.emplace(readManagerPipeFrame(
                pipes.server.get(), readerContext, pipes.shutdownEvent.get()));
        }};
        require(::WaitForSingleObject(entered.get(), 5'000U) == WAIT_OBJECT_0,
                "the cancellation test reader did not enter");
        ::Sleep(20U);
        require(cancellation.request_stop(),
                "the cancellation test could not request its first stop");
        reader.join();
        require(result.has_value(),
                "the cancelled manager pipe read did not return");
        requireError(
            result.value(), Domain::ErrorCodes::Cancelled,
            "the manager pipe read did not report cancellation");
    }

    requireError(
        readManagerPipeFrame(
            pipes.server.get(), operationContext({}, std::chrono::milliseconds{25}),
            pipes.shutdownEvent.get()),
        Domain::ErrorCodes::DeadlineExceeded,
        "the manager pipe read did not report its deadline");

    const auto frame = frameWithPayloadBytes(257U);
    take(writeManagerPipeFrame(
        pipes.client.get(), frame, operationContext(),
        pipes.shutdownEvent.get()));
    const auto received = take(readManagerPipeFrame(
        pipes.server.get(), operationContext(), pipes.shutdownEvent.get()));
    require(received == frame,
            "repeated cancellation left a live or corrupt manager pipe operation");

    require(::SetEvent(pipes.shutdownEvent.get()) != FALSE,
            "the manager pipe test could not request shared shutdown");
    requireError(
        readManagerPipeFrame(
            pipes.server.get(), operationContext(), pipes.shutdownEvent.get()),
        Domain::ErrorCodes::Cancelled,
        "the manager pipe read ignored the shared shutdown event");
}

void openHonorsCancellationAndDeadline()
{
    auto shutdownEvent = createShutdownEvent();
    const std::wstring missingPipe = pipeName(L"missing");

    std::stop_source cancelled;
    require(cancelled.request_stop(),
            "the manager pipe open test could not request cancellation");
    requireError(
        openManagerPipe(
            missingPipe, GENERIC_READ | GENERIC_WRITE,
            operationContext(cancelled.get_token()), shutdownEvent.get()),
        Domain::ErrorCodes::Cancelled,
        "the manager pipe open ignored pre-requested cancellation");
    requireError(
        openManagerPipe(
            missingPipe, GENERIC_READ | GENERIC_WRITE,
            operationContext({}, std::chrono::milliseconds{25}),
            shutdownEvent.get()),
        Domain::ErrorCodes::DeadlineExceeded,
        "the manager pipe open did not stop retrying at its deadline");
}

void serverRejectsTransportLimitsOutsideHardBounds()
{
    const auto clock = std::make_shared<SystemClock>();
    const auto controller =
        std::make_shared<NonCooperativeManagerController>();
    const auto dispatcher =
        std::make_shared<Manager::ManagerRequestDispatcher>(controller, clock);
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto authenticationToken = take(
        Domain::Sha256Digest::parse(std::string(64U, 'c')));

    const auto requireInvalid = [&](Manager::ManagerTransportLimits limits) {
        WindowsManagerNamedPipeServerOptions options;
        options.pipeName = pipeName(L"invalid-limits");
        options.limits = limits;
        requireError(
            WindowsManagerNamedPipeServer::create(
                clock, dispatcher, identity, authenticationToken,
                std::move(options)),
            Domain::ErrorCodes::InvalidRequest,
            "the manager server accepted transport limits outside policy");
    };

    Manager::ManagerTransportLimits limits;
    limits.maximumRequestLifetime = std::chrono::minutes{5} +
        std::chrono::milliseconds{1};
    requireInvalid(limits);
    limits = {};
    limits.connectTimeout = std::chrono::seconds{2} +
        std::chrono::milliseconds{1};
    requireInvalid(limits);
    limits = {};
    limits.shutdownDrainTimeout = std::chrono::milliseconds::zero();
    requireInvalid(limits);
    limits = {};
    limits.shutdownDrainTimeout = std::chrono::seconds{5} +
        std::chrono::milliseconds{1};
    requireInvalid(limits);
    limits = {};
    limits.maximumConcurrentClientRequests = 17U;
    requireInvalid(limits);

    dispatcher->shutdown();
}

void serverShutdownDetachesOnlyAfterBoundedNonCooperativeDrain()
{
    const auto clock = std::make_shared<SystemClock>();
    const auto controller =
        std::make_shared<NonCooperativeManagerController>();
    Manager::ManagerTransportLimits limits;
    limits.shutdownDrainTimeout = std::chrono::milliseconds{50};
    limits.maximumActiveRegularOperations = 1U;
    const auto dispatcher =
        std::make_shared<Manager::ManagerRequestDispatcher>(
            controller, clock, limits);
    const auto identity = take(WindowsCurrentUserIdentity::load());
    const auto authenticationToken = take(
        Domain::Sha256Digest::parse(std::string(64U, 'd')));
    const std::wstring name = pipeName(L"non-cooperative-shutdown");
    WindowsManagerNamedPipeServerOptions options;
    options.pipeName = name;
    options.limits = limits;
    options.workerCount = 2U;
    options.ingressTimeout = std::chrono::milliseconds{500};
    auto server = take(WindowsManagerNamedPipeServer::create(
        clock, dispatcher, identity, authenticationToken, options));
    auto client = take(WindowsManagerNamedPipeClient::create(
        clock, name, authenticationToken, limits));

    auto runComplete = createShutdownEvent();
    std::optional<Domain::Result<void>> runResult;
    const Domain::OperationContext runContext = operationContext(
        {}, std::chrono::seconds{5});
    std::jthread runThread{[&] {
        runResult.emplace(server->run(runContext));
        static_cast<void>(::SetEvent(runComplete.get()));
    }};
    std::optional<Domain::Result<Domain::ManagerStatus>> clientResult;
    const Domain::OperationContext clientContext = operationContext(
        {}, std::chrono::seconds{5});
    std::jthread clientThread{[&] {
        clientResult.emplace(client->status(clientContext));
    }};

    const bool callbackEntered = controller->waitUntilEntered(
        std::chrono::seconds{2});
    const auto shutdownStart = std::chrono::steady_clock::now();
    server->shutdown();
    const bool runReturned = ::WaitForSingleObject(
        runComplete.get(), 750U) == WAIT_OBJECT_0;
    const auto shutdownElapsed = std::chrono::steady_clock::now() -
        shutdownStart;

    // Always release the deliberately stuck callback before assertions so a
    // failed regression remains a bounded test failure.
    controller->release();
    clientThread.join();
    runThread.join();
    client->shutdown();
    dispatcher->shutdown();

    require(callbackEntered,
            "the non-cooperative controller callback did not start");
    require(runReturned && shutdownElapsed < std::chrono::milliseconds{750},
            "manager server shutdown waited past its configured callback drain");
    require(runResult.has_value() && static_cast<bool>(runResult.value()),
            "manager server shutdown did not return its orderly result");
    require(clientResult.has_value(),
            "the non-cooperative manager client did not return after release");
}

} // namespace

void registerManagerPipeInfrastructureTests(TestRegistry& tests)
{
    addTest(tests, "manager_pipe.current_user_dacl_is_exact_and_reusable",
            currentUserDaclIsExactAndReusable);
    addTest(tests, "manager_pipe.same_user_client_sid_is_accepted",
            sameUserClientSidIsAccepted);
    addTest(tests, "manager_pipe.wrong_client_sid_is_rejected",
            wrongClientSidIsRejected);
    addTest(tests, "manager_pipe.exact_maximum_frame_round_trips",
            exactMaximumFrameRoundTrips);
    addTest(tests, "manager_pipe.response_receipt_is_exact_and_bounded",
            responseReceiptIsExactAndBounded);
    addTest(tests, "manager_pipe.oversize_and_malformed_frames_are_rejected",
            oversizeAndMalformedFramesAreRejectedBeforePayloadRead);
    addTest(tests, "manager_pipe.cancellation_deadline_and_shutdown_reap_io",
            cancellationDeadlineAndShutdownAlwaysLeaveIoReaped);
    addTest(tests, "manager_pipe.open_honors_cancellation_and_deadline",
            openHonorsCancellationAndDeadline);
    addTest(tests, "manager_pipe.server_rejects_unbounded_transport_limits",
            serverRejectsTransportLimitsOutsideHardBounds);
    addTest(tests, "manager_pipe.server_shutdown_is_noncooperative_bounded",
            serverShutdownDetachesOnlyAfterBoundedNonCooperativeDrain);
}

} // namespace ForgeConductor::Tests

#if defined(FORGE_MANAGER_PIPE_STANDALONE_TEST_MAIN)

#include <exception>
#include <iostream>

int main()
{
    ForgeConductor::Tests::TestRegistry tests;
    ForgeConductor::Tests::registerManagerPipeInfrastructureTests(tests);
    std::size_t passed = 0U;
    for (const auto& [name, run] : tests) {
        try {
            std::cout << "[RUN] " << name << '\n' << std::flush;
            run();
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
            return 1;
        }
    }
    std::cout << passed << '/' << tests.size()
              << " manager pipe infrastructure tests passed.\n";
    return 0;
}

#endif
