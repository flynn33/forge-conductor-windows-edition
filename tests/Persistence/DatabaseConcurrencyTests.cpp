#include "PersistenceTestSupport.h"

#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Persistence/Windows/Detail/AnchoredSqliteVfs.h"
#include "Persistence/Windows/Detail/DatabaseNamespaceLease.h"
#include "Persistence/Windows/Detail/WinsqliteConnection.h"
#include "Persistence/Windows/Detail/WinsqliteStatement.h"
#include "Persistence/Windows/Detail/WinsqliteTransaction.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
namespace PersistenceDetail = ForgeConductor::Persistence::Windows::Detail;

using namespace std::chrono_literals;
using PersistenceSupport::ScopedTestDirectory;

constexpr DWORD ChildWaitMilliseconds = 15'000U;

[[nodiscard]] std::filesystem::path canonicalPath(
    const std::filesystem::path& path,
    const std::string_view failure)
{
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    require(!error, failure);
    return canonical;
}

[[nodiscard]] std::filesystem::path requireProcessFixture(
    const std::filesystem::path& processFixture)
{
    const auto canonical = canonicalPath(
        processFixture, "the persistence process fixture could not be canonicalized");
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(canonical, error);
    require(!error && regular,
            "the persistence process fixture is not a regular file");
    return canonical;
}

[[nodiscard]] Domain::OperationContext concurrencyContext(
    const std::string_view correlation,
    const std::chrono::milliseconds timeout = 30s,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("83000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + timeout,
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> makeNamespace(
    const std::filesystem::path& directory,
    const std::wstring_view mainBasename,
    const std::wstring_view lockBasename)
{
    return take(PersistenceDetail::DatabaseNamespaceLease::create(
        canonicalPath(directory, "the concurrency directory could not be canonicalized").native(),
        mainBasename,
        lockBasename));
}

[[nodiscard]] std::wstring quoteArgument(const std::wstring_view argument)
{
    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashes{};
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append((backslashes * 2U) + 1U, L'\\');
            quoted.push_back(character);
            backslashes = 0U;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::wstring makeCommandLine(
    const std::filesystem::path& executable,
    const std::span<const std::wstring> arguments)
{
    std::wstring commandLine = quoteArgument(executable.native());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(argument);
    }
    require(commandLine.size() < 32'768U,
            "the persistence fixture command line exceeds the native bound");
    return commandLine;
}

class ChildProcess final {
public:
    [[nodiscard]] static ChildProcess launch(
        const std::filesystem::path& executable,
        const std::span<const std::wstring> arguments)
    {
        InfrastructureDetail::UniqueHandle job{::CreateJobObjectW(nullptr, nullptr)};
        require(static_cast<bool>(job),
                "the persistence fixture job object could not be created");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(::SetInformationJobObject(
                    job.get(), JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)) != FALSE,
                "the persistence fixture kill-on-close policy could not be applied");

        std::wstring commandLine = makeCommandLine(executable, arguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        const BOOL created = ::CreateProcessW(
            executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
            &startup, &information);
        const DWORD createError = created != FALSE ? ERROR_SUCCESS : ::GetLastError();
        require(created != FALSE,
                std::string{"the persistence process fixture could not be launched; native error "} +
                    std::to_string(createError));

        InfrastructureDetail::UniqueHandle process{information.hProcess};
        InfrastructureDetail::UniqueHandle thread{information.hThread};
        if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
            const DWORD assignError = ::GetLastError();
            static_cast<void>(::TerminateProcess(process.get(), 91U));
            static_cast<void>(::WaitForSingleObject(process.get(), 5'000U));
            require(false,
                    std::string{"the persistence fixture could not enter its kill-on-close job; "
                                "native error "} + std::to_string(assignError));
        }
        if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            const DWORD resumeError = ::GetLastError();
            static_cast<void>(::TerminateProcess(process.get(), 92U));
            static_cast<void>(::WaitForSingleObject(process.get(), 5'000U));
            require(false,
                    std::string{"the persistence fixture initial thread could not resume; native "
                                "error "} + std::to_string(resumeError));
        }
        thread.reset();
        return ChildProcess{std::move(job), std::move(process)};
    }

    ~ChildProcess() noexcept
    {
        if (process_) {
            DWORD code{};
            if (::GetExitCodeProcess(process_.get(), &code) != FALSE &&
                code == STILL_ACTIVE) {
                static_cast<void>(::TerminateProcess(process_.get(), 93U));
                static_cast<void>(::WaitForSingleObject(process_.get(), 5'000U));
            }
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) noexcept = default;
    ChildProcess& operator=(ChildProcess&&) noexcept = default;

    [[nodiscard]] HANDLE nativeHandle() const noexcept { return process_.get(); }

    [[nodiscard]] DWORD exitCode() const
    {
        DWORD code{};
        require(::GetExitCodeProcess(process_.get(), &code) != FALSE,
                "the persistence fixture exit code could not be read");
        require(code != STILL_ACTIVE,
                "the persistence fixture was still active when its exit code was read");
        return code;
    }

    [[nodiscard]] DWORD wait(const DWORD timeoutMilliseconds) const
    {
        const DWORD waitResult =
            ::WaitForSingleObject(process_.get(), timeoutMilliseconds);
        require(waitResult == WAIT_OBJECT_0,
                waitResult == WAIT_TIMEOUT
                    ? "the persistence fixture exceeded its bounded shutdown wait"
                    : "waiting for the persistence fixture failed");
        return exitCode();
    }

private:
    ChildProcess(
        InfrastructureDetail::UniqueHandle job,
        InfrastructureDetail::UniqueHandle process) noexcept
        : job_{std::move(job)}, process_{std::move(process)}
    {
    }

    InfrastructureDetail::UniqueHandle job_;
    InfrastructureDetail::UniqueHandle process_;
};

struct NamedEvent final {
    std::wstring name;
    InfrastructureDetail::UniqueHandle handle;
};

[[nodiscard]] NamedEvent createNamedEvent(const std::wstring_view label)
{
    LARGE_INTEGER counter{};
    require(::QueryPerformanceCounter(&counter) != FALSE,
            "a persistence event nonce could not be generated");
    std::wstring name =
        L"Local\\ForgeConductor-P07-" + std::wstring{label} + L"-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetCurrentThreadId()) + L"-" +
        std::to_wstring(static_cast<unsigned long long>(counter.QuadPart));
    InfrastructureDetail::UniqueHandle handle{
        ::CreateEventW(nullptr, TRUE, FALSE, name.c_str())};
    const DWORD createError = ::GetLastError();
    require(static_cast<bool>(handle) && createError != ERROR_ALREADY_EXISTS,
            "a unique persistence coordination event could not be created");
    return NamedEvent{std::move(name), std::move(handle)};
}

void signalEvent(const NamedEvent& event, const std::string_view failure)
{
    require(::SetEvent(event.handle.get()) != FALSE, failure);
}

void waitForReadyOrChild(
    const NamedEvent& ready,
    const ChildProcess& child,
    const std::string_view earlyExit,
    const std::string_view timeout)
{
    const std::array<HANDLE, 2U> handles{ready.handle.get(), child.nativeHandle()};
    const DWORD waitResult = ::WaitForMultipleObjects(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        ChildWaitMilliseconds);
    if (waitResult == WAIT_OBJECT_0 + 1U) {
        require(false,
                std::string{earlyExit} + "; exit code " +
                    std::to_string(child.exitCode()));
    }
    require(waitResult == WAIT_OBJECT_0,
            waitResult == WAIT_TIMEOUT ? timeout
                                       : "waiting for fixture readiness failed");
}

class AnchoredDatabase final {
public:
    [[nodiscard]] static std::unique_ptr<AnchoredDatabase> open(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        const PersistenceDetail::WinsqliteOpenMode openMode,
        const Domain::OperationContext& context)
    {
        auto vfs = take(PersistenceDetail::AnchoredSqliteVfs::create(namespaceLease));
        auto connection = take(PersistenceDetail::WinsqliteConnection::open(
            namespaceLease->canonicalMainDatabasePath(),
            PersistenceDetail::WinsqliteConnectionOptions{
                std::string{vfs->vfsName()}, openMode,
                PersistenceDetail::WinsqliteSynchronousMode::Full,
                PersistenceDetail::WinsqliteJournalMode::WriteAheadLog,
                namespaceLease},
            context));
        return std::unique_ptr<AnchoredDatabase>{new AnchoredDatabase{
            std::move(namespaceLease), std::move(vfs), std::move(connection)}};
    }

    ~AnchoredDatabase() noexcept
    {
        connection_.reset();
        if (vfs_) {
            static_cast<void>(vfs_->close());
        }
    }

    AnchoredDatabase(const AnchoredDatabase&) = delete;
    AnchoredDatabase& operator=(const AnchoredDatabase&) = delete;

    [[nodiscard]] PersistenceDetail::WinsqliteConnection& connection() noexcept
    {
        return connection_.value();
    }

    [[nodiscard]] PersistenceDetail::DatabaseNamespaceLease& namespaceLease() noexcept
    {
        return *namespaceLease_;
    }

    [[nodiscard]] PersistenceDetail::AnchoredSqliteVfs& vfs() noexcept
    {
        return *vfs_;
    }

    void requireExactOpenOwnerCounts() const
    {
        require(vfs_->openFileCount() > 0U &&
                    vfs_->openFileCount() == namespaceLease_->openVfsFileCount(),
                "the VFS and namespace did not report identical open-file ownership");
    }

    void close(const Domain::OperationContext& context)
    {
        require(connection_.has_value(),
                "the concurrency database connection was already closed");
        take(connection_->close(context));
        connection_.reset();
        require(vfs_->openFileCount() == 0U &&
                    namespaceLease_->openVfsFileCount() == 0U,
                "an explicitly closed database retained a VFS or namespace owner");
        take(vfs_->close());
        require(!vfs_->isRegistered(),
                "an explicitly closed database retained its VFS registration");
    }

private:
    AnchoredDatabase(
        std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease,
        std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs,
        PersistenceDetail::WinsqliteConnection connection) noexcept
        : namespaceLease_{std::move(namespaceLease)},
          vfs_{std::move(vfs)},
          connection_{std::move(connection)}
    {
    }

    std::shared_ptr<PersistenceDetail::DatabaseNamespaceLease> namespaceLease_;
    std::unique_ptr<PersistenceDetail::AnchoredSqliteVfs> vfs_;
    std::optional<PersistenceDetail::WinsqliteConnection> connection_;
};

[[nodiscard]] std::int64_t queryInteger(
    PersistenceDetail::WinsqliteConnection& connection,
    const std::string_view sql,
    const Domain::OperationContext& context)
{
    auto statement = take(connection.prepare(sql, context));
    require(take(statement.step()) == PersistenceDetail::WinsqliteStepResult::Row,
            "the concurrency query returned no row");
    const std::int64_t value = take(statement.columnInt64(0));
    require(take(statement.step()) == PersistenceDetail::WinsqliteStepResult::Done,
            "the concurrency query returned multiple rows");
    return value;
}

[[nodiscard]] std::vector<std::wstring> lockArguments(
    const std::filesystem::path& directory,
    const NamedEvent& ready,
    const NamedEvent& release)
{
    return {
        L"--hold-lock",
        directory.native(),
        L"lock.sqlite",
        L"lock.sqlite.migration.lock",
        ready.name,
        release.name,
        std::to_wstring(ChildWaitMilliseconds)};
}

void testCrossProcessMigrationLockCancellationAndCleanup(
    const std::filesystem::path& fixturePath)
{
    const auto processFixture = requireProcessFixture(fixturePath);
    ScopedTestDirectory directory{L"concurrency-migration-lock"};
    const auto canonicalDirectory = canonicalPath(
        directory.path(), "the lock test directory could not be canonicalized");
    auto namespaceLease = makeNamespace(
        canonicalDirectory, L"lock.sqlite", L"lock.sqlite.migration.lock");
    auto ready = createNamedEvent(L"migration-ready");
    auto release = createNamedEvent(L"migration-release");
    const auto arguments = lockArguments(canonicalDirectory, ready, release);
    auto child = ChildProcess::launch(processFixture, arguments);
    waitForReadyOrChild(
        ready, child,
        "the migration-lock fixture exited before acquiring the lock",
        "the migration-lock fixture did not become ready within 15 seconds");

    const auto deadlineStarted = std::chrono::steady_clock::now();
    const auto deadline = namespaceLease->acquireMigrationLock(
        concurrencyContext("p07-lock-deadline", 75ms));
    const auto deadlineElapsed = std::chrono::steady_clock::now() - deadlineStarted;
    requireError(deadline, Domain::ErrorCodes::DeadlineExceeded,
                 "a contended migration lock ignored its deadline");
    require(deadlineElapsed >= 50ms && deadlineElapsed < 1s,
            "a contended migration lock did not honor the bounded 75 ms deadline window");

    std::stop_source cancellation;
    std::jthread canceller{[&cancellation] {
        std::this_thread::sleep_for(50ms);
        cancellation.request_stop();
    }};
    const auto cancellationStarted = std::chrono::steady_clock::now();
    const auto cancelled = namespaceLease->acquireMigrationLock(
        concurrencyContext("p07-lock-cancel", 5s, cancellation.get_token()));
    const auto cancellationElapsed =
        std::chrono::steady_clock::now() - cancellationStarted;
    canceller.join();
    requireError(cancelled, Domain::ErrorCodes::Cancelled,
                 "a contended migration lock ignored cancellation");
    require(cancellationElapsed >= 25ms && cancellationElapsed < 1s,
            "a contended migration lock did not honor bounded cancellation wakeup");

    const auto capStarted = std::chrono::steady_clock::now();
    const auto capped = namespaceLease->acquireMigrationLock(
        concurrencyContext("p07-lock-three-second-cap", 10s));
    const auto capElapsed = std::chrono::steady_clock::now() - capStarted;
    requireError(capped, Domain::ErrorCodes::DatabaseBusy,
                 "a contended migration lock exceeded its three-second admission cap");
    require(capElapsed >= 2500ms && capElapsed < 4500ms,
            "migration-lock admission did not honor its three-second cap");

    static_cast<void>(namespaceLease->acquireMigrationLock(
        concurrencyContext("p07-lock-handle-warmup", 20ms)));
    DWORD handlesBefore{};
    require(::GetProcessHandleCount(::GetCurrentProcess(), &handlesBefore) != FALSE,
            "GetProcessHandleCount failed before repeated lock deadlines");
    for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
        const auto bounded = namespaceLease->acquireMigrationLock(
            concurrencyContext("p07-lock-handle-bound", 20ms));
        requireError(bounded, Domain::ErrorCodes::DeadlineExceeded,
                     "a repeated lock deadline unexpectedly acquired the held lock");
    }
    DWORD handlesAfter{};
    require(::GetProcessHandleCount(::GetCurrentProcess(), &handlesAfter) != FALSE,
            "GetProcessHandleCount failed after repeated lock deadlines");
    require(handlesAfter == handlesBefore,
            "a cancelled or deadline-bounded migration lock retained a native handle");

    signalEvent(release, "the migration-lock fixture could not be released");
    require(child.wait(ChildWaitMilliseconds) == 0U,
            "the migration-lock fixture did not shut down cleanly");
    {
        auto recovered = take(namespaceLease->acquireMigrationLock(
            concurrencyContext("p07-lock-recovered", 2s)));
        require(static_cast<bool>(recovered),
                "migration-lock ownership did not recover after process shutdown");
    }

    const std::array stages{namespaceLease};
    const std::size_t cleaned = take(PersistenceDetail::DatabaseNamespaceLease::
        cleanupClosedStages(std::span{stages},
                            concurrencyContext("p07-lock-cleanup", 2s)));
    require(cleaned == 1U,
            "bounded stale-stage cleanup did not report the exact lock-bearing stage");
    require(!std::filesystem::exists(namespaceLease->canonicalPath(
                PersistenceDetail::DatabaseLeafRole::MigrationLock)),
            "bounded stale-stage cleanup retained the exact migration-lock leaf");
    require(namespaceLease->openVfsFileCount() == 0U,
            "the migration-lock test retained a namespace file owner");
}

void testMigrationLockWaiterRotatesAfterProducerUnlinks()
{
    ScopedTestDirectory directory{L"concurrency-lock-waiter-rotation"};
    auto namespaceLease = makeNamespace(
        directory.path(), L"rotation.sqlite", L"rotation.sqlite.lock");
    DWORD handlesBeforeWaiter{};
    require(::GetProcessHandleCount(
                ::GetCurrentProcess(), &handlesBeforeWaiter) != FALSE,
            "GetProcessHandleCount failed before the lock waiter started");
    auto producer = take(namespaceLease->acquireMigrationLock(
        concurrencyContext("p07-lock-rotation-producer", 8s)));

    std::atomic<int> waiterStatus{};
    std::atomic<bool> startAcquire{};
    std::atomic<bool> releaseWaiter{};
    std::jthread waiter{[&] {
        auto observed = namespaceLease->openLeaf(
            PersistenceDetail::DatabaseLeafRole::MigrationLock,
            PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
            FILE_READ_ATTRIBUTES);
        if (!observed) {
            waiterStatus.store(-1, std::memory_order_release);
            return;
        }
        auto observer = std::move(observed).value();
        static_cast<void>(observer.nativeHandle());
        waiterStatus.store(1, std::memory_order_release);

        const auto startDeadline = std::chrono::steady_clock::now() + 5s;
        while (!startAcquire.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < startDeadline) {
            std::this_thread::yield();
        }
        if (!startAcquire.load(std::memory_order_acquire)) {
            waiterStatus.store(-2, std::memory_order_release);
            return;
        }

        auto admitted = namespaceLease->acquireMigrationLock(
            concurrencyContext("p07-lock-rotation-waiter", 8s));
        if (!admitted) {
            waiterStatus.store(-3, std::memory_order_release);
            return;
        }
        auto waiterLock = std::move(admitted).value();
        waiterStatus.store(2, std::memory_order_release);

        const auto releaseDeadline = std::chrono::steady_clock::now() + 5s;
        while (!releaseWaiter.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < releaseDeadline) {
            std::this_thread::yield();
        }
        if (!releaseWaiter.load(std::memory_order_acquire)) {
            waiterStatus.store(-4, std::memory_order_release);
            return;
        }
        const auto cleaned = namespaceLease->cleanupClosedStageWithLock(
            waiterLock, concurrencyContext("p07-lock-rotation-cleanup", 5s));
        waiterStatus.store(
            cleaned ? 3 : -5,
            std::memory_order_release);
    }};

    const auto waitUntil = [](const auto& predicate,
                              const std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            ::Sleep(1U);
        }
        return predicate();
    };

    const bool observerReady = waitUntil(
        [&] { return waiterStatus.load(std::memory_order_acquire) != 0; }, 5s);
    require(observerReady && waiterStatus.load(std::memory_order_acquire) == 1,
            "the migration-lock waiter could not pin the producer's exact lock leaf");

    DWORD blockedBaseline{};
    require(::GetProcessHandleCount(::GetCurrentProcess(), &blockedBaseline) != FALSE,
            "GetProcessHandleCount failed before lock-wait admission");
    startAcquire.store(true, std::memory_order_release);
    const bool waiterBlockedOnExactLeaf = waitUntil(
        [&] {
            DWORD current{};
            return ::GetProcessHandleCount(::GetCurrentProcess(), &current) != FALSE &&
                   current >= blockedBaseline + 2U;
        },
        5s);
    require(waiterBlockedOnExactLeaf,
            "the migration-lock waiter did not reach its bounded immediate-retry loop");

    static_cast<void>(take(namespaceLease->cleanupClosedStageWithLock(
        producer, concurrencyContext("p07-lock-rotation-unlink", 5s))));
    const bool waiterRotated = waitUntil(
        [&] { return waiterStatus.load(std::memory_order_acquire) != 1; }, 5s);
    require(waiterRotated && waiterStatus.load(std::memory_order_acquire) == 2,
            "a waiter admitted on an unlinked lock predecessor did not rotate to the current pathname");
    require(std::filesystem::is_regular_file(namespaceLease->canonicalPath(
                PersistenceDetail::DatabaseLeafRole::MigrationLock)),
            "the rotated migration-lock waiter did not own a live exact pathname");

    releaseWaiter.store(true, std::memory_order_release);
    waiter.join();
    require(waiterStatus.load(std::memory_order_acquire) == 3,
            "the rotated migration-lock waiter could not remove its exact lock leaf");
    require(!std::filesystem::exists(namespaceLease->canonicalPath(
                PersistenceDetail::DatabaseLeafRole::MigrationLock)),
            "migration-lock waiter rotation retained an orphan lock pathname");

    DWORD handlesAfterWaiter{};
    require(::GetProcessHandleCount(
                ::GetCurrentProcess(), &handlesAfterWaiter) != FALSE,
            "GetProcessHandleCount failed after the lock waiter completed");
    require(handlesAfterWaiter == handlesBeforeWaiter,
            "migration-lock waiter rotation retained a native handle");
}

void testSimultaneousInitializers(const std::filesystem::path& fixturePath)
{
    const auto processFixture = requireProcessFixture(fixturePath);
    ScopedTestDirectory directory{L"concurrency-initializers"};
    const auto canonicalDirectory = canonicalPath(
        directory.path(), "the initializer test directory could not be canonicalized");
    auto start = createNamedEvent(L"initializers-start");
    const std::wstring wait = std::to_wstring(ChildWaitMilliseconds);
    const std::vector<std::wstring> firstArguments{
        L"--initialize", canonicalDirectory.native(), L"initializers.sqlite",
        L"initializers.sqlite.migration.lock", start.name, L"1", wait};
    const std::vector<std::wstring> secondArguments{
        L"--initialize", canonicalDirectory.native(), L"initializers.sqlite",
        L"initializers.sqlite.migration.lock", start.name, L"2", wait};
    auto first = ChildProcess::launch(processFixture, firstArguments);
    auto second = ChildProcess::launch(processFixture, secondArguments);
    signalEvent(start, "the simultaneous initializer start event could not be signalled");

    const std::array<HANDLE, 2U> children{first.nativeHandle(), second.nativeHandle()};
    const DWORD waitResult = ::WaitForMultipleObjects(
        static_cast<DWORD>(children.size()), children.data(), TRUE,
        ChildWaitMilliseconds);
    require(waitResult == WAIT_OBJECT_0,
            waitResult == WAIT_TIMEOUT
                ? "simultaneous database initializers exceeded the 15-second bound"
                : "waiting for simultaneous database initializers failed");
    require(first.exitCode() == 0U && second.exitCode() == 0U,
            "a serialized database initializer did not exit successfully");

    auto namespaceLease = makeNamespace(
        canonicalDirectory, L"initializers.sqlite",
        L"initializers.sqlite.migration.lock");
    const auto context = concurrencyContext("p07-initializers-verify");
    auto database = AnchoredDatabase::open(
        namespaceLease, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
        context);
    database->requireExactOpenOwnerCounts();
    require(queryInteger(
                database->connection(),
                "SELECT COUNT(*) FROM process_initializers;", context) == 2 &&
                queryInteger(
                    database->connection(),
                    "SELECT SUM(initializer_id) FROM process_initializers;", context) == 3,
            "simultaneous initializers did not serialize both exact writes");
    database->close(context);
    require(namespaceLease->openVfsFileCount() == 0U,
            "initializer verification retained a namespace file owner");
}

void testCrossProcessWalVisibilityAndShutdown(
    const std::filesystem::path& fixturePath)
{
    const auto processFixture = requireProcessFixture(fixturePath);
    ScopedTestDirectory directory{L"concurrency-wal"};
    const auto canonicalDirectory = canonicalPath(
        directory.path(), "the WAL test directory could not be canonicalized");
    auto ready = createNamedEvent(L"wal-ready");
    auto release = createNamedEvent(L"wal-release");
    const std::vector<std::wstring> arguments{
        L"--wal-writer", canonicalDirectory.native(), L"wal.sqlite",
        L"wal.sqlite.migration.lock", ready.name, release.name, L"2718",
        std::to_wstring(ChildWaitMilliseconds)};
    auto child = ChildProcess::launch(processFixture, arguments);
    waitForReadyOrChild(
        ready, child,
        "the WAL writer exited before publishing its committed marker",
        "the WAL writer did not become ready within 15 seconds");

    auto namespaceLease = makeNamespace(
        canonicalDirectory, L"wal.sqlite", L"wal.sqlite.migration.lock");
    const std::wstring mainPath = namespaceLease->canonicalMainDatabasePath();
    const std::wstring walPath = namespaceLease->canonicalPath(
        PersistenceDetail::DatabaseLeafRole::Wal);
    const std::wstring shmPath = namespaceLease->canonicalPath(
        PersistenceDetail::DatabaseLeafRole::SharedMemory);
    std::error_code sizeError;
    const auto walSize = std::filesystem::file_size(walPath, sizeError);
    require(!sizeError && walSize > 32U,
            "the live writer did not retain a non-empty WAL sidecar");

    const auto context = concurrencyContext("p07-wal-reader");
    auto reader = AnchoredDatabase::open(
        namespaceLease, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
        context);
    reader->requireExactOpenOwnerCounts();
    require(queryInteger(
                reader->connection(),
                "SELECT COUNT(*) FROM process_wal_probe WHERE marker=2718;", context) == 1,
            "a second process could not observe the committed WAL marker");
    reader->close(context);
    require(namespaceLease->openVfsFileCount() == 0U,
            "the WAL reader retained a namespace file owner after close");
    reader.reset();
    const auto mainIdentity = take(namespaceLease->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Main,
        PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
        FILE_READ_ATTRIBUTES)).identity();
    const auto walIdentity = take(namespaceLease->openLeaf(
        PersistenceDetail::DatabaseLeafRole::Wal,
        PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
        FILE_READ_ATTRIBUTES)).identity();
    const auto shmIdentity = take(namespaceLease->openLeaf(
        PersistenceDetail::DatabaseLeafRole::SharedMemory,
        PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
        FILE_READ_ATTRIBUTES)).identity();
    const std::string mainBytes = PersistenceSupport::readFixture(mainPath);
    const std::string walBytes = PersistenceSupport::readFixture(walPath);
    namespaceLease.reset();

    signalEvent(release, "the WAL writer release event could not be signalled");
    require(child.wait(ChildWaitMilliseconds) == 0U,
            "the WAL writer did not shut down cleanly without checkpointing");
    require(std::filesystem::exists(walPath) && std::filesystem::exists(shmPath),
            "bounded shutdown did not retain the persistent WAL/SHM cohort");
    sizeError.clear();
    require(std::filesystem::file_size(walPath, sizeError) > 32U && !sizeError,
            "bounded shutdown truncated the committed WAL");
    require(PersistenceSupport::readFixture(mainPath) == mainBytes &&
                PersistenceSupport::readFixture(walPath) == walBytes,
            "bounded shutdown checkpointed Main or changed the committed WAL");

    auto reopenedNamespace = makeNamespace(
        canonicalDirectory, L"wal.sqlite", L"wal.sqlite.migration.lock");
    require(take(reopenedNamespace->openLeaf(
                     PersistenceDetail::DatabaseLeafRole::Main,
                     PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
                     FILE_READ_ATTRIBUTES)).identity() == mainIdentity &&
                take(reopenedNamespace->openLeaf(
                     PersistenceDetail::DatabaseLeafRole::Wal,
                     PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
                     FILE_READ_ATTRIBUTES)).identity() == walIdentity &&
                take(reopenedNamespace->openLeaf(
                     PersistenceDetail::DatabaseLeafRole::SharedMemory,
                     PersistenceDetail::DatabaseLeafDisposition::OpenExisting,
                     FILE_READ_ATTRIBUTES)).identity() == shmIdentity,
            "shutdown changed the persistent Main/WAL/SHM file identities");
    auto reopened = AnchoredDatabase::open(
        reopenedNamespace, PersistenceDetail::WinsqliteOpenMode::ReadOnlyExisting,
        concurrencyContext("p07-wal-reopen"));
    reopened->requireExactOpenOwnerCounts();
    require(queryInteger(
                reopened->connection(),
                "SELECT COUNT(*) FROM process_wal_probe WHERE marker=2718;",
                concurrencyContext("p07-wal-reopen-query")) == 1,
            "the committed WAL marker was not durable after writer shutdown");
    reopened->close(concurrencyContext("p07-wal-reopen-close"));
    require(reopenedNamespace->openVfsFileCount() == 0U,
            "the reopened WAL namespace retained a file owner");
}

void testCrossProcessBusyWindowAndOwnerRelease(
    const std::filesystem::path& fixturePath)
{
    const auto processFixture = requireProcessFixture(fixturePath);
    ScopedTestDirectory directory{L"concurrency-busy"};
    const auto canonicalDirectory = canonicalPath(
        directory.path(), "the busy test directory could not be canonicalized");
    auto namespaceLease = makeNamespace(
        canonicalDirectory, L"busy.sqlite", L"busy.sqlite.migration.lock");
    const auto setupContext = concurrencyContext("p07-busy-setup");
    {
        auto setup = AnchoredDatabase::open(
            namespaceLease, PersistenceDetail::WinsqliteOpenMode::ReadWriteCreate,
            setupContext);
        take(setup->connection().execute(
            "CREATE TABLE busy_probe(value INTEGER NOT NULL);", setupContext));
        setup->close(setupContext);
    }

    auto ready = createNamedEvent(L"busy-ready");
    auto release = createNamedEvent(L"busy-release");
    const std::vector<std::wstring> arguments{
        L"--hold-write-lock", canonicalDirectory.native(), L"busy.sqlite",
        L"busy.sqlite.migration.lock", ready.name, release.name,
        std::to_wstring(ChildWaitMilliseconds)};
    auto child = ChildProcess::launch(processFixture, arguments);
    waitForReadyOrChild(
        ready, child,
        "the write-lock fixture exited before retaining its transaction",
        "the write-lock fixture did not become ready within 15 seconds");

    const auto contenderContext = concurrencyContext("p07-busy-contender", 10s);
    auto contender = AnchoredDatabase::open(
        namespaceLease, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
        contenderContext);
    contender->requireExactOpenOwnerCounts();
    const auto started = std::chrono::steady_clock::now();
    const auto busy = PersistenceDetail::WinsqliteTransaction::beginImmediate(
        contender->connection(), contenderContext);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    requireError(busy, Domain::ErrorCodes::DatabaseBusy,
                 "a cross-process write lock was not mapped to database_busy");
    require(elapsed >= 2500ms && elapsed < 4500ms,
            "cross-process busy handling did not honor the three-second cap");
    contender->close(concurrencyContext("p07-busy-close"));
    require(namespaceLease->openVfsFileCount() == 0U,
            "the busy contender retained a namespace file owner");
    contender.reset();
    namespaceLease.reset();

    signalEvent(release, "the write-lock fixture release event could not be signalled");
    require(child.wait(ChildWaitMilliseconds) == 0U,
            "the write-lock fixture did not roll back and shut down cleanly");

    auto recoveredNamespace = makeNamespace(
        canonicalDirectory, L"busy.sqlite", L"busy.sqlite.migration.lock");
    auto recovered = AnchoredDatabase::open(
        recoveredNamespace, PersistenceDetail::WinsqliteOpenMode::ReadWriteExisting,
        concurrencyContext("p07-busy-reopen"));
    recovered->requireExactOpenOwnerCounts();
    {
        auto transaction = take(PersistenceDetail::WinsqliteTransaction::beginImmediate(
            recovered->connection(), concurrencyContext("p07-busy-recovered")));
        take(transaction.rollback());
    }
    recovered->close(concurrencyContext("p07-busy-recovered-close"));
    require(recoveredNamespace->openVfsFileCount() == 0U,
            "recovered write-lock ownership retained a namespace owner");
}

} // namespace

void registerDatabaseConcurrencyTests(
    TestRegistry& tests,
    const std::filesystem::path& processFixture)
{
    addTest(tests, "persistence.concurrency.migration-lock",
            [processFixture] {
                testCrossProcessMigrationLockCancellationAndCleanup(processFixture);
            });
    addTest(tests, "persistence.concurrency.migration-lock-waiter-rotation",
            testMigrationLockWaiterRotatesAfterProducerUnlinks);
    addTest(tests, "persistence.concurrency.simultaneous-initializers",
            [processFixture] { testSimultaneousInitializers(processFixture); });
    addTest(tests, "persistence.concurrency.wal-visibility",
            [processFixture] { testCrossProcessWalVisibilityAndShutdown(processFixture); });
    addTest(tests, "persistence.concurrency.busy-window",
            [processFixture] { testCrossProcessBusyWindowAndOwnerRelease(processFixture); });
}

} // namespace ForgeConductor::Tests
