#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using Api = Detail::IDashboardIoCompletionPortApi;
using ControlPostResult =
    Detail::DashboardIoCompletionControlPostResult;
using DequeueDisposition = Detail::DashboardIoCompletionDequeueDisposition;
using DequeueResult = Detail::DashboardIoCompletionDequeueResult;
using Key = Detail::DashboardIoCompletionKey;
using Packet = Detail::DashboardIoCompletionPacket;
using Port = Detail::DashboardIoCompletionPort;
using SystemApi = Detail::DashboardIoCompletionPortSystemApi;

template <typename Value>
concept HasRawGet = requires(const Value& value) {
    value.get();
};

template <typename Value>
concept HasRawRelease = requires(Value& value) {
    value.release();
};

static_assert(std::is_abstract_v<Api>);
static_assert(std::is_final_v<SystemApi>);
static_assert(std::is_final_v<Port>);
static_assert(!std::is_copy_constructible_v<Port>);
static_assert(!std::is_copy_assignable_v<Port>);
static_assert(!std::is_move_constructible_v<Port>);
static_assert(!std::is_move_assignable_v<Port>);
static_assert(std::is_nothrow_destructible_v<Port>);
static_assert(!HasRawGet<Port>);
static_assert(!HasRawRelease<Port>);
static_assert(Port::RequiredConcurrencyThreadCount == 4U);
static_assert(noexcept(Port::create()));
static_assert(noexcept(Port::create({})));
static_assert(noexcept(std::declval<const Port&>().associateHandle(
    nullptr, Key{0U})));
static_assert(noexcept(std::declval<const Port&>().associateSocket(
    INVALID_SOCKET, Key{0U})));
static_assert(noexcept(std::declval<const Port&>().post(
    0U, Key{0U}, nullptr)));
static_assert(noexcept(std::declval<const Port&>().postControl(Key{0U})));
static_assert(noexcept(std::declval<const Port&>().dequeue(0U)));
static_assert(std::is_trivially_copyable_v<Key>);
static_assert(std::is_final_v<ControlPostResult>);
static_assert(std::is_trivially_copyable_v<ControlPostResult>);
static_assert(std::is_final_v<Packet>);
static_assert(std::is_final_v<DequeueResult>);

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

[[nodiscard]] HANDLE handleValue(const std::uintptr_t value) noexcept
{
    return reinterpret_cast<HANDLE>(value);
}

enum class Event : std::uint8_t {
    Create,
    Associate,
    Post,
    Dequeue,
    LastError,
    Close,
};

class FakeApi final : public Api {
public:
    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR completionKey,
        const DWORD concurrentThreadCount) noexcept override
    {
        if (existingCompletionPort == nullptr) {
            record(Event::Create);
            ++createCalls;
            creationFileHandle = fileHandle;
            creationExistingPort = existingCompletionPort;
            creationCompletionKey = completionKey;
            creationConcurrency = concurrentThreadCount;
            return portToCreate;
        }

        record(Event::Associate);
        ++associateCalls;
        associatedFileHandle = fileHandle;
        associatedExistingPort = existingCompletionPort;
        associatedCompletionKey = completionKey;
        associatedConcurrency = concurrentThreadCount;
        return associationResult;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        const HANDLE completionPort,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        record(Event::Post);
        ++postCalls;
        postedPort = completionPort;
        postedBytes = transferredBytes;
        postedKey = completionKey;
        postedOperation = operation;
        return postStatus;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        const HANDLE completionPort,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        record(Event::Dequeue);
        ++dequeueCalls;
        dequeuedPort = completionPort;
        dequeueTimeout = timeoutMilliseconds;
        initialTransferredBytes = transferredBytes;
        initialCompletionKey = completionKey;
        initialOperation = operation;
        transferredBytes = dequeueBytes;
        completionKey = dequeueKey;
        operation = dequeueOperation;
        return dequeueStatus;
    }

    [[nodiscard]] BOOL closeHandle(const HANDLE handle) noexcept override
    {
        record(Event::Close);
        ++closeCalls;
        closedHandle = handle;
        return closeStatus;
    }

    [[nodiscard]] DWORD lastError() noexcept override
    {
        record(Event::LastError);
        ++lastErrorCalls;
        return errorToReturn;
    }

    [[nodiscard]] Event event(const std::size_t index) const
    {
        require(index < eventCount_, "event index exceeded recorded calls");
        return events_[index];
    }

    [[nodiscard]] std::size_t eventCount() const noexcept
    {
        return eventCount_;
    }

    HANDLE portToCreate{handleValue(0x1001U)};
    HANDLE associationResult{portToCreate};
    BOOL postStatus{TRUE};
    BOOL dequeueStatus{TRUE};
    BOOL closeStatus{TRUE};
    DWORD errorToReturn{ERROR_GEN_FAILURE};
    DWORD dequeueBytes{};
    ULONG_PTR dequeueKey{};
    OVERLAPPED* dequeueOperation{};

    std::size_t createCalls{};
    std::size_t associateCalls{};
    std::size_t postCalls{};
    std::size_t dequeueCalls{};
    std::size_t lastErrorCalls{};
    std::size_t closeCalls{};

    HANDLE creationFileHandle{};
    HANDLE creationExistingPort{};
    ULONG_PTR creationCompletionKey{};
    DWORD creationConcurrency{};
    HANDLE associatedFileHandle{};
    HANDLE associatedExistingPort{};
    ULONG_PTR associatedCompletionKey{};
    DWORD associatedConcurrency{};
    HANDLE postedPort{};
    DWORD postedBytes{};
    ULONG_PTR postedKey{};
    OVERLAPPED* postedOperation{};
    HANDLE dequeuedPort{};
    DWORD dequeueTimeout{};
    DWORD initialTransferredBytes{1U};
    ULONG_PTR initialCompletionKey{1U};
    OVERLAPPED* initialOperation{
        reinterpret_cast<OVERLAPPED*>(static_cast<std::uintptr_t>(1U))};
    HANDLE closedHandle{};

private:
    void record(const Event event) noexcept
    {
        if (eventCount_ < events_.size()) {
            events_[eventCount_++] = event;
        }
    }

    std::array<Event, 64U> events_{};
    std::size_t eventCount_{};
};

[[nodiscard]] std::unique_ptr<Port> createPort(
    const std::shared_ptr<FakeApi>& api)
{
    return take(Port::create(api));
}

void creationUsesOnePortAndExactlyFourConcurrencyThreads()
{
    auto api = std::make_shared<FakeApi>();
    api->closeStatus = FALSE;
    {
        auto owner = createPort(api);
        require(owner != nullptr, "completion-port factory returned null");
        require(api->createCalls == 1U,
                "factory did not issue exactly one creation call");
        require(api->creationFileHandle == INVALID_HANDLE_VALUE,
                "factory did not use INVALID_HANDLE_VALUE");
        require(api->creationExistingPort == nullptr,
                "factory supplied an existing port during creation");
        require(api->creationCompletionKey == 0U,
                "factory supplied a creation completion key");
        require(api->creationConcurrency == 4U,
                "factory did not request exactly four concurrency threads");
        require(api->closeCalls == 0U,
                "factory closed the completion port before destruction");
        require(api->eventCount() == 1U && api->event(0U) == Event::Create,
                "successful creation made an unexpected native call");
    }
    require(api->closeCalls == 1U,
            "owner destruction did not close the port exactly once");
    require(api->closedHandle == api->portToCreate,
            "owner destruction closed the wrong handle");
    require(api->lastErrorCalls == 0U,
            "successful creation or close read a stale native error");
    require(api->eventCount() == 2U && api->event(1U) == Event::Close,
            "port close did not follow successful creation");
}

void nullApiIsRejectedWithoutNativeCalls()
{
    const auto result = Port::create(std::shared_ptr<Api>{});
    require(!result, "factory accepted a null native API");
    require(result.error().code == Domain::ErrorCodes::InvalidRequest,
            "null API returned the wrong stable error");
    require(!result.error().retryable,
            "null API rejection was unexpectedly retryable");
}

void creationFailuresAreTypedAndNeverClosed()
{
    struct Case final {
        DWORD nativeError;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{ERROR_NOT_ENOUGH_MEMORY, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_NO_SYSTEM_RESOURCES, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_ACCESS_DENIED,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{ERROR_INVALID_FUNCTION,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
    };

    for (const auto& testCase : cases) {
        auto api = std::make_shared<FakeApi>();
        api->portToCreate = nullptr;
        api->associationResult = nullptr;
        api->errorToReturn = testCase.nativeError;
        const auto result = Port::create(api);
        require(!result, "failed CreateIoCompletionPort returned an owner");
        require(result.error().code == testCase.stableCode,
                "creation failure returned the wrong stable error");
        require(result.error().retryable == testCase.retryable,
                "creation failure returned the wrong retryability");
        require(result.error().message.find(
                    std::to_string(testCase.nativeError)) != std::string::npos,
                "creation failure omitted its native error");
        require(api->createCalls == 1U && api->lastErrorCalls == 1U,
                "creation failure used the wrong native call counts");
        require(api->closeCalls == 0U,
                "failed creation attempted to close a null handle");
        require(api->eventCount() == 2U &&
                    api->event(0U) == Event::Create &&
                    api->event(1U) == Event::LastError,
                "creation failure used the wrong native ordering");
    }

    auto invalid = std::make_shared<FakeApi>();
    invalid->portToCreate = INVALID_HANDLE_VALUE;
    invalid->associationResult = INVALID_HANDLE_VALUE;
    invalid->errorToReturn = ERROR_INVALID_HANDLE;
    const auto result = Port::create(invalid);
    require(!result, "factory accepted INVALID_HANDLE_VALUE as a port");
    require(invalid->lastErrorCalls == 1U,
            "invalid creation handle did not read its native error");
    require(invalid->closeCalls == 0U,
            "invalid creation handle was incorrectly closed");
}

void handleAndSocketAssociationUseTypedKeysAndBorrowedOwnership()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = createPort(api);
    const HANDLE borrowedHandle = handleValue(0x2202U);
    const Key handleKey{static_cast<std::uintptr_t>(0xAABBCCDDU)};

    const auto associated = owner->associateHandle(borrowedHandle, handleKey);
    require(static_cast<bool>(associated), "valid handle association failed");
    require(api->associateCalls == 1U,
            "handle association did not make exactly one native call");
    require(api->associatedFileHandle == borrowedHandle,
            "handle association changed the borrowed handle");
    require(api->associatedExistingPort == api->portToCreate,
            "handle association did not target the owned port");
    require(api->associatedCompletionKey ==
                static_cast<ULONG_PTR>(handleKey.value()),
            "handle association changed the typed key");
    require(api->associatedConcurrency == 0U,
            "association attempted to change port concurrency");
    require(api->closeCalls == 0U,
            "association took ownership of the borrowed handle");

    const SOCKET borrowedSocket = static_cast<SOCKET>(0x3303U);
    const Key socketKey{0U};
    const auto socketAssociated = owner->associateSocket(
        borrowedSocket, socketKey);
    require(static_cast<bool>(socketAssociated), "valid socket association failed");
    require(api->associateCalls == 2U,
            "socket association did not make one additional native call");
    require(api->associatedFileHandle ==
                reinterpret_cast<HANDLE>(borrowedSocket),
            "socket association changed the borrowed socket value");
    require(api->associatedExistingPort == api->portToCreate,
            "socket association did not target the owned port");
    require(api->associatedCompletionKey == 0U,
            "socket association rejected or changed a zero typed key");
    require(api->associatedConcurrency == 0U,
            "socket association attempted to change port concurrency");
    require(api->lastErrorCalls == 0U,
            "successful association read a stale native error");

    owner.reset();
    require(api->closeCalls == 1U,
            "completion port was not closed after associations");
    require(api->closedHandle == api->portToCreate,
            "association caused the borrowed object to be closed");
}

void invalidBorrowedObjectsAreRejectedBeforeNativeAssociation()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = createPort(api);

    const auto nullHandle = owner->associateHandle(nullptr, Key{1U});
    require(!nullHandle, "association accepted a null handle");
    require(nullHandle.error().code == Domain::ErrorCodes::InvalidRequest,
            "null handle returned the wrong stable error");

    const auto invalidHandle = owner->associateHandle(
        INVALID_HANDLE_VALUE, Key{2U});
    require(!invalidHandle,
            "association accepted INVALID_HANDLE_VALUE");
    require(invalidHandle.error().code == Domain::ErrorCodes::InvalidRequest,
            "invalid handle returned the wrong stable error");

    const auto invalidSocket = owner->associateSocket(INVALID_SOCKET, Key{3U});
    require(!invalidSocket, "association accepted INVALID_SOCKET");
    require(invalidSocket.error().code == Domain::ErrorCodes::InvalidRequest,
            "invalid socket returned the wrong stable error");
    require(api->associateCalls == 0U,
            "invalid borrowed object reached CreateIoCompletionPort");
    require(api->lastErrorCalls == 0U,
            "validation failure read a native error");
}

void associationFailuresAreTypedWithoutTakingBorrowedOwnership()
{
    struct Case final {
        DWORD nativeError;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{ERROR_ACCESS_DENIED, Domain::ErrorCodes::Unauthorized, false},
        Case{ERROR_PRIVILEGE_NOT_HELD, Domain::ErrorCodes::Unauthorized, false},
        Case{ERROR_INVALID_PARAMETER, Domain::ErrorCodes::Conflict, false},
        Case{ERROR_NOT_ENOUGH_MEMORY, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_NO_SYSTEM_RESOURCES, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_GEN_FAILURE, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        auto api = std::make_shared<FakeApi>();
        api->associationResult = nullptr;
        api->errorToReturn = testCase.nativeError;
        auto owner = createPort(api);
        const HANDLE borrowed = handleValue(0x4404U);
        const auto result = owner->associateHandle(borrowed, Key{71U});
        require(!result, "failed association returned success");
        require(result.error().code == testCase.stableCode,
                "association failure returned the wrong stable error");
        require(result.error().retryable == testCase.retryable,
                "association failure returned the wrong retryability");
        require(result.error().message.find(
                    std::to_string(testCase.nativeError)) != std::string::npos,
                "association failure omitted its native error");
        require(api->associateCalls == 1U && api->lastErrorCalls == 1U,
                "association failure used the wrong native call counts");
        require(api->closeCalls == 0U,
                "association failure closed an object before owner teardown");
        owner.reset();
        require(api->closeCalls == 1U &&
                    api->closedHandle == api->portToCreate,
                "association failure did not close only the owned port");
    }
}

void mismatchedAssociationResultIsRejectedAsIntegrityFailure()
{
    auto api = std::make_shared<FakeApi>();
    api->associationResult = handleValue(0x9999U);
    auto owner = createPort(api);
    const auto result = owner->associateHandle(handleValue(0x5555U), Key{8U});
    require(!result, "association accepted a different returned port");
    require(result.error().code == Domain::ErrorCodes::IntegrityFailure,
            "mismatched association returned the wrong stable error");
    require(!result.error().retryable,
            "mismatched association was unexpectedly retryable");
    require(api->associateCalls == 1U,
            "mismatched association did not make exactly one native call");
    require(api->lastErrorCalls == 0U,
            "nonnull mismatched association read a stale native error");
    require(api->closeCalls == 0U,
            "mismatched association closed an unowned returned handle");
    owner.reset();
    require(api->closeCalls == 1U &&
                api->closedHandle == api->portToCreate,
            "mismatched association did not close exactly the owned port");
}

void postingPreservesPacketFieldsAndAllowsSyntheticPackets()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = createPort(api);
    OVERLAPPED operation{};
    const Key key{static_cast<std::uintptr_t>(0x10203040U)};

    const auto posted = owner->post(999U, key, &operation);
    require(static_cast<bool>(posted), "valid completion post failed");
    require(api->postCalls == 1U, "post did not make exactly one native call");
    require(api->postedPort == api->portToCreate,
            "post did not target the owned port");
    require(api->postedBytes == 999U, "post changed the byte count");
    require(api->postedKey == static_cast<ULONG_PTR>(key.value()),
            "post changed the typed completion key");
    require(api->postedOperation == &operation,
            "post changed the operation pointer");
    require(api->lastErrorCalls == 0U,
            "successful post read a stale native error");

    const auto synthetic = owner->post(0U, Key{0U}, nullptr);
    require(static_cast<bool>(synthetic), "synthetic completion post failed");
    require(api->postCalls == 2U,
            "synthetic post did not make one additional native call");
    require(api->postedBytes == 0U && api->postedKey == 0U &&
                api->postedOperation == nullptr,
            "synthetic completion fields were changed");
}

void postingFailuresAreTypedAndReadOneNativeError()
{
    struct Case final {
        DWORD nativeError;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{ERROR_INVALID_HANDLE, Domain::ErrorCodes::TransportClosed, true},
        Case{ERROR_ABANDONED_WAIT_0,
             Domain::ErrorCodes::TransportClosed, true},
        Case{ERROR_NOT_ENOUGH_MEMORY, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_NO_SYSTEM_RESOURCES, Domain::ErrorCodes::LimitExceeded, true},
        Case{ERROR_GEN_FAILURE, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        auto api = std::make_shared<FakeApi>();
        api->postStatus = FALSE;
        api->errorToReturn = testCase.nativeError;
        auto owner = createPort(api);
        OVERLAPPED operation{};
        const auto result = owner->post(17U, Key{18U}, &operation);
        require(!result, "failed PostQueuedCompletionStatus returned success");
        require(result.error().code == testCase.stableCode,
                "post failure returned the wrong stable error");
        require(result.error().retryable == testCase.retryable,
                "post failure returned the wrong retryability");
        require(result.error().message.find(
                    std::to_string(testCase.nativeError)) != std::string::npos,
                "post failure omitted its native error");
        require(api->postCalls == 1U && api->lastErrorCalls == 1U,
                "post failure used the wrong native call counts");
        require(api->postedPort == api->portToCreate &&
                    api->postedBytes == 17U && api->postedKey == 18U &&
                    api->postedOperation == &operation,
                "failed post changed a packet field");
    }
}

void controlPostingUsesAnAllocationFreeFixedResult()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = createPort(api);
    const Key key{static_cast<std::uintptr_t>(0x51525354U)};

    const ControlPostResult posted = owner->postControl(key);
    require(posted.succeeded(), "fixed control post reported false failure");
    require(posted.nativeError() == ERROR_SUCCESS,
            "successful fixed control post exposed a stale native error");
    require(api->postCalls == 1U && api->lastErrorCalls == 0U,
            "successful fixed control post used the wrong native calls");
    require(api->postedPort == api->portToCreate &&
                api->postedBytes == 0U &&
                api->postedKey == static_cast<ULONG_PTR>(key.value()) &&
                api->postedOperation == nullptr,
            "fixed control post changed the reserved packet shape");

    api->postStatus = FALSE;
    api->errorToReturn = ERROR_NOT_ENOUGH_MEMORY;
    const ControlPostResult failed = owner->postControl(key);
    require(!failed.succeeded(), "failed fixed control post reported success");
    require(failed.nativeError() == ERROR_NOT_ENOUGH_MEMORY,
            "failed fixed control post changed its native error");
    require(api->postCalls == 2U && api->lastErrorCalls == 1U,
            "failed fixed control post used the wrong native calls");
}

void successfulDequeueReturnsAnExactPacketWithoutReadingLastError()
{
    auto api = std::make_shared<FakeApi>();
    api->dequeueStatus = TRUE;
    api->dequeueBytes = 412U;
    api->dequeueKey = static_cast<ULONG_PTR>(0xABCDEFU);
    OVERLAPPED operation{};
    api->dequeueOperation = &operation;
    auto owner = createPort(api);

    const DequeueResult result = owner->dequeue(37U);
    require(result.disposition() == DequeueDisposition::Succeeded,
            "successful dequeue returned the wrong disposition");
    require(result.hasPacket() && result.packet() != nullptr,
            "successful dequeue omitted its packet");
    require(result.packet()->transferredBytes == 412U,
            "successful dequeue changed the byte count");
    require(result.packet()->completionKey == Key{0xABCDEFU},
            "successful dequeue changed the typed key");
    require(result.packet()->operation == &operation,
            "successful dequeue changed the operation pointer");
    require(result.nativeError() == ERROR_SUCCESS,
            "successful dequeue exposed a native error");
    require(api->dequeueCalls == 1U && api->lastErrorCalls == 0U,
            "successful dequeue used the wrong native call counts");
    require(api->dequeuedPort == api->portToCreate,
            "dequeue did not target the owned port");
    require(api->dequeueTimeout == 37U,
            "dequeue changed the caller's bounded timeout");
    require(api->initialTransferredBytes == 0U &&
                api->initialCompletionKey == 0U &&
                api->initialOperation == nullptr,
            "dequeue outputs were not initialized before the native call");

    api->dequeueBytes = 0U;
    api->dequeueKey = 0U;
    api->dequeueOperation = nullptr;
    const DequeueResult synthetic = owner->dequeue(0U);
    require(synthetic.disposition() == DequeueDisposition::Succeeded,
            "synthetic dequeue returned the wrong disposition");
    require(synthetic.hasPacket() && synthetic.packet() != nullptr,
            "synthetic dequeue omitted its packet model");
    require(synthetic.packet()->transferredBytes == 0U &&
                synthetic.packet()->completionKey == Key{0U} &&
                synthetic.packet()->operation == nullptr,
            "synthetic dequeue changed its zero packet fields");
}

void failedIoPacketIsDistinctFromTimeoutAndFatalDequeue()
{
    auto api = std::make_shared<FakeApi>();
    api->dequeueStatus = FALSE;
    api->dequeueBytes = 73U;
    api->dequeueKey = 74U;
    api->errorToReturn = ERROR_OPERATION_ABORTED;
    OVERLAPPED operation{};
    api->dequeueOperation = &operation;
    auto owner = createPort(api);

    const DequeueResult result = owner->dequeue(55U);
    require(result.disposition() == DequeueDisposition::IoFailed,
            "failed I/O packet returned the wrong disposition");
    require(result.hasPacket() && result.packet() != nullptr,
            "failed I/O packet omitted its packet fields");
    require(result.packet()->transferredBytes == 73U &&
                result.packet()->completionKey == Key{74U} &&
                result.packet()->operation == &operation,
            "failed I/O packet changed its completion fields");
    require(result.nativeError() == ERROR_OPERATION_ABORTED,
            "failed I/O packet changed its native error");
    require(api->dequeueCalls == 1U && api->lastErrorCalls == 1U,
            "failed I/O packet used the wrong native call counts");

    api->errorToReturn = WAIT_TIMEOUT;
    const DequeueResult pathological = owner->dequeue(56U);
    require(pathological.disposition() == DequeueDisposition::IoFailed,
            "nonnull OVERLAPPED was misclassified as a timeout");
    require(pathological.hasPacket() &&
                pathological.nativeError() == WAIT_TIMEOUT,
            "nonnull OVERLAPPED did not retain its reported error");
}

void nullOperationTimeoutAndFatalErrorsAreDistinctAndPacketless()
{
    auto api = std::make_shared<FakeApi>();
    api->dequeueStatus = FALSE;
    api->dequeueOperation = nullptr;
    api->dequeueBytes = 91U;
    api->dequeueKey = 92U;
    api->errorToReturn = WAIT_TIMEOUT;
    auto owner = createPort(api);

    const DequeueResult timedOut = owner->dequeue(63U);
    require(timedOut.disposition() == DequeueDisposition::TimedOut,
            "bounded wait timeout returned the wrong disposition");
    require(!timedOut.hasPacket() && timedOut.packet() == nullptr,
            "bounded wait timeout exposed stale packet fields");
    require(timedOut.nativeError() == WAIT_TIMEOUT,
            "bounded wait timeout changed its native error");

    api->errorToReturn = ERROR_INVALID_HANDLE;
    const DequeueResult fatal = owner->dequeue(64U);
    require(fatal.disposition() == DequeueDisposition::FatalError,
            "fatal dequeue returned the wrong disposition");
    require(!fatal.hasPacket() && fatal.packet() == nullptr,
            "fatal dequeue exposed stale packet fields");
    require(fatal.nativeError() == ERROR_INVALID_HANDLE,
            "fatal dequeue changed its native error");
    require(api->dequeueCalls == 2U && api->lastErrorCalls == 2U,
            "timeout and fatal dequeues used the wrong native call counts");
}

void systemApiCreatePostAndDequeueSmokeIsSafe()
{
    auto owner = take(Port::create());
    require(owner != nullptr, "real completion-port factory returned null");

    const DequeueResult empty = owner->dequeue(0U);
    require(empty.disposition() == DequeueDisposition::TimedOut,
            "empty real completion port did not time out");
    require(!empty.hasPacket() && empty.nativeError() == WAIT_TIMEOUT,
            "empty real completion port returned stale packet state");

    constexpr DWORD Bytes = 0x1234U;
    const Key key{static_cast<std::uintptr_t>(0x5678U)};
    const auto posted = owner->post(Bytes, key, nullptr);
    require(static_cast<bool>(posted),
            "real synthetic completion post failed");

    const DequeueResult dequeued = owner->dequeue(1'000U);
    require(dequeued.disposition() == DequeueDisposition::Succeeded,
            "real posted packet did not dequeue successfully");
    require(dequeued.hasPacket() && dequeued.packet() != nullptr,
            "real posted packet omitted its packet model");
    require(dequeued.packet()->transferredBytes == Bytes,
            "real posted packet changed the byte count");
    require(dequeued.packet()->completionKey == key,
            "real posted packet changed the typed key");
    require(dequeued.packet()->operation == nullptr,
            "real synthetic packet gained an operation pointer");
    require(dequeued.nativeError() == ERROR_SUCCESS,
            "real successful dequeue exposed a native error");
}

} // namespace

int main()
{
    try {
        creationUsesOnePortAndExactlyFourConcurrencyThreads();
        nullApiIsRejectedWithoutNativeCalls();
        creationFailuresAreTypedAndNeverClosed();
        handleAndSocketAssociationUseTypedKeysAndBorrowedOwnership();
        invalidBorrowedObjectsAreRejectedBeforeNativeAssociation();
        associationFailuresAreTypedWithoutTakingBorrowedOwnership();
        mismatchedAssociationResultIsRejectedAsIntegrityFailure();
        postingPreservesPacketFieldsAndAllowsSyntheticPackets();
        postingFailuresAreTypedAndReadOneNativeError();
        controlPostingUsesAnAllocationFreeFixedResult();
        successfulDequeueReturnsAnExactPacketWithoutReadingLastError();
        failedIoPacketIsDistinctFromTimeoutAndFatalDequeue();
        nullOperationTimeoutAndFatalErrorsAreDistinctAndPacketless();
        systemApiCreatePostAndDequeueSmokeIsSafe();
        std::cout << "Dashboard IOCP owner tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard IOCP owner tests failed after "
                  << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard IOCP owner tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
