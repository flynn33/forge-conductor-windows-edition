#include "Infrastructure/TestSupport.h"

#include "ForgeConductor/Contracts/IManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Contracts::ManagerAuthenticationSecret;
using Infrastructure::Windows::WindowsManagerAuthenticationTokenGenerator;
using Infrastructure::Windows::WindowsManagerAuthenticationTokenStore;

static_assert(std::is_final_v<ManagerAuthenticationSecret>);
static_assert(!std::is_copy_constructible_v<ManagerAuthenticationSecret>);
static_assert(!std::is_copy_assignable_v<ManagerAuthenticationSecret>);
static_assert(std::is_nothrow_move_constructible_v<ManagerAuthenticationSecret>);
static_assert(std::is_nothrow_move_assignable_v<ManagerAuthenticationSecret>);
static_assert(std::is_final_v<WindowsManagerAuthenticationTokenGenerator>);
static_assert(std::is_final_v<WindowsManagerAuthenticationTokenStore>);
static_assert(!std::is_copy_constructible_v<WindowsManagerAuthenticationTokenStore>);
static_assert(!std::is_copy_assignable_v<WindowsManagerAuthenticationTokenStore>);

[[nodiscard]] ManagerAuthenticationSecret::Bytes sequentialBytes() noexcept
{
    ManagerAuthenticationSecret::Bytes bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    return bytes;
}

[[nodiscard]] Domain::Error scriptedError(const std::string_view code)
{
    return Domain::makeError(code, "Scripted manager authentication boundary failure.");
}

class ScriptedGenerator final : public Contracts::IManagerAuthenticationTokenGenerator {
public:
    explicit ScriptedGenerator(ManagerAuthenticationSecret::Bytes bytes) noexcept
        : bytes_{bytes}
    {
    }

    [[nodiscard]] Domain::Result<ManagerAuthenticationSecret> next() noexcept override
    {
        try {
            ++calls_;
            if (error_.has_value()) {
                return Domain::Result<ManagerAuthenticationSecret>::failure(*error_);
            }
            return Domain::Result<ManagerAuthenticationSecret>::success(
                ManagerAuthenticationSecret{bytes_});
        } catch (...) {
            return Domain::Result<ManagerAuthenticationSecret>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "Scripted token generation failed."));
        }
    }

    void failWith(Domain::Error error) { error_ = std::move(error); }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

private:
    ManagerAuthenticationSecret::Bytes bytes_{};
    std::optional<Domain::Error> error_;
    std::size_t calls_{};
};

class StatefulSecureStorage final : public Contracts::ISecureStorage {
public:
    [[nodiscard]] Domain::Result<void> put(
        const std::string_view key,
        const std::span<const std::byte> secret,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++putCalls_;
            lastPutKey_ = key;
            lastPutContext_ = context;
            capturedPut_.assign(secret.begin(), secret.end());
            if (putError_.has_value()) {
                return Domain::Result<void>::failure(*putError_);
            }
            stored_.emplace(secret.begin(), secret.end());
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "Scripted secure-storage put failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::optional<std::vector<std::byte>>> get(
        const std::string_view key,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::function<void()> afterGet;
            std::optional<Domain::Error> error;
            std::optional<std::vector<std::byte>> stored;
            {
                std::unique_lock lock{mutex_};
                ++getCalls_;
                lastGetKey_ = key;
                lastMaximumBytes_ = maximumBytes;
                lastGetContext_ = context;
                getEntered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this]() noexcept { return !blockGet_ || releaseGet_; });
                afterGet = afterGet_;
                error = getError_;
                stored = stored_;
            }
            if (afterGet) {
                afterGet();
            }
            if (error.has_value()) {
                return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                    std::move(error).value());
            }
            return Domain::Result<std::optional<std::vector<std::byte>>>::success(
                std::move(stored));
        } catch (...) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "Scripted secure-storage get failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> remove(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Manager authentication must not remove its durable token."));
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        ++shutdownCalls_;
    }

    void setStored(std::optional<std::vector<std::byte>> stored)
    {
        std::lock_guard lock{mutex_};
        stored_ = std::move(stored);
    }

    void failGetWith(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        getError_ = std::move(error);
    }

    void failPutWith(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        putError_ = std::move(error);
    }

    void runAfterGet(std::function<void()> action)
    {
        std::lock_guard lock{mutex_};
        afterGet_ = std::move(action);
    }

    void blockGet()
    {
        std::lock_guard lock{mutex_};
        blockGet_ = true;
        releaseGet_ = false;
        getEntered_ = false;
    }

    [[nodiscard]] bool waitForBlockedGet(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this]() noexcept { return getEntered_; });
    }

    void releaseBlockedGet()
    {
        std::lock_guard lock{mutex_};
        releaseGet_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] std::size_t getCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return getCalls_;
    }

    [[nodiscard]] std::size_t putCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return putCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

    [[nodiscard]] std::string lastGetKey() const
    {
        std::lock_guard lock{mutex_};
        return lastGetKey_;
    }

    [[nodiscard]] std::string lastPutKey() const
    {
        std::lock_guard lock{mutex_};
        return lastPutKey_;
    }

    [[nodiscard]] std::size_t lastMaximumBytes() const noexcept
    {
        std::lock_guard lock{mutex_};
        return lastMaximumBytes_;
    }

    [[nodiscard]] std::vector<std::byte> capturedPut() const
    {
        std::lock_guard lock{mutex_};
        return capturedPut_;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> stored() const
    {
        std::lock_guard lock{mutex_};
        return stored_;
    }

    [[nodiscard]] std::optional<Domain::OperationContext> lastGetContext() const
    {
        std::lock_guard lock{mutex_};
        return lastGetContext_;
    }

    [[nodiscard]] std::optional<Domain::OperationContext> lastPutContext() const
    {
        std::lock_guard lock{mutex_};
        return lastPutContext_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<std::vector<std::byte>> stored_;
    std::optional<Domain::Error> getError_;
    std::optional<Domain::Error> putError_;
    std::function<void()> afterGet_;
    std::string lastGetKey_;
    std::string lastPutKey_;
    std::vector<std::byte> capturedPut_;
    std::optional<Domain::OperationContext> lastGetContext_;
    std::optional<Domain::OperationContext> lastPutContext_;
    std::size_t lastMaximumBytes_{};
    std::size_t getCalls_{};
    std::size_t putCalls_{};
    std::size_t shutdownCalls_{};
    bool blockGet_{};
    bool releaseGet_{};
    bool getEntered_{};
};

[[nodiscard]] std::vector<std::byte> vectorFrom(
    const ManagerAuthenticationSecret::Bytes& bytes)
{
    return {bytes.begin(), bytes.end()};
}

void exactLowercaseHexEncodingPreservesLeadingZeros()
{
    const auto bytes = sequentialBytes();
    const ManagerAuthenticationSecret secret{bytes};
    const auto token = take(Contracts::encodeManagerAuthenticationToken(secret));

    require(token.value() ==
                "000102030405060708090a0b0c0d0e0f"
                "101112131415161718191a1b1c1d1e1f",
            "manager authentication did not encode the exact raw bytes as lowercase hex");
    require(token.value().size() == 64U,
            "manager authentication did not produce exactly 64 wire characters");
}

void missingTokenIsCreatedPersistedAndReloaded()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    const auto expectedBytes = sequentialBytes();
    ScriptedGenerator generator{expectedBytes};
    WindowsManagerAuthenticationTokenStore store{storage, generator};

    const auto missing = take(store.load(fixture.active()));
    require(!missing.has_value(), "a missing manager authentication token appeared present");
    require(storage.getCalls() == 1U && storage.putCalls() == 0U,
            "a read-only missing-token lookup mutated secure storage");

    const auto created = take(store.loadOrCreate(fixture.active()));
    require(created.value() ==
                "000102030405060708090a0b0c0d0e0f"
                "101112131415161718191a1b1c1d1e1f",
            "loadOrCreate returned a token other than the generated raw bytes");
    require(generator.calls() == 1U && storage.putCalls() == 1U,
            "loadOrCreate did not generate and persist exactly once");
    require(storage.lastGetKey() == WindowsManagerAuthenticationTokenStore::StorageKey &&
                storage.lastPutKey() == WindowsManagerAuthenticationTokenStore::StorageKey,
            "manager authentication used an unexpected secure-storage key");
    require(storage.lastMaximumBytes() == ManagerAuthenticationSecret::SizeBytes,
            "manager authentication requested an unbounded secure-storage value");
    require(std::ranges::equal(storage.capturedPut(), expectedBytes),
            "manager authentication did not persist the exact generated 32 bytes");

    const auto reloaded = take(store.loadOrCreate(fixture.active()));
    require(reloaded == created,
            "manager authentication did not reload the persisted wire token");
    require(generator.calls() == 1U && storage.putCalls() == 1U,
            "loadOrCreate regenerated a token that was already present");
}

void invalidStoredLengthsAreRejected()
{
    TestContext fixture;
    for (const std::size_t length : {31U, 33U}) {
        StatefulSecureStorage storage;
        storage.setStored(std::vector<std::byte>(length, std::byte{0x5a}));
        ScriptedGenerator generator{sequentialBytes()};
        WindowsManagerAuthenticationTokenStore store{storage, generator};

        requireError(store.load(fixture.active()), Domain::ErrorCodes::IntegrityFailure,
                     "manager authentication accepted a stored nonce with invalid length");
        require(generator.calls() == 0U && storage.putCalls() == 0U,
                "invalid stored authentication material triggered token creation");
    }
}

void boundaryFailuresAreReturnedWithoutMutation()
{
    TestContext fixture;
    {
        StatefulSecureStorage storage;
        storage.failGetWith(scriptedError(Domain::ErrorCodes::StorageFull));
        ScriptedGenerator generator{sequentialBytes()};
        WindowsManagerAuthenticationTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()), Domain::ErrorCodes::StorageFull,
                     "manager authentication replaced a secure-storage get failure");
        require(generator.calls() == 0U && storage.putCalls() == 0U,
                "a secure-storage read failure triggered authentication mutation");
    }
    {
        StatefulSecureStorage storage;
        ScriptedGenerator generator{sequentialBytes()};
        generator.failWith(scriptedError(Domain::ErrorCodes::InternalFailure));
        WindowsManagerAuthenticationTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::InternalFailure,
                     "manager authentication replaced a generator failure");
        require(storage.putCalls() == 0U,
                "a failed authentication generator wrote secure storage");
    }
    {
        StatefulSecureStorage storage;
        storage.failPutWith(scriptedError(Domain::ErrorCodes::StorageFull));
        ScriptedGenerator generator{sequentialBytes()};
        WindowsManagerAuthenticationTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()), Domain::ErrorCodes::StorageFull,
                     "manager authentication replaced a secure-storage put failure");
        require(!storage.stored().has_value(),
                "a failed authentication persistence call committed a token");
    }
}

void operationContextIsForwardedAndRevalidated()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    ScriptedGenerator generator{sequentialBytes()};
    WindowsManagerAuthenticationTokenStore store{storage, generator};
    const auto active = fixture.active();

    const auto loaded = take(store.load(active));
    require(!loaded.has_value(),
            "the context-forwarding fixture unexpectedly loaded a token");
    const auto captured = storage.lastGetContext();
    require(captured.has_value() && captured->operationId == active.operationId &&
                captured->correlationId == active.correlationId &&
                captured->deadline == active.deadline &&
                captured->cancellation == active.cancellation,
            "manager authentication did not forward the exact operation context");

    std::stop_source cancelled;
    cancelled.request_stop();
    const Domain::OperationContext cancelledContext{
        active.operationId, active.deadline, cancelled.get_token(), active.correlationId};
    const auto callsBeforeCancelled = storage.getCalls();
    requireError(store.load(cancelledContext), Domain::ErrorCodes::Cancelled,
                 "manager authentication accepted a pre-cancelled operation");
    require(storage.getCalls() == callsBeforeCancelled,
            "a pre-cancelled authentication operation reached secure storage");

    const auto callsBeforeExpired = storage.getCalls();
    requireError(store.load(fixture.expired()), Domain::ErrorCodes::DeadlineExceeded,
                 "manager authentication accepted an expired operation");
    require(storage.getCalls() == callsBeforeExpired,
            "an expired authentication operation reached secure storage");

    std::stop_source cancelAfterRead;
    const Domain::OperationContext cancelDuringCreate{
        active.operationId, active.deadline, cancelAfterRead.get_token(), active.correlationId};
    storage.runAfterGet([&cancelAfterRead]() noexcept { cancelAfterRead.request_stop(); });
    requireError(store.loadOrCreate(cancelDuringCreate), Domain::ErrorCodes::Cancelled,
                 "manager authentication did not revalidate cancellation after storage read");
    require(generator.calls() == 0U && storage.putCalls() == 0U,
            "cancellation after storage read allowed token generation or persistence");
}

void admissionCapacityIsExactlyOne()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    storage.blockGet();
    ScriptedGenerator generator{sequentialBytes()};
    WindowsManagerAuthenticationTokenStore store{storage, generator};
    std::optional<Domain::Result<std::optional<Domain::Sha256Digest>>> firstResult;

    std::jthread first{[&]() { firstResult.emplace(store.load(fixture.active())); }};
    const bool entered = storage.waitForBlockedGet(std::chrono::seconds{2});
    if (!entered) {
        storage.releaseBlockedGet();
        first.join();
        require(false, "the first authentication operation did not enter secure storage");
    }

    requireError(store.load(fixture.active()), Domain::ErrorCodes::LimitExceeded,
                 "manager authentication admitted a queued concurrent operation");
    require(storage.getCalls() == 1U,
            "the rejected concurrent authentication operation reached secure storage");

    storage.releaseBlockedGet();
    first.join();
    require(firstResult.has_value() && firstResult.value() &&
                !firstResult.value().value().has_value(),
            "the admitted authentication operation did not complete normally");
}

void shutdownIsIdempotentAndRejectsNewCalls()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    ScriptedGenerator generator{sequentialBytes()};
    WindowsManagerAuthenticationTokenStore store{storage, generator};

    store.shutdown();
    store.shutdown();
    requireError(store.load(fixture.active()), Domain::ErrorCodes::TransportClosed,
                 "manager authentication load succeeded after shutdown");
    requireError(store.loadOrCreate(fixture.active()), Domain::ErrorCodes::TransportClosed,
                 "manager authentication creation succeeded after shutdown");
    require(storage.getCalls() == 0U && storage.putCalls() == 0U &&
                generator.calls() == 0U,
            "manager authentication reached dependencies after shutdown");
    require(storage.shutdownCalls() == 0U,
            "the non-owning authentication store shut down its injected dependency");
}

void constantTimeComparisonCoversEveryMismatchPosition()
{
    const auto baseline = parse<Domain::Sha256Digest>(std::string(64U, 'a'));
    require(Contracts::constantTimeManagerAuthenticationTokenEquals(baseline, baseline),
            "constant-time authentication comparison rejected equal tokens");

    for (std::size_t index = 0U; index < 64U; ++index) {
        std::string changed(64U, 'a');
        changed[index] = 'b';
        const auto mismatch = parse<Domain::Sha256Digest>(changed);
        require(!Contracts::constantTimeManagerAuthenticationTokenEquals(
                    baseline, mismatch),
                "constant-time authentication comparison missed a mismatch position");
        require(!Contracts::constantTimeManagerAuthenticationTokenEquals(
                    mismatch, baseline),
                "constant-time authentication comparison was not symmetric");
    }
}

} // namespace

void registerWindowsManagerAuthenticationTests(TestRegistry& tests)
{
    addTest(tests, "manager_authentication.exact_lowercase_hex_encoding",
            exactLowercaseHexEncodingPreservesLeadingZeros);
    addTest(tests, "manager_authentication.missing_create_persist_reload",
            missingTokenIsCreatedPersistedAndReloaded);
    addTest(tests, "manager_authentication.invalid_stored_lengths",
            invalidStoredLengthsAreRejected);
    addTest(tests, "manager_authentication.boundary_failures",
            boundaryFailuresAreReturnedWithoutMutation);
    addTest(tests, "manager_authentication.operation_context",
            operationContextIsForwardedAndRevalidated);
    addTest(tests, "manager_authentication.capacity_one",
            admissionCapacityIsExactlyOne);
    addTest(tests, "manager_authentication.shutdown",
            shutdownIsIdempotentAndRejectsNewCalls);
    addTest(tests, "manager_authentication.constant_time_comparison",
            constantTimeComparisonCoversEveryMismatchPosition);
}

} // namespace ForgeConductor::Tests
