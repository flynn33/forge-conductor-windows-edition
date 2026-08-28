#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Api = Detail::IDashboardIoCompletionPortApi;
using Key = Detail::DashboardIoCompletionKey;
using Kernel = Detail::DashboardIocpWorkerKernel;
using Packet = Detail::DashboardIoCompletionPacket;
using Port = Detail::DashboardIoCompletionPort;
using Sink = Detail::IDashboardIocpCompletionSink;

static_assert(std::is_abstract_v<Sink>);
static_assert(std::is_final_v<Kernel>);
static_assert(!std::is_copy_constructible_v<Kernel>);
static_assert(!std::is_move_constructible_v<Kernel>);
static_assert(Kernel::WorkerCount == 4U);
static_assert(
    Kernel::WorkerCount == Port::RequiredConcurrencyThreadCount);
static_assert(Kernel::WorkerWaitMilliseconds != INFINITE);
static_assert(Kernel::WorkerStartupTimeout == std::chrono::seconds{5});
static_assert(Kernel::ShutdownDrainTimeout == std::chrono::seconds{5});

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

class FakeApi final : public Api {
public:
    enum class CompletionKind : std::uint8_t {
        Succeeded,
        IoFailed,
        Fatal,
    };

    struct Completion final {
        CompletionKind kind{CompletionKind::Succeeded};
        DWORD transferredBytes{};
        ULONG_PTR completionKey{};
        OVERLAPPED* operation{};
        DWORD error{};
    };

    [[nodiscard]] HANDLE createIoCompletionPort(
        const HANDLE fileHandle,
        const HANDLE existingCompletionPort,
        const ULONG_PTR completionKey,
        const DWORD concurrentThreadCount) noexcept override
    {
        const std::lock_guard lock{mutex_};
        if (fileHandle == INVALID_HANDLE_VALUE) {
            ++createCalls;
            createConcurrency = concurrentThreadCount;
            createKey = completionKey;
            return failCreate ? nullptr : portHandle();
        }
        ++associationCalls;
        associatedHandle = fileHandle;
        associatedKey = completionKey;
        associationConcurrency = concurrentThreadCount;
        return failAssociation ? nullptr : existingCompletionPort;
    }

    [[nodiscard]] BOOL postQueuedCompletionStatus(
        const HANDLE completionPort,
        const DWORD transferredBytes,
        const ULONG_PTR completionKey,
        OVERLAPPED* const operation) noexcept override
    {
        std::unique_lock lock{mutex_};
        ++postCalls;
        lastPostPort = completionPort;
        if (blockDataPost_ &&
            completionKey != Kernel::ShutdownKeyValue) {
            dataPostEntered_ = true;
            stateChanged_.notify_all();
            stateChanged_.wait(lock, [this] { return releaseDataPost_; });
        }
        if (failPosts) {
            setThreadError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        completions_.push_back(Completion{
            CompletionKind::Succeeded,
            transferredBytes,
            completionKey,
            operation,
            ERROR_SUCCESS});
        stateChanged_.notify_one();
        return TRUE;
    }

    [[nodiscard]] BOOL getQueuedCompletionStatus(
        const HANDLE completionPort,
        DWORD& transferredBytes,
        ULONG_PTR& completionKey,
        OVERLAPPED*& operation,
        const DWORD timeoutMilliseconds) noexcept override
    {
        std::unique_lock lock{mutex_};
        ++dequeueCalls;
        lastDequeuePort = completionPort;
        lastDequeueTimeout = timeoutMilliseconds;
        everyDequeueWasBounded_ = everyDequeueWasBounded_ &&
            timeoutMilliseconds == Kernel::WorkerWaitMilliseconds;
        const auto workerId = std::this_thread::get_id();
        bool knownWorker{};
        for (std::size_t index{}; index < dequeueWorkerCount_; ++index) {
            knownWorker = knownWorker || dequeueWorkerIds_[index] == workerId;
        }
        if (!knownWorker && dequeueWorkerCount_ < dequeueWorkerIds_.size()) {
            dequeueWorkerIds_[dequeueWorkerCount_] = workerId;
            ++dequeueWorkerCount_;
            stateChanged_.notify_all();
        }
        const auto ready = [this] {
            return !completions_.empty() || closed_;
        };
        if (!ready()) {
            static_cast<void>(stateChanged_.wait_for(
                lock,
                std::chrono::milliseconds{timeoutMilliseconds},
                ready));
        }
        if (completions_.empty()) {
            transferredBytes = 0U;
            completionKey = 0U;
            operation = nullptr;
            setThreadError(closed_ ? ERROR_INVALID_HANDLE : WAIT_TIMEOUT);
            return FALSE;
        }

        const auto completion = completions_.front();
        completions_.pop_front();
        transferredBytes = completion.transferredBytes;
        completionKey = completion.completionKey;
        operation = completion.operation;
        setThreadError(completion.error);
        return completion.kind == CompletionKind::Succeeded ? TRUE : FALSE;
    }

    [[nodiscard]] BOOL closeHandle(const HANDLE handle) noexcept override
    {
        const std::lock_guard lock{mutex_};
        ++closeCalls;
        lastClosedHandle = handle;
        closed_ = true;
        stateChanged_.notify_all();
        return TRUE;
    }

    [[nodiscard]] DWORD lastError() noexcept override
    {
        return threadError();
    }

    void injectIoFailure(
        const DWORD transferredBytes,
        const std::uintptr_t completionKey,
        OVERLAPPED* const operation,
        const DWORD error)
    {
        const std::lock_guard lock{mutex_};
        completions_.push_back(Completion{
            CompletionKind::IoFailed,
            transferredBytes,
            completionKey,
            operation,
            error});
        stateChanged_.notify_one();
    }

    void injectFatal(const DWORD error)
    {
        const std::lock_guard lock{mutex_};
        completions_.push_back(Completion{
            CompletionKind::Fatal, 0U, 0U, nullptr, error});
        stateChanged_.notify_one();
    }

    void blockNextDataPost()
    {
        const std::lock_guard lock{mutex_};
        blockDataPost_ = true;
        releaseDataPost_ = false;
        dataPostEntered_ = false;
    }

    [[nodiscard]] bool waitForBlockedDataPost(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(
            lock, timeout, [this] { return dataPostEntered_; });
    }

    [[nodiscard]] bool waitForDequeueWorkerCount(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return stateChanged_.wait_for(
            lock,
            timeout,
            [this, count] { return dequeueWorkerCount_ >= count; });
    }

    void releaseBlockedDataPost()
    {
        const std::lock_guard lock{mutex_};
        releaseDataPost_ = true;
        stateChanged_.notify_all();
    }

    void failEveryPost() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            failPosts = true;
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::size_t postCallCount() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return postCalls;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] DWORD dequeueTimeout() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return lastDequeueTimeout;
        } catch (...) {
            return INFINITE;
        }
    }

    [[nodiscard]] bool everyDequeueWasBounded() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return everyDequeueWasBounded_;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] static HANDLE portHandle() noexcept
    {
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(0x1A11U));
    }

    bool failCreate{};
    bool failAssociation{};
    bool failPosts{};
    std::size_t createCalls{};
    std::size_t associationCalls{};
    std::size_t postCalls{};
    std::size_t dequeueCalls{};
    std::size_t closeCalls{};
    DWORD createConcurrency{};
    ULONG_PTR createKey{};
    HANDLE associatedHandle{};
    ULONG_PTR associatedKey{};
    DWORD associationConcurrency{};
    HANDLE lastPostPort{};
    HANDLE lastDequeuePort{};
    DWORD lastDequeueTimeout{};
    HANDLE lastClosedHandle{};

private:
    [[nodiscard]] static DWORD& threadError() noexcept
    {
        thread_local DWORD value{ERROR_SUCCESS};
        return value;
    }

    static void setThreadError(const DWORD error) noexcept
    {
        threadError() = error;
    }

    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    std::deque<Completion> completions_;
    std::array<std::thread::id, Kernel::WorkerCount> dequeueWorkerIds_{};
    std::size_t dequeueWorkerCount_{};
    bool closed_{};
    bool everyDequeueWasBounded_{true};
    bool blockDataPost_{};
    bool releaseDataPost_{};
    bool dataPostEntered_{};
};

class RecordingSink final : public Sink {
public:
    struct Observation final {
        Packet packet;
        DWORD nativeError{};
        std::thread::id workerId;
    };

    void consume(Packet packet, const DWORD nativeError) noexcept override
    {
        try {
            const std::lock_guard lock{mutex_};
            observations_.push_back(Observation{
                packet, nativeError, std::this_thread::get_id()});
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        Kernel* shutdownOwner{};
        try {
            {
                const std::lock_guard lock{mutex_};
                ++fatalCount_;
                if (!fatalError_) {
                    fatalError_ = nativeError;
                    fatalWorkerId_ = std::this_thread::get_id();
                }
                shutdownOwner = shutdownOwner_;
            }
            changed_.notify_all();
            if (shutdownOwner != nullptr) {
                shutdownOwner->shutdown();
            }
        } catch (...) {
            std::terminate();
        }
    }

    void requestShutdownOnFatal(Kernel& owner) noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            shutdownOwner_ = std::addressof(owner);
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitForFatal(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this] { return fatalError_.has_value(); });
    }

    [[nodiscard]] std::optional<DWORD> fatalError() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return fatalError_;
        } catch (...) {
            return ERROR_GEN_FAILURE;
        }
    }

    [[nodiscard]] std::size_t fatalCount() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return fatalCount_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] bool waitForCount(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this, count] {
                return observations_.size() >= count;
            });
    }

    [[nodiscard]] Observation observation(const std::size_t index) const
    {
        const std::lock_guard lock{mutex_};
        if (index >= observations_.size()) {
            fail("completion observation index was out of range");
        }
        return observations_[index];
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return observations_.size();
        } catch (...) {
            return 0U;
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Observation> observations_;
    std::optional<DWORD> fatalError_;
    std::thread::id fatalWorkerId_;
    Kernel* shutdownOwner_{};
    std::size_t fatalCount_{};
};

[[nodiscard]] std::unique_ptr<Port> port(
    const std::shared_ptr<FakeApi>& api)
{
    return take(Port::create(api));
}

[[nodiscard]] std::unique_ptr<Kernel> kernel(
    const std::shared_ptr<FakeApi>& api,
    const std::shared_ptr<RecordingSink>& sink)
{
    return take(Kernel::create(port(api), sink));
}

void creationOwnsExactlyFourWorkersAndOnePort()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);

    require(api->waitForDequeueWorkerCount(
                Kernel::WorkerCount, std::chrono::seconds{2}),
            "four distinct workers did not enter the completion loop");

    const auto snapshot = owner->snapshot();
    require(snapshot.startedWorkerCount() == 4U,
            "kernel did not start exactly four workers");
    require(snapshot.exitedWorkerCount() == 0U,
            "new workers had already exited");
    require(!snapshot.isShuttingDown(),
            "new kernel reported shutdown");
    require(!snapshot.controlPostFailed(),
            "new kernel reported a control-post failure");
    require(!snapshot.fatalNativeError().has_value(),
            "new kernel reported a fatal dequeue error");
    require(api->createCalls == 1U,
            "kernel did not retain one completion port");
    require(api->createConcurrency == 4U,
            "completion port concurrency was not exactly four");
    require(api->closeCalls == 0U,
            "live kernel closed its completion port");

    owner->shutdown();
    const auto stopped = owner->snapshot();
    require(stopped.startedWorkerCount() == 4U,
            "shutdown changed the started-worker count");
    require(stopped.exitedWorkerCount() == 4U,
            "shutdown did not reap all four workers");
    require(stopped.isShuttingDown(),
            "stopped kernel did not retain its terminal state");
    require(api->postCallCount() == 4U,
            "shutdown did not post exactly four control packets");
    require(api->closeCalls == 0U,
            "shutdown closed the port before kernel destruction");

    owner.reset();
    require(api->closeCalls == 1U,
            "kernel destruction did not close the port exactly once");
    require(api->lastClosedHandle == FakeApi::portHandle(),
            "kernel closed the wrong handle");
}

void packetDispatchPreservesSuccessAndIoFailure()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);
    OVERLAPPED successful{};
    OVERLAPPED failed{};

    auto posted = owner->postAdmitted(17U, Key{71U}, &successful);
    require(static_cast<bool>(posted),
            "admitted completion could not be posted");
    api->injectIoFailure(9U, 72U, &failed, ERROR_OPERATION_ABORTED);
    require(sink->waitForCount(2U, std::chrono::seconds{2}),
            "workers did not dispatch both completion packets");

    const auto first = sink->observation(0U);
    const auto second = sink->observation(1U);
    const std::array observations{first, second};
    bool sawSuccess{};
    bool sawFailure{};
    for (const auto& observation : observations) {
        if (observation.packet.completionKey == Key{71U}) {
            sawSuccess = true;
            require(observation.packet.transferredBytes == 17U,
                    "successful packet changed its byte count");
            require(observation.packet.operation == &successful,
                    "successful packet changed its operation pointer");
            require(observation.nativeError == ERROR_SUCCESS,
                    "successful packet carried a native failure");
        } else if (observation.packet.completionKey == Key{72U}) {
            sawFailure = true;
            require(observation.packet.transferredBytes == 9U,
                    "failed packet changed its byte count");
            require(observation.packet.operation == &failed,
                    "failed packet changed its operation pointer");
            require(observation.nativeError == ERROR_OPERATION_ABORTED,
                    "failed packet lost its native error");
        }
    }
    require(sawSuccess && sawFailure,
            "worker dispatch lost a success or failed-I/O packet");
    require(!owner->snapshot().fatalNativeError().has_value(),
            "failed I/O packet was misclassified as a fatal dequeue");
}

void associationAndReservedKeyPolicyAreExact()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);
    const auto borrowed = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(0x2112U));

    require(static_cast<bool>(owner->associateHandle(borrowed, Key{81U})),
            "valid handle association failed");
    require(api->associationCalls == 1U,
            "handle association did not reach the native API once");
    require(api->associatedHandle == borrowed && api->associatedKey == 81U,
            "handle association changed its handle or key");
    require(api->associationConcurrency == 0U,
            "association did not use the ignored zero concurrency value");

    const Key reserved{Kernel::ShutdownKeyValue};
    const auto rejectedAssociation = owner->associateHandle(borrowed, reserved);
    require(!rejectedAssociation,
            "reserved shutdown key was associated with a handle");
    require(rejectedAssociation.error().code == Domain::ErrorCodes::InvalidRequest,
            "reserved association used the wrong error code");
    const auto rejectedPost = owner->postAdmitted(0U, reserved, nullptr);
    require(!rejectedPost, "reserved shutdown packet was publicly posted");
    require(api->associationCalls == 1U,
            "reserved association reached the native API");
    require(api->postCallCount() == 0U,
            "reserved public post reached the native API");
}

void admittedPostFinishesBeforeShutdownWorkersExit()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);
    OVERLAPPED operation{};
    api->blockNextDataPost();

    std::optional<Domain::Result<void>> result;
    std::thread publisher{[&] {
        result.emplace(owner->postAdmitted(23U, Key{91U}, &operation));
    }};
    const bool enteredNativePost =
        api->waitForBlockedDataPost(std::chrono::seconds{2});

    owner->beginShutdown();
    const bool admissionClosed = owner->snapshot().isShuttingDown();
    const auto holdPastMultipleWaits = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(
            Kernel::WorkerWaitMilliseconds * 2U + 50U)};
    std::this_thread::sleep_for(holdPastMultipleWaits);
    const auto blockedSnapshot = owner->snapshot();
    const auto postsBeforeRelease = api->postCallCount();

    api->releaseBlockedDataPost();
    publisher.join();
    const bool packetConsumed =
        sink->waitForCount(1U, std::chrono::seconds{2});
    owner->shutdown();
    require(enteredNativePost,
            "test post did not enter the native API");
    require(admissionClosed,
            "beginShutdown did not close admission");
    require(blockedSnapshot.exitedWorkerCount() == 0U,
            "workers exited while an admitted native post still owned the control barrier");
    require(postsBeforeRelease == 1U,
            "shutdown posted control packets before admitted native work finished");
    require(result.has_value() && static_cast<bool>(*result),
            "in-flight admitted post failed during shutdown");
    require(packetConsumed,
            "in-flight completion was lost behind shutdown packets");
    require(owner->snapshot().exitedWorkerCount() == 4U,
            "shutdown did not join workers after the in-flight post");
    require(api->postCallCount() == 5U,
            "shutdown used other than one data and four control posts");

    const auto after = owner->postAdmitted(1U, Key{92U}, nullptr);
    require(!after, "post after shutdown was accepted");
    require(after.error().code == Domain::ErrorCodes::TransportClosed,
            "post after shutdown used the wrong error code");
    require(api->postCallCount() == 5U,
            "rejected post after shutdown reached the native API");
}

void fatalDequeueStopsEveryWorkerAndIsRetained()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);
    sink->requestShutdownOnFatal(*owner);
    api->injectFatal(ERROR_INVALID_HANDLE);

    constexpr DWORD fatalError =
        static_cast<DWORD>(ERROR_INVALID_HANDLE);
    require(sink->waitForFatal(std::chrono::seconds{2}),
            "fatal dequeue did not notify its owner");
    require(sink->fatalError() == fatalError,
            "fatal callback changed the native error");
    require(sink->fatalCount() == 1U,
            "fatal dequeue notified its owner more than once");

    owner->shutdown();
    const auto snapshot = owner->snapshot();
    require(snapshot.isShuttingDown(),
            "fatal dequeue did not stop the kernel");
    require(snapshot.exitedWorkerCount() == 4U,
            "fatal dequeue did not reap every worker");
    require(snapshot.fatalNativeError() == fatalError,
            "fatal dequeue error was not retained exactly");
    require(sink->count() == 0U,
            "fatal dequeue was forwarded as an operation packet");
}

void failedControlPostsUseFiniteWaitFallback()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    auto owner = kernel(api, sink);
    api->failEveryPost();

    owner->shutdown();
    const auto snapshot = owner->snapshot();
    require(snapshot.exitedWorkerCount() == 4U,
            "finite wait fallback did not reap all workers");
    require(snapshot.controlPostFailed(),
            "failed shutdown posts were not retained");
    require(api->dequeueTimeout() == Kernel::WorkerWaitMilliseconds,
            "worker used an unbounded or unexpected dequeue wait");
    require(api->everyDequeueWasBounded(),
            "a worker used a dequeue wait other than the exact finite bound");
}

void nullDependenciesFailWithoutLeakingThePort()
{
    auto api = std::make_shared<FakeApi>();
    auto sink = std::make_shared<RecordingSink>();
    const auto nullPort = Kernel::create(nullptr, sink);
    require(!nullPort, "kernel accepted a null completion port");
    require(nullPort.error().code == Domain::ErrorCodes::InvalidRequest,
            "null port used the wrong error code");

    auto ownedPort = port(api);
    const auto nullSink = Kernel::create(std::move(ownedPort), nullptr);
    require(!nullSink, "kernel accepted a null completion sink");
    require(nullSink.error().code == Domain::ErrorCodes::InvalidRequest,
            "null sink used the wrong error code");
    require(api->closeCalls == 1U,
            "rejected null sink leaked its completion port");
}

void realPortDispatchSmoke()
{
    auto realPort = take(Port::create());
    auto sink = std::make_shared<RecordingSink>();
    auto owner = take(Kernel::create(std::move(realPort), sink));
    OVERLAPPED operation{};
    require(static_cast<bool>(owner->postAdmitted(29U, Key{101U}, &operation)),
            "real completion port rejected an admitted synthetic packet");
    require(sink->waitForCount(1U, std::chrono::seconds{2}),
            "real workers did not dispatch a synthetic packet");
    const auto observed = sink->observation(0U);
    require(observed.packet.transferredBytes == 29U,
            "real completion port changed the byte count");
    require(observed.packet.completionKey == Key{101U},
            "real completion port changed the key");
    require(observed.packet.operation == &operation,
            "real completion port changed the operation pointer");
    require(observed.nativeError == ERROR_SUCCESS,
            "real completion port reported a false failure");
    owner->shutdown();
    require(owner->snapshot().exitedWorkerCount() == 4U,
            "real completion port did not stop all workers");
}

} // namespace

int main()
{
    try {
        creationOwnsExactlyFourWorkersAndOnePort();
        packetDispatchPreservesSuccessAndIoFailure();
        associationAndReservedKeyPolicyAreExact();
        admittedPostFinishesBeforeShutdownWorkersExit();
        fatalDequeueStopsEveryWorkerAndIsRetained();
        failedControlPostsUseFiniteWaitFallback();
        nullDependenciesFailWithoutLeakingThePort();
        realPortDispatchSmoke();
        std::cout << "Dashboard IOCP worker kernel tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard IOCP worker kernel tests failed after "
                  << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard IOCP worker kernel tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
