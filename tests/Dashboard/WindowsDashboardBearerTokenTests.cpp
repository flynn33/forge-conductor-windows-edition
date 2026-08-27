#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBearerToken.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

static_assert(std::is_final_v<Contracts::DashboardBearerSecret>);
static_assert(!std::is_copy_constructible_v<Contracts::DashboardBearerSecret>);
static_assert(!std::is_copy_assignable_v<Contracts::DashboardBearerSecret>);
static_assert(std::is_nothrow_move_constructible_v<Contracts::DashboardBearerSecret>);
static_assert(std::is_nothrow_move_assignable_v<Contracts::DashboardBearerSecret>);
static_assert(std::is_final_v<Windows::WindowsDashboardBearerTokenGenerator>);
static_assert(std::is_final_v<Windows::WindowsDashboardBearerTokenStore>);
static_assert(!std::is_copy_constructible_v<Windows::WindowsDashboardBearerTokenStore>);
static_assert(!std::is_copy_assignable_v<Windows::WindowsDashboardBearerTokenStore>);
static_assert(noexcept(std::declval<Contracts::IDashboardBearerTokenGenerator&>().next()));
static_assert(noexcept(std::declval<Contracts::IDashboardBearerTokenStore&>().load(
    std::declval<const Domain::OperationContext&>())));
static_assert(noexcept(std::declval<Contracts::IDashboardBearerTokenStore&>().loadOrCreate(
    std::declval<const Domain::OperationContext&>())));
static_assert(noexcept(std::declval<Contracts::IDashboardBearerTokenStore&>().shutdown()));
static_assert(noexcept(Contracts::encodeDashboardBearerToken(
    std::declval<const Contracts::DashboardBearerSecret&>())));
static_assert(noexcept(Contracts::constantTimeDashboardBearerTokenEquals(
    std::declval<const Domain::Sha256Digest&>(),
    std::declval<const Domain::Sha256Digest&>())));
static_assert(Contracts::DashboardBearerSecret::SizeBytes == 32U);
static_assert(Windows::WindowsDashboardBearerTokenStore::StorageKey ==
              std::string_view{"manager.dashboard.bearer.v1"});
static_assert(Windows::WindowsDashboardBearerTokenStore::StorageKey !=
              std::string_view{"manager.ipc.nonce.v1"});

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == code);
}

struct TestContext final {
    Domain::MonotonicTimePoint now{std::chrono::steady_clock::now()};
    Domain::OperationId operationId{parse<Domain::OperationId>(
        "11111111-1111-4111-8111-111111111111")};
    Domain::CorrelationId correlationId{parse<Domain::CorrelationId>(
        "dashboard-bearer-test")};
    std::stop_source cancellation;

    [[nodiscard]] Domain::OperationContext active() const
    {
        return Domain::OperationContext{
            operationId,
            now + std::chrono::minutes{5},
            cancellation.get_token(),
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext expired() const
    {
        return Domain::OperationContext{
            operationId,
            now,
            cancellation.get_token(),
            correlationId};
    }
};

[[nodiscard]] Contracts::DashboardBearerSecret::Bytes sequentialBytes() noexcept
{
    Contracts::DashboardBearerSecret::Bytes bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    return bytes;
}

[[nodiscard]] Contracts::DashboardBearerSecret::Bytes reverseBytes() noexcept
{
    Contracts::DashboardBearerSecret::Bytes bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(0xffU - index);
    }
    return bytes;
}

[[nodiscard]] bool allZero(
    const std::span<const std::byte, Contracts::DashboardBearerSecret::SizeBytes> bytes)
    noexcept
{
    return std::ranges::all_of(bytes, [](const std::byte value) noexcept {
        return value == std::byte{};
    });
}

[[nodiscard]] Domain::Error scriptedError(const std::string_view code)
{
    return Domain::makeError(code, "Scripted dashboard bearer boundary failure.");
}

class ScriptedGenerator final : public Contracts::IDashboardBearerTokenGenerator {
public:
    explicit ScriptedGenerator(Contracts::DashboardBearerSecret::Bytes bytes) noexcept
        : bytes_{bytes}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::DashboardBearerSecret> next()
        noexcept override
    {
        try {
            ++calls_;
            if (afterNext_) {
                afterNext_();
            }
            if (error_.has_value()) {
                return Domain::Result<Contracts::DashboardBearerSecret>::failure(*error_);
            }
            return Domain::Result<Contracts::DashboardBearerSecret>::success(
                Contracts::DashboardBearerSecret{bytes_});
        } catch (...) {
            return Domain::Result<Contracts::DashboardBearerSecret>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Scripted dashboard bearer generation failed."));
        }
    }

    void failWith(Domain::Error error) { error_ = std::move(error); }
    void runAfterNext(std::function<void()> action) { afterNext_ = std::move(action); }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

private:
    Contracts::DashboardBearerSecret::Bytes bytes_{};
    std::optional<Domain::Error> error_;
    std::function<void()> afterNext_;
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
            std::function<void()> afterPut;
            std::optional<Domain::Error> error;
            {
                std::lock_guard lock{mutex_};
                ++putCalls_;
                lastPutKey_ = key;
                lastPutContext_ = context;
                capturedPut_.assign(secret.begin(), secret.end());
                afterPut = afterPut_;
                error = putError_;
                if (!error.has_value()) {
                    stored_.emplace(secret.begin(), secret.end());
                }
            }
            if (afterPut) {
                afterPut();
            }
            if (error.has_value()) {
                return Domain::Result<void>::failure(std::move(error).value());
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
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
                condition_.wait(lock, [this]() noexcept {
                    return !blockGet_ || releaseGet_;
                });
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
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Scripted secure-storage get failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> remove(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        ++removeCalls_;
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard bearer storage must not remove its durable secret."));
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

    void runAfterPut(std::function<void()> action)
    {
        std::lock_guard lock{mutex_};
        afterPut_ = std::move(action);
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
        return condition_.wait_for(lock, timeout, [this]() noexcept {
            return getEntered_;
        });
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

    [[nodiscard]] std::size_t removeCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return removeCalls_;
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
    std::function<void()> afterPut_;
    std::string lastGetKey_;
    std::string lastPutKey_;
    std::vector<std::byte> capturedPut_;
    std::optional<Domain::OperationContext> lastGetContext_;
    std::optional<Domain::OperationContext> lastPutContext_;
    std::size_t lastMaximumBytes_{};
    std::size_t getCalls_{};
    std::size_t putCalls_{};
    std::size_t removeCalls_{};
    std::size_t shutdownCalls_{};
    bool blockGet_{};
    bool releaseGet_{};
    bool getEntered_{};
};

[[nodiscard]] std::vector<std::byte> vectorFrom(
    const Contracts::DashboardBearerSecret::Bytes& bytes)
{
    return {bytes.begin(), bytes.end()};
}

void ownedRawMaterialIsMoveOnlyAndWipedOnTransfer()
{
    const auto firstBytes = sequentialBytes();
    Contracts::DashboardBearerSecret first{firstBytes};
    Contracts::DashboardBearerSecret moved{std::move(first)};
    REQUIRE(allZero(first.bytes()));
    REQUIRE(std::ranges::equal(moved.bytes(), firstBytes));

    const auto secondBytes = reverseBytes();
    Contracts::DashboardBearerSecret second{secondBytes};
    moved = std::move(second);
    REQUIRE(allZero(second.bytes()));
    REQUIRE(std::ranges::equal(moved.bytes(), secondBytes));

    moved = std::move(moved);
    REQUIRE(std::ranges::equal(moved.bytes(), secondBytes));
}

void exactLowercaseHexEncodingPreservesAllNibbles()
{
    const auto bytes = sequentialBytes();
    const Contracts::DashboardBearerSecret secret{bytes};
    const auto token = take(Contracts::encodeDashboardBearerToken(secret));

    REQUIRE(token.value() ==
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f");
    REQUIRE(token.value().size() == 64U);
    for (const char character : token.value()) {
        REQUIRE((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f'));
    }
}

void constantTimeComparisonCoversEveryMismatchPosition()
{
    const auto baseline = parse<Domain::Sha256Digest>(std::string(64U, 'a'));
    REQUIRE(Contracts::constantTimeDashboardBearerTokenEquals(baseline, baseline));

    for (std::size_t index = 0U; index < 64U; ++index) {
        std::string changed(64U, 'a');
        changed[index] = 'b';
        const auto mismatch = parse<Domain::Sha256Digest>(changed);
        REQUIRE(!Contracts::constantTimeDashboardBearerTokenEquals(
            baseline, mismatch));
        REQUIRE(!Contracts::constantTimeDashboardBearerTokenEquals(
            mismatch, baseline));
    }
}

void windowsGeneratorProducesAValidBoundedBearer()
{
    Windows::WindowsDashboardBearerTokenGenerator generator;
    auto secret = take(generator.next());
    REQUIRE(secret.bytes().size() == Contracts::DashboardBearerSecret::SizeBytes);
    const auto token = take(Contracts::encodeDashboardBearerToken(secret));
    REQUIRE(token.value().size() == 64U);
    for (const char character : token.value()) {
        REQUIRE((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f'));
    }
}

void missingBearerIsCreatedPersistedAndReloaded()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    const auto expectedBytes = sequentialBytes();
    ScriptedGenerator generator{expectedBytes};
    Windows::WindowsDashboardBearerTokenStore store{storage, generator};
    const auto active = fixture.active();

    const auto missing = take(store.load(active));
    REQUIRE(!missing.has_value());
    REQUIRE(storage.getCalls() == 1U);
    REQUIRE(storage.putCalls() == 0U);

    const auto created = take(store.loadOrCreate(active));
    REQUIRE(created.value() ==
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f");
    REQUIRE(generator.calls() == 1U);
    REQUIRE(storage.putCalls() == 1U);
    REQUIRE(storage.lastGetKey() ==
            Windows::WindowsDashboardBearerTokenStore::StorageKey);
    REQUIRE(storage.lastPutKey() ==
            Windows::WindowsDashboardBearerTokenStore::StorageKey);
    REQUIRE(storage.lastGetKey() != "manager.ipc.nonce.v1");
    REQUIRE(storage.lastPutKey() != "manager.ipc.nonce.v1");
    REQUIRE(storage.lastMaximumBytes() ==
            Contracts::DashboardBearerSecret::SizeBytes);
    REQUIRE(std::ranges::equal(storage.capturedPut(), expectedBytes));
    const auto putContext = storage.lastPutContext();
    REQUIRE(putContext.has_value());
    REQUIRE(putContext->operationId == active.operationId);
    REQUIRE(putContext->correlationId == active.correlationId);
    REQUIRE(putContext->deadline == active.deadline);
    REQUIRE(putContext->cancellation == active.cancellation);

    const auto reloaded = take(store.loadOrCreate(active));
    REQUIRE(reloaded == created);
    REQUIRE(generator.calls() == 1U);
    REQUIRE(storage.putCalls() == 1U);
    REQUIRE(storage.removeCalls() == 0U);
}

void validStoredBearerLoadsWithoutMutation()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    const auto expectedBytes = reverseBytes();
    storage.setStored(vectorFrom(expectedBytes));
    ScriptedGenerator generator{sequentialBytes()};
    Windows::WindowsDashboardBearerTokenStore store{storage, generator};

    const auto loaded = take(store.load(fixture.active()));
    REQUIRE(loaded.has_value());
    const Contracts::DashboardBearerSecret secret{expectedBytes};
    REQUIRE(loaded.value() == take(Contracts::encodeDashboardBearerToken(secret)));
    REQUIRE(generator.calls() == 0U);
    REQUIRE(storage.putCalls() == 0U);
    REQUIRE(storage.removeCalls() == 0U);
}

void invalidStoredLengthsAreRejectedWithoutRegeneration()
{
    TestContext fixture;
    for (const std::size_t length : {0U, 1U, 31U, 33U, 64U}) {
        StatefulSecureStorage storage;
        storage.setStored(std::vector<std::byte>(length, std::byte{0x5a}));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
        REQUIRE(storage.lastMaximumBytes() ==
                Contracts::DashboardBearerSecret::SizeBytes);
    }
}

void dependencyFailuresRemainTypedAndDoNotMutate()
{
    TestContext fixture;
    {
        StatefulSecureStorage storage;
        storage.failGetWith(scriptedError(Domain::ErrorCodes::StorageFull));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::StorageFull);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
    }
    {
        StatefulSecureStorage storage;
        ScriptedGenerator generator{sequentialBytes()};
        generator.failWith(scriptedError(Domain::ErrorCodes::InternalFailure));
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::InternalFailure);
        REQUIRE(generator.calls() == 1U);
        REQUIRE(storage.putCalls() == 0U);
    }
    {
        StatefulSecureStorage storage;
        storage.failPutWith(scriptedError(Domain::ErrorCodes::StorageFull));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};

        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::StorageFull);
        REQUIRE(generator.calls() == 1U);
        REQUIRE(storage.putCalls() == 1U);
        REQUIRE(!storage.stored().has_value());
    }
}

void operationContextIsForwardedAndRevalidated()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    ScriptedGenerator generator{sequentialBytes()};
    Windows::WindowsDashboardBearerTokenStore store{storage, generator};
    const auto active = fixture.active();

    const auto missing = take(store.load(active));
    REQUIRE(!missing.has_value());
    const auto captured = storage.lastGetContext();
    REQUIRE(captured.has_value());
    REQUIRE(captured->operationId == active.operationId);
    REQUIRE(captured->correlationId == active.correlationId);
    REQUIRE(captured->deadline == active.deadline);
    REQUIRE(captured->cancellation == active.cancellation);

    std::stop_source cancelled;
    cancelled.request_stop();
    const Domain::OperationContext cancelledContext{
        active.operationId,
        active.deadline,
        cancelled.get_token(),
        active.correlationId};
    const auto beforeCancelled = storage.getCalls();
    requireError(store.load(cancelledContext), Domain::ErrorCodes::Cancelled);
    REQUIRE(storage.getCalls() == beforeCancelled);

    const auto beforeExpired = storage.getCalls();
    requireError(store.load(fixture.expired()), Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(storage.getCalls() == beforeExpired);

    std::stop_source cancelAfterRead;
    const Domain::OperationContext cancelledDuringRead{
        active.operationId,
        active.deadline,
        cancelAfterRead.get_token(),
        active.correlationId};
    storage.runAfterGet([&cancelAfterRead]() noexcept {
        cancelAfterRead.request_stop();
    });
    requireError(store.loadOrCreate(cancelledDuringRead),
                 Domain::ErrorCodes::Cancelled);
    REQUIRE(generator.calls() == 0U);
    REQUIRE(storage.putCalls() == 0U);
}

void cancellationAfterGenerationPreventsPersistence()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    ScriptedGenerator generator{sequentialBytes()};
    Windows::WindowsDashboardBearerTokenStore store{storage, generator};
    std::stop_source cancellation;
    const auto active = fixture.active();
    const Domain::OperationContext context{
        active.operationId,
        active.deadline,
        cancellation.get_token(),
        active.correlationId};
    generator.runAfterNext([&cancellation]() noexcept {
        cancellation.request_stop();
    });

    requireError(store.loadOrCreate(context), Domain::ErrorCodes::Cancelled);
    REQUIRE(generator.calls() == 1U);
    REQUIRE(storage.putCalls() == 0U);
    REQUIRE(!storage.stored().has_value());
}

void postReadContextValidationCoversLoadAndExistingLoadOrCreate()
{
    TestContext fixture;
    const auto active = fixture.active();
    const auto storedBytes = reverseBytes();

    {
        StatefulSecureStorage storage;
        storage.setStored(vectorFrom(storedBytes));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        std::stop_source cancellation;
        const Domain::OperationContext context{
            active.operationId,
            active.deadline,
            cancellation.get_token(),
            active.correlationId};
        storage.runAfterGet([&cancellation]() noexcept {
            cancellation.request_stop();
        });

        requireError(store.load(context), Domain::ErrorCodes::Cancelled);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
    }

    {
        StatefulSecureStorage storage;
        storage.setStored(vectorFrom(storedBytes));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        std::stop_source cancellation;
        const Domain::OperationContext context{
            active.operationId,
            active.deadline,
            cancellation.get_token(),
            active.correlationId};
        storage.runAfterGet([&cancellation]() noexcept {
            cancellation.request_stop();
        });

        requireError(store.loadOrCreate(context), Domain::ErrorCodes::Cancelled);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
    }

    {
        StatefulSecureStorage storage;
        storage.setStored(vectorFrom(storedBytes));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds{100};
        const Domain::OperationContext context{
            active.operationId,
            deadline,
            active.cancellation,
            active.correlationId};
        storage.runAfterGet([deadline]() {
            std::this_thread::sleep_until(deadline);
        });

        requireError(store.load(context), Domain::ErrorCodes::DeadlineExceeded);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
    }

    {
        StatefulSecureStorage storage;
        storage.setStored(vectorFrom(storedBytes));
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds{100};
        const Domain::OperationContext context{
            active.operationId,
            deadline,
            active.cancellation,
            active.correlationId};
        storage.runAfterGet([deadline]() {
            std::this_thread::sleep_until(deadline);
        });

        requireError(store.loadOrCreate(context),
                     Domain::ErrorCodes::DeadlineExceeded);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(generator.calls() == 0U);
        REQUIRE(storage.putCalls() == 0U);
    }
}

void admissionCapacityIsExactlyOne()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    storage.blockGet();
    ScriptedGenerator generator{sequentialBytes()};
    Windows::WindowsDashboardBearerTokenStore store{storage, generator};
    std::optional<Domain::Result<std::optional<Domain::Sha256Digest>>> firstResult;

    std::jthread first{[&]() {
        firstResult.emplace(store.load(fixture.active()));
    }};
    const bool entered = storage.waitForBlockedGet(std::chrono::seconds{2});
    if (!entered) {
        storage.releaseBlockedGet();
        first.join();
        REQUIRE(false);
    }

    const auto rejected = store.loadOrCreate(fixture.active());
    requireError(rejected, Domain::ErrorCodes::LimitExceeded);
    REQUIRE(rejected.error().retryable);
    REQUIRE(storage.getCalls() == 1U);
    REQUIRE(generator.calls() == 0U);

    storage.releaseBlockedGet();
    first.join();
    REQUIRE(firstResult.has_value());
    REQUIRE(firstResult.value());
    REQUIRE(!firstResult.value().value().has_value());
}

void shutdownIsIdempotentAndDoesNotOwnDependencies()
{
    TestContext fixture;
    StatefulSecureStorage storage;
    ScriptedGenerator generator{sequentialBytes()};
    {
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        store.shutdown();
        store.shutdown();
        requireError(store.load(fixture.active()),
                     Domain::ErrorCodes::TransportClosed);
        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::TransportClosed);
    }

    REQUIRE(storage.getCalls() == 0U);
    REQUIRE(storage.putCalls() == 0U);
    REQUIRE(storage.removeCalls() == 0U);
    REQUIRE(storage.shutdownCalls() == 0U);
    REQUIRE(generator.calls() == 0U);
}

void shutdownIsPromptDuringBlockedAndReentrantReads()
{
    TestContext fixture;
    {
        StatefulSecureStorage storage;
        storage.blockGet();
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        std::optional<Domain::Result<std::optional<Domain::Sha256Digest>>> firstResult;
        std::mutex completionMutex;
        std::condition_variable completionCondition;
        bool shutdownReturned{};

        std::jthread first{[&]() {
            firstResult.emplace(store.load(fixture.active()));
        }};
        const bool entered = storage.waitForBlockedGet(std::chrono::seconds{2});
        if (!entered) {
            storage.releaseBlockedGet();
            first.join();
            REQUIRE(false);
        }

        std::jthread shutdownThread{[&]() {
            store.shutdown();
            {
                std::lock_guard lock{completionMutex};
                shutdownReturned = true;
            }
            completionCondition.notify_all();
        }};

        bool returnedBeforeDependencyRelease{};
        {
            std::unique_lock lock{completionMutex};
            returnedBeforeDependencyRelease = completionCondition.wait_for(
                lock,
                std::chrono::milliseconds{500},
                [&shutdownReturned]() noexcept { return shutdownReturned; });
        }
        const auto rejectedWhileFirstIsBlocked = store.load(fixture.active());
        storage.releaseBlockedGet();
        shutdownThread.join();
        first.join();

        REQUIRE(returnedBeforeDependencyRelease);
        requireError(rejectedWhileFirstIsBlocked,
                     Domain::ErrorCodes::TransportClosed);
        REQUIRE(firstResult.has_value());
        requireError(firstResult.value(), Domain::ErrorCodes::TransportClosed);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(storage.shutdownCalls() == 0U);
    }

    {
        StatefulSecureStorage storage;
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        storage.runAfterGet([&store]() noexcept {
            store.shutdown();
        });

        requireError(store.load(fixture.active()),
                     Domain::ErrorCodes::TransportClosed);
        requireError(store.loadOrCreate(fixture.active()),
                     Domain::ErrorCodes::TransportClosed);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(storage.putCalls() == 0U);
        REQUIRE(generator.calls() == 0U);
    }

    {
        StatefulSecureStorage storage;
        ScriptedGenerator generator{sequentialBytes()};
        Windows::WindowsDashboardBearerTokenStore store{storage, generator};
        storage.runAfterPut([&store]() noexcept {
            store.shutdown();
        });

        const auto committed = store.loadOrCreate(fixture.active());
        REQUIRE(committed);
        requireError(store.load(fixture.active()),
                     Domain::ErrorCodes::TransportClosed);
        REQUIRE(storage.getCalls() == 1U);
        REQUIRE(storage.putCalls() == 1U);
        REQUIRE(generator.calls() == 1U);
    }
}

} // namespace

int main()
{
    try {
        ownedRawMaterialIsMoveOnlyAndWipedOnTransfer();
        exactLowercaseHexEncodingPreservesAllNibbles();
        constantTimeComparisonCoversEveryMismatchPosition();
        windowsGeneratorProducesAValidBoundedBearer();
        missingBearerIsCreatedPersistedAndReloaded();
        validStoredBearerLoadsWithoutMutation();
        invalidStoredLengthsAreRejectedWithoutRegeneration();
        dependencyFailuresRemainTypedAndDoNotMutate();
        operationContextIsForwardedAndRevalidated();
        cancellationAfterGenerationPreventsPersistence();
        postReadContextValidationCoversLoadAndExistingLoadOrCreate();
        admissionCapacityIsExactlyOne();
        shutdownIsIdempotentAndDoesNotOwnDependencies();
        shutdownIsPromptDuringBlockedAndReentrantReads();
        std::cout << "Windows dashboard bearer token tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows dashboard bearer token tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Windows dashboard bearer token tests failed with an unknown error.\n";
        return 1;
    }
}
