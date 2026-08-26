#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"
#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"
#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/InMemoryNativeSessionLedger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace SessionHost = ForgeConductor::SessionHost;
namespace Fakes = ForgeConductor::Tests::Fakes;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view expression)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

[[nodiscard]] std::string uuidText(const std::uint64_t value)
{
    std::ostringstream stream;
    stream << "20000000-0000-4000-8000-" << std::hex
           << std::nouppercase;
    stream.width(12);
    stream.fill('0');
    stream << value;
    return stream.str();
}

[[nodiscard]] std::vector<Domain::Uuid> uuidSequence(
    const std::uint64_t first,
    const std::size_t count = 32U)
{
    std::vector<Domain::Uuid> values;
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        values.push_back(parse<Domain::Uuid>(uuidText(first + index)));
    }
    return values;
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Contracts::IClock& clock,
    const std::uint64_t identifier,
    const std::string_view correlation,
    const std::stop_token cancellation = {},
    const bool expired = false)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(uuidText(90'000U + identifier)),
        clock.monotonicNow() + (expired ? 0s : 5min),
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::SessionCreationRequest creationRequest(
    const std::uint64_t identifier,
    std::optional<Domain::IdempotencyKey> idempotencyKey = std::nullopt)
{
    return Domain::SessionCreationRequest{
        parse<Domain::ContinuityOperationId>(uuidText(10'000U + identifier)),
        parse<Domain::ProjectId>(uuidText(1'000U + identifier)),
        parse<Domain::SessionId>(uuidText(30'000U + identifier)),
        idempotencyKey.value_or(take(Domain::IdempotencyKey::create(
            "native-session-" + std::to_string(identifier))))};
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

class ScriptedNativeTransport final
    : public Contracts::INativeSessionTransport {
public:
    enum class BootstrapMode {
        Exact,
        MalformedJson,
        OversizedChunk,
        NegativeUsage,
        WrongHandoff,
        WrongSession
    };

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++createCalls_;
            createRequests_.push_back(request);
            if (shutdown_ || context.isCancellationRequested() ||
                cancelledOperation(context.operationId)) {
                return cancelled<Domain::NativeTransportSession>();
            }
            if (rateLimitedAttempts_ > 0U) {
                --rateLimitedAttempts_;
                return Domain::Result<Domain::NativeTransportSession>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RateLimited,
                        "The scripted provider is rate limited.",
                        true));
            }
            const auto existing = sessions_.find(request.idempotencyKey.value());
            if (existing != sessions_.end()) {
                return Domain::Result<Domain::NativeTransportSession>::success(
                    existing->second);
            }
            auto providerId = Domain::ProviderSessionId::parse(
                "provider-" + request.operationId.value(), 512U);
            if (!providerId) {
                return Domain::Result<Domain::NativeTransportSession>::failure(
                    std::move(providerId).error());
            }
            Domain::NativeTransportSession session{
                std::move(providerId).value(),
                std::optional<std::string>{"scripted-native-model"}};
            sessions_.emplace(request.idempotencyKey.value(), session);
            return Domain::Result<Domain::NativeTransportSession>::success(
                std::move(session));
        } catch (...) {
            return internal<Domain::NativeTransportSession>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++bootstrapCalls_;
            lastBootstrap_ = request;
            if (shutdown_ || context.isCancellationRequested() ||
                cancelledOperation(context.operationId)) {
                return cancelled<Domain::NativeBootstrapResponse>();
            }
            if (bootstrapMode_ == BootstrapMode::OversizedChunk) {
                return Domain::Result<Domain::NativeBootstrapResponse>::success(
                    Domain::NativeBootstrapResponse{
                        {std::vector<std::byte>(
                            Domain::MaximumNativeResponseChunkBytes + 1U)},
                        1,
                        1});
            }
            if (bootstrapMode_ == BootstrapMode::MalformedJson) {
                return Domain::Result<Domain::NativeBootstrapResponse>::success(
                    Domain::NativeBootstrapResponse{{bytes("{")}, 1, 1});
            }
            auto handoffId = request.handoffId.value();
            auto successorId = request.successorSessionId.value();
            if (bootstrapMode_ == BootstrapMode::WrongHandoff) {
                handoffId = uuidText(71'111U);
            }
            if (bootstrapMode_ == BootstrapMode::WrongSession) {
                successorId = uuidText(72'222U);
            }
            const std::string acknowledgement =
                "{\"handoff_id\":\"" + handoffId +
                "\",\"successor_session_id\":\"" + successorId + "\"}";
            const auto split = acknowledgement.size() / 2U;
            return Domain::Result<Domain::NativeBootstrapResponse>::success(
                Domain::NativeBootstrapResponse{
                    {bytes(std::string_view{acknowledgement}.substr(0U, split)),
                     bytes(std::string_view{acknowledgement}.substr(split))},
                    bootstrapMode_ == BootstrapMode::NegativeUsage ? -1 : 11,
                    7});
        } catch (...) {
            return internal<Domain::NativeBootstrapResponse>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++queryCalls_;
            if (shutdown_ || context.isCancellationRequested()) {
                return cancelled<Domain::HostSessionStatus>();
            }
            const auto found = std::find_if(
                sessions_.begin(), sessions_.end(), [&](const auto& value) {
                    return value.second.providerSessionId == sessionId;
                });
            if (found == sessions_.end()) {
                return Domain::Result<Domain::HostSessionStatus>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::SessionNotFound,
                        "The scripted provider session was not found."));
            }
            return Domain::Result<Domain::HostSessionStatus>::success(
                Domain::HostSessionStatus::Ready);
        } catch (...) {
            return internal<Domain::HostSessionStatus>();
        }
    }

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            cancelled_.emplace_back(operationId, sessionId);
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
        } catch (...) {
        }
    }

    void setRateLimitedAttempts(const std::size_t attempts) noexcept
    {
        std::lock_guard lock{mutex_};
        rateLimitedAttempts_ = attempts;
    }

    void setBootstrapMode(const BootstrapMode mode) noexcept
    {
        std::lock_guard lock{mutex_};
        bootstrapMode_ = mode;
    }

    [[nodiscard]] std::size_t createCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return createCalls_;
    }

    [[nodiscard]] std::size_t bootstrapCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return bootstrapCalls_;
    }

    [[nodiscard]] std::size_t queryCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return queryCalls_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelled_.size();
    }

    [[nodiscard]] bool isShutdown() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdown_;
    }

    [[nodiscard]] std::optional<Domain::NativeBootstrapRequest>
    lastBootstrap() const
    {
        std::lock_guard lock{mutex_};
        return lastBootstrap_;
    }

private:
    [[nodiscard]] bool cancelledOperation(
        const Domain::OperationId& operationId) const
    {
        return std::any_of(
            cancelled_.begin(), cancelled_.end(), [&](const auto& value) {
                return value.first == operationId;
            });
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> cancelled() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The scripted native transport was cancelled."));
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> internal() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The scripted native transport failed safely."));
    }

    mutable std::mutex mutex_;
    std::map<std::string, Domain::NativeTransportSession> sessions_;
    std::vector<Domain::SessionCreationRequest> createRequests_;
    std::vector<std::pair<
        Domain::OperationId,
        std::optional<Domain::ProviderSessionId>>> cancelled_;
    std::optional<Domain::NativeBootstrapRequest> lastBootstrap_;
    std::size_t rateLimitedAttempts_{};
    std::size_t createCalls_{};
    std::size_t bootstrapCalls_{};
    std::size_t queryCalls_{};
    BootstrapMode bootstrapMode_{BootstrapMode::Exact};
    bool shutdown_{};
};

class RecordingContinuityDocumentCodec final
    : public Contracts::IContinuityDocumentCodec {
public:
    explicit RecordingContinuityDocumentCodec(
        Contracts::IContinuityDocumentCodec& inner) noexcept
        : inner_{&inner}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        encodeCalls_.fetch_add(1U, std::memory_order_relaxed);
        return inner_->encode(handoff, context);
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> decode(
        const std::string_view canonicalUtf8,
        const Domain::OperationContext& context) noexcept override
    {
        decodeCalls_.fetch_add(1U, std::memory_order_relaxed);
        return inner_->decode(canonicalUtf8, context);
    }

    [[nodiscard]] std::size_t decodeCalls() const noexcept
    {
        return decodeCalls_.load(std::memory_order_relaxed);
    }

private:
    Contracts::IContinuityDocumentCodec* inner_{};
    std::atomic_size_t encodeCalls_{};
    std::atomic_size_t decodeCalls_{};
};

class BlockingContinuationScheduler final
    : public Contracts::INativeLogicalContinuationScheduler {
public:
    [[nodiscard]] Domain::Result<void> schedule(
        const Domain::NativeLogicalContinuation& continuation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            if (shutdown_ || context.isCancellationRequested()) {
                return cancelled();
            }
            if (context.isExpired(std::chrono::steady_clock::now())) {
                return deadlineExceeded();
            }
            ++scheduleCalls_;
            accepted_ = continuation;
            scheduleEntered_ = true;
            changed_.notify_all();
            const auto released = changed_.wait_until(
                lock,
                context.deadline,
                [&] {
                    return releaseRequested_ || shutdown_ ||
                           context.isCancellationRequested();
                });
            if (shutdown_ || context.isCancellationRequested()) {
                return cancelled();
            }
            if (!released) {
                return deadlineExceeded();
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The blocking scheduler failed safely."));
        }
    }

    void cancel(
        const Domain::ProviderSessionId& sessionId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++cancelCalls_;
            if (accepted_ && accepted_->providerSessionId == sessionId) {
                accepted_.reset();
            }
            changed_.notify_all();
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++shutdownCalls_;
            shutdown_ = true;
            accepted_.reset();
            changed_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilScheduleEntered(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock, timeout, [this] { return scheduleEntered_; });
        } catch (...) {
            return false;
        }
    }

    void release() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            releaseRequested_ = true;
            changed_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool hasAcceptedWork() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return accepted_.has_value();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t scheduleCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return scheduleCalls_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

private:
    [[nodiscard]] static Domain::Result<void> cancelled()
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The blocking scheduler was cancelled."));
    }

    [[nodiscard]] static Domain::Result<void> deadlineExceeded()
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The blocking scheduler exceeded its deadline."));
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Domain::NativeLogicalContinuation> accepted_;
    std::size_t scheduleCalls_{};
    std::size_t cancelCalls_{};
    std::size_t shutdownCalls_{};
    bool scheduleEntered_{};
    bool releaseRequested_{};
    bool shutdown_{};
};

struct AdapterFixture final {
    explicit AdapterFixture(
        const std::uint64_t firstUuid = 50'000U,
        const std::size_t uuidCount = 32U)
        : clock{std::make_shared<Fakes::FakeClock>(
              Domain::UtcTimePoint{1'800'000'000s},
              std::chrono::steady_clock::now())},
          hasher{std::make_shared<InfrastructureWindows::BCryptSha256Hasher>()},
          codec{hasher, clock},
          uuidGenerator{uuidSequence(firstUuid, uuidCount)},
          adapter{
              parse<Domain::AdapterId>(
                  SessionHost::ForgeNativeSessionHostAdapter::AdapterIdentifier),
              ledger,
              transport,
              codec,
              uuidGenerator,
              *clock}
    {
    }

    std::shared_ptr<Fakes::FakeClock> clock;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher;
    InfrastructureWindows::WindowsContinuityDocumentCodec codec;
    Fakes::InMemoryNativeSessionLedger ledger;
    ScriptedNativeTransport transport;
    Fakes::SequenceUuidGenerator uuidGenerator;
    SessionHost::ForgeNativeSessionHostAdapter adapter;
};

[[nodiscard]] Domain::ContinuityHandoff handoffFor(
    const std::uint64_t identifier,
    const Domain::SessionCreationRequest& request,
    const Domain::HostSession& successor,
    InfrastructureWindows::WindowsContinuityDocumentCodec& codec,
    const Contracts::IClock& clock)
{
    Domain::ContinuityHandoff handoff{
        parse<Domain::ContinuityHandoffId>(uuidText(20'000U + identifier)),
        request.operationId,
        clock.utcNow(),
        Domain::ContinuityProject{
            request.projectId,
            "Native host project " + std::to_string(identifier),
            take(Domain::PathText::create(
                "D:/native-host/project-" + std::to_string(identifier))),
            "main",
            "0123456789abcdef",
            {}},
        Domain::ContinuitySession{
            request.predecessorSessionId,
            std::nullopt,
            std::optional<std::string>{"predecessor-model"},
            std::optional<std::string>{"test-provider"}},
        Domain::ContinuitySession{
            successor.id,
            successor.providerSessionId,
            successor.model,
            std::optional<std::string>{"test-provider"}},
        "Consume the canonical handoff and continue autonomously",
        {"Preserve the exact successor binding"},
        Domain::ContinuityCurrentWork{
            "P12",
            "native-session-host",
            "Bootstrap the logical successor",
            {take(Domain::PathText::create(
                "tests/SessionHost/NativeSessionHostAdapterTests.cpp"))}},
        {},
        {{std::optional<std::string>{"native-host"},
          "Schedule the continuation receipt",
          std::optional<std::string>{"open"}}},
        {{"Consume canonical bytes before acknowledging", std::nullopt}},
        Domain::ContinuityValidation{{"G11"}, {"G12"}, {}},
        {},
        {},
        {{1U,
          "Run the scheduled continuation",
          "forge continue --exact",
          "The successor reports the next action complete"}},
        Domain::ContinuityHostState{
            parse<Domain::AdapterId>(
                SessionHost::ForgeNativeSessionHostAdapter::AdapterIdentifier),
            Domain::ContinuityState::SuccessorCreated,
            "native-host-test",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        true};
    return take(codec.encode(
        handoff,
        operationContext(clock, 70'000U + identifier, "native-handoff-codec")))
        .handoff;
}

void inMemoryLedgerUsesRevisionCasAndCommittedDigest()
{
    Fakes::FakeClock clock{
        Domain::UtcTimePoint{1'800'000'000s},
        std::chrono::steady_clock::now()};
    Fakes::InMemoryNativeSessionLedger ledger;
    const auto context = operationContext(clock, 0U, "native-ledger-cas");
    const auto initial = take(ledger.load(context));
    REQUIRE(initial.revision == 0U);
    REQUIRE(!initial.contentSha256.has_value());
    const auto committed = take(ledger.commit(initial, 0U, context));
    REQUIRE(committed.revision == 1U);
    REQUIRE(committed.contentSha256.has_value());
    REQUIRE(Domain::validateNativeSessionLedger(committed));
    requireError(
        ledger.commit(initial, 0U, context),
        Domain::ErrorCodes::Conflict);
    ledger.shutdown();
    requireError(ledger.load(context), Domain::ErrorCodes::Cancelled);
}

void exactCapabilitiesManifestAndHealth()
{
    AdapterFixture fixture;
    const auto context = operationContext(
        *fixture.clock, 1U, "native-capabilities");
    REQUIRE(fixture.adapter.identifier().value() ==
            SessionHost::ForgeNativeSessionHostAdapter::AdapterIdentifier);
    REQUIRE(fixture.adapter.version() ==
            SessionHost::ForgeNativeSessionHostAdapter::AdapterVersion);
    const auto capabilities = take(fixture.adapter.capabilities(context));
    REQUIRE(capabilities.create);
    REQUIRE(capabilities.bootstrap);
    REQUIRE(capabilities.usageReporting);
    REQUIRE(capabilities.resume);
    REQUIRE(capabilities.idempotency);
    REQUIRE(capabilities.queryByIdempotencyKey);
    REQUIRE(capabilities.recovery);
    REQUIRE(capabilities.cancellation);

    const auto executable = take(Domain::PathText::create(
        "D:/Forge/ForgeNativeSessionHostPlugin.dll"));
    const auto manifest = take(
        SessionHost::nativeSessionHostPluginManifest(executable));
    REQUIRE(manifest.adapterId == fixture.adapter.identifier());
    REQUIRE(manifest.adapterVersion == fixture.adapter.version());
    REQUIRE(manifest.protocolVersion ==
            SessionHost::ForgeNativeSessionHostAdapter::ProtocolVersion);
    REQUIRE(manifest.executable == executable);
    REQUIRE(manifest.capabilities.create && manifest.capabilities.bootstrap &&
            manifest.capabilities.usageReporting && manifest.capabilities.resume &&
            manifest.capabilities.idempotency &&
            manifest.capabilities.queryByIdempotencyKey &&
            manifest.capabilities.recovery &&
            manifest.capabilities.cancellation);

    const auto health = take(fixture.adapter.health(context));
    REQUIRE(health.healthy);
    REQUIRE(health.records == 0U);
    REQUIRE(health.maximumRecords == Domain::MaximumNativeSessionRecords);
    REQUIRE(health.maximumResponseBytes == Domain::MaximumNativeResponseBytes);
}

void createIntentReplayAndBindingConflicts()
{
    AdapterFixture fixture;
    const auto request = creationRequest(2U);
    const auto firstContext = operationContext(
        *fixture.clock, 2U, "native-create-first");
    const auto created = take(fixture.adapter.createSession(
        request, firstContext));
    REQUIRE(created.projectId == request.projectId);
    REQUIRE(created.operationId == request.operationId);
    REQUIRE(created.predecessorSessionId == request.predecessorSessionId);
    REQUIRE(created.idempotencyKey == request.idempotencyKey);
    REQUIRE(created.providerSessionId.has_value());
    REQUIRE(created.status == Domain::HostSessionStatus::Active);
    REQUIRE(fixture.transport.createCalls() == 1U);
    REQUIRE(fixture.ledger.commitCalls() == 2U);
    const auto firstLedger = fixture.ledger.snapshot();
    REQUIRE(firstLedger.revision == 2U);
    REQUIRE(firstLedger.contentSha256.has_value());
    REQUIRE(firstLedger.records.size() == 1U);
    REQUIRE(firstLedger.records.front().ownerOperationId ==
            firstContext.operationId);
    REQUIRE(Domain::validateNativeSessionLedger(firstLedger));

    const auto replayed = take(fixture.adapter.createSession(
        request,
        operationContext(*fixture.clock, 3U, "native-create-replay")));
    REQUIRE(replayed.id == created.id);
    REQUIRE(replayed.providerSessionId == created.providerSessionId);
    REQUIRE(fixture.transport.createCalls() == 1U);
    REQUIRE(fixture.ledger.commitCalls() == 2U);

    auto wrongProject = request;
    wrongProject.projectId = parse<Domain::ProjectId>(uuidText(8'001U));
    requireError(
        fixture.adapter.createSession(
            wrongProject,
            operationContext(*fixture.clock, 4U, "native-wrong-project")),
        Domain::ErrorCodes::ProjectScopeMismatch);
    auto wrongOperation = request;
    wrongOperation.operationId =
        parse<Domain::ContinuityOperationId>(uuidText(18'002U));
    requireError(
        fixture.adapter.createSession(
            wrongOperation,
            operationContext(*fixture.clock, 5U, "native-wrong-operation")),
        Domain::ErrorCodes::IntegrityFailure);
    auto wrongPredecessor = request;
    wrongPredecessor.predecessorSessionId =
        parse<Domain::SessionId>(uuidText(38'003U));
    requireError(
        fixture.adapter.createSession(
            wrongPredecessor,
            operationContext(*fixture.clock, 6U, "native-wrong-predecessor")),
        Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(fixture.transport.createCalls() == 1U);

    const auto queried = take(fixture.adapter.queryByIdempotencyKey(
        request.projectId,
        request.idempotencyKey,
        operationContext(*fixture.clock, 7U, "native-query-key")));
    REQUIRE(queried.has_value());
    REQUIRE(queried->id == created.id);
    requireError(
        fixture.adapter.queryByIdempotencyKey(
            wrongProject.projectId,
            request.idempotencyKey,
            operationContext(*fixture.clock, 8U, "native-query-cross-project")),
        Domain::ErrorCodes::ProjectScopeMismatch);
}

void retriesAreBoundedAndCreatingIntentRecovers()
{
    {
        AdapterFixture fixture;
        fixture.transport.setRateLimitedAttempts(2U);
        const auto session = take(fixture.adapter.createSession(
            creationRequest(10U),
            operationContext(*fixture.clock, 10U, "native-retry-success")));
        REQUIRE(session.status == Domain::HostSessionStatus::Active);
        REQUIRE(fixture.transport.createCalls() ==
                SessionHost::ForgeNativeSessionHostAdapter::MaximumRetries);
    }

    AdapterFixture fixture;
    fixture.transport.setRateLimitedAttempts(3U);
    const auto request = creationRequest(11U);
    const auto failed = fixture.adapter.createSession(
        request,
        operationContext(*fixture.clock, 11U, "native-retry-exhausted"));
    requireError(failed, Domain::ErrorCodes::RateLimited);
    REQUIRE(failed.error().retryable);
    REQUIRE(fixture.transport.createCalls() ==
            SessionHost::ForgeNativeSessionHostAdapter::MaximumRetries);
    const auto intent = fixture.ledger.snapshot();
    REQUIRE(intent.records.size() == 1U);
    REQUIRE(intent.records.front().session.status ==
            Domain::HostSessionStatus::Creating);
    REQUIRE(!intent.records.front().session.providerSessionId.has_value());
    REQUIRE(intent.revision == 1U);

    fixture.transport.setRateLimitedAttempts(0U);
    const auto recovered = take(fixture.adapter.recover(
        Domain::HostRecoveryRequest{
            request.projectId,
            request.operationId,
            false},
        operationContext(*fixture.clock, 12U, "native-recover-intent")));
    REQUIRE(recovered.inspected == 1U);
    REQUIRE(recovered.recovered == 1U);
    REQUIRE(recovered.failed == 0U);
    REQUIRE(recovered.sessions.size() == 1U);
    REQUIRE(recovered.sessions.front().status ==
            Domain::HostSessionStatus::Active);
    REQUIRE(fixture.transport.createCalls() == 4U);
    const auto published = fixture.ledger.snapshot();
    REQUIRE(published.revision == 2U);
    REQUIRE(published.records.front().session.providerSessionId.has_value());
    REQUIRE(Domain::validateNativeSessionLedger(published));

    AdapterFixture orphanFixture;
    orphanFixture.transport.setRateLimitedAttempts(3U);
    const auto orphanRequest = creationRequest(12U);
    requireError(
        orphanFixture.adapter.createSession(
            orphanRequest,
            operationContext(
                *orphanFixture.clock, 13U, "native-orphan-create")),
        Domain::ErrorCodes::RateLimited);
    const auto cancelled = take(orphanFixture.adapter.recover(
        Domain::HostRecoveryRequest{
            orphanRequest.projectId,
            orphanRequest.operationId,
            true},
        operationContext(
            *orphanFixture.clock, 14U, "native-cancel-orphan")));
    REQUIRE(cancelled.inspected == 1U);
    REQUIRE(cancelled.recovered == 0U);
    REQUIRE(cancelled.cancelled == 1U);
    REQUIRE(cancelled.failed == 0U);
    REQUIRE(cancelled.sessions.size() == 1U);
    REQUIRE(cancelled.sessions.front().status ==
            Domain::HostSessionStatus::Cancelled);
    REQUIRE(orphanFixture.transport.cancelCalls() == 1U);
    REQUIRE(orphanFixture.ledger.snapshot().revision == 2U);
}

void canonicalBootstrapValidationAndExactAcknowledgement()
{
    AdapterFixture fixture;
    const auto request = creationRequest(20U);
    const auto session = take(fixture.adapter.createSession(
        request,
        operationContext(*fixture.clock, 20U, "native-bootstrap-create")));
    const auto handoff = handoffFor(
        20U, request, session, fixture.codec, *fixture.clock);

    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::MalformedJson);
    requireError(
        fixture.adapter.bootstrap(
            session,
            handoff,
            operationContext(*fixture.clock, 21U, "native-malformed-ack")),
        Domain::ErrorCodes::MalformedMessage);
    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::OversizedChunk);
    requireError(
        fixture.adapter.bootstrap(
            session,
            handoff,
            operationContext(*fixture.clock, 22U, "native-oversized-ack")),
        Domain::ErrorCodes::PayloadTooLarge);
    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::NegativeUsage);
    requireError(
        fixture.adapter.bootstrap(
            session,
            handoff,
            operationContext(*fixture.clock, 23U, "native-negative-usage")),
        Domain::ErrorCodes::MalformedMessage);
    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::WrongHandoff);
    requireError(
        fixture.adapter.bootstrap(
            session,
            handoff,
            operationContext(*fixture.clock, 24U, "native-wrong-response-handoff")),
        Domain::ErrorCodes::IntegrityFailure);
    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::WrongSession);
    requireError(
        fixture.adapter.bootstrap(
            session,
            handoff,
            operationContext(*fixture.clock, 25U, "native-wrong-response-session")),
        Domain::ErrorCodes::IntegrityFailure);

    fixture.transport.setBootstrapMode(
        ScriptedNativeTransport::BootstrapMode::Exact);
    take(fixture.adapter.bootstrap(
        session,
        handoff,
        operationContext(*fixture.clock, 26U, "native-bootstrap-exact")));
    REQUIRE(fixture.transport.bootstrapCalls() == 6U);
    const auto requestBytes = fixture.transport.lastBootstrap();
    REQUIRE(requestBytes.has_value());
    const auto canonical = take(fixture.codec.encode(
        handoff,
        operationContext(*fixture.clock, 27U, "native-bootstrap-reencode")));
    REQUIRE(requestBytes->canonicalHandoffUtf8 == canonical.canonicalUtf8);
    REQUIRE(requestBytes->handoffSha256 == handoff.contentSha256);
    REQUIRE(requestBytes->handoffId == handoff.handoffId);
    REQUIRE(requestBytes->successorSessionId == session.id);
    const auto ledger = fixture.ledger.snapshot();
    REQUIRE(ledger.records.front().session.status ==
            Domain::HostSessionStatus::Ready);
    REQUIRE(ledger.records.front().handoffId == handoff.handoffId);
    REQUIRE(ledger.records.front().handoffSha256 == handoff.contentSha256);
    REQUIRE(ledger.records.front().inputTokens == 11U);
    REQUIRE(ledger.records.front().outputTokens == 7U);

    const auto acknowledgement = take(fixture.adapter.awaitAcknowledgement(
        session,
        handoff.handoffId,
        handoff.contentSha256,
        operationContext(*fixture.clock, 28U, "native-ack-exact")));
    REQUIRE(acknowledgement.handoffId == handoff.handoffId);
    REQUIRE(acknowledgement.successorSessionId == session.id);
    REQUIRE(acknowledgement.adapterId == fixture.adapter.identifier());
    REQUIRE(acknowledgement.canonicalHandoffSha256 ==
            handoff.contentSha256);

    requireError(
        fixture.adapter.awaitAcknowledgement(
            session,
            parse<Domain::ContinuityHandoffId>(uuidText(29'001U)),
            handoff.contentSha256,
            operationContext(*fixture.clock, 29U, "native-ack-wrong-handoff")),
        Domain::ErrorCodes::AcknowledgementTimeout);
    auto wrongSession = session;
    wrongSession.id = parse<Domain::SessionId>(uuidText(39'002U));
    requireError(
        fixture.adapter.awaitAcknowledgement(
            wrongSession,
            handoff.handoffId,
            handoff.contentSha256,
            operationContext(*fixture.clock, 30U, "native-ack-wrong-session")),
        Domain::ErrorCodes::AcknowledgementTimeout);
    requireError(
        fixture.adapter.awaitAcknowledgement(
            session,
            handoff.handoffId,
            parse<Domain::Sha256Digest>(
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
            operationContext(*fixture.clock, 31U, "native-ack-wrong-hash")),
        Domain::ErrorCodes::AcknowledgementTimeout);
}

void contextCancellationAndShutdownAreFailClosed()
{
    AdapterFixture fixture;
    const auto request = creationRequest(30U);
    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        fixture.adapter.createSession(
            request,
            operationContext(
                *fixture.clock,
                30U,
                "native-cancelled-context",
                cancellation.get_token())),
        Domain::ErrorCodes::Cancelled);
    requireError(
        fixture.adapter.createSession(
            request,
            operationContext(
                *fixture.clock, 31U, "native-expired-context", {}, true)),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.transport.createCalls() == 0U);
    REQUIRE(fixture.ledger.commitCalls() == 0U);

    const auto ownerContext = operationContext(
        *fixture.clock, 32U, "native-cancel-owner");
    const auto session = take(fixture.adapter.createSession(
        request, ownerContext));
    fixture.adapter.cancel(ownerContext.operationId);
    REQUIRE(fixture.transport.cancelCalls() == 1U);
    REQUIRE(take(fixture.adapter.query(
                session.id,
                operationContext(*fixture.clock, 33U, "native-query-cancelled"))) ==
            Domain::HostSessionStatus::Cancelled);
    REQUIRE(fixture.ledger.snapshot().records.front().session.status ==
            Domain::HostSessionStatus::Cancelled);

    fixture.adapter.shutdown();
    fixture.adapter.shutdown();
    REQUIRE(fixture.transport.isShutdown());
    REQUIRE(fixture.ledger.isShutdown());
    requireError(
        fixture.adapter.capabilities(
            operationContext(*fixture.clock, 34U, "native-after-shutdown")),
        Domain::ErrorCodes::Cancelled);
}

void eightProjectsCreateConcurrently()
{
    AdapterFixture fixture{60'000U, 16U};
    constexpr std::size_t ProjectCount = 8U;
    std::array<Domain::SessionCreationRequest, ProjectCount> requests{
        creationRequest(100U),
        creationRequest(101U),
        creationRequest(102U),
        creationRequest(103U),
        creationRequest(104U),
        creationRequest(105U),
        creationRequest(106U),
        creationRequest(107U)};
    std::array<std::optional<Domain::Result<Domain::HostSession>>, ProjectCount>
        results;
    std::vector<std::jthread> workers;
    workers.reserve(ProjectCount);
    for (std::size_t index = 0U; index < ProjectCount; ++index) {
        workers.emplace_back([&, index] {
            results[index].emplace(fixture.adapter.createSession(
                requests[index],
                operationContext(
                    *fixture.clock,
                    100U + index,
                    "native-concurrent-project")));
        });
    }
    workers.clear();
    for (std::size_t index = 0U; index < ProjectCount; ++index) {
        REQUIRE(results[index].has_value());
        REQUIRE(*results[index]);
        REQUIRE(results[index]->value().projectId == requests[index].projectId);
        REQUIRE(results[index]->value().status ==
                Domain::HostSessionStatus::Active);
    }
    const auto ledger = fixture.ledger.snapshot();
    REQUIRE(ledger.records.size() == ProjectCount);
    REQUIRE(ledger.revision == ProjectCount * 2U);
    REQUIRE(fixture.ledger.commitCalls() == ProjectCount * 2U);
    REQUIRE(fixture.transport.createCalls() == ProjectCount);
    REQUIRE(Domain::validateNativeSessionLedger(ledger));
}

void localLogicalTransportReturnsAContinuationReceipt()
{
    const auto monotonic = std::chrono::steady_clock::now();
    auto clock = std::make_shared<Fakes::FakeClock>(
        Domain::UtcTimePoint{1'800'000'000s}, monotonic);
    auto hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
    InfrastructureWindows::WindowsContinuityDocumentCodec codec{hasher, clock};
    SessionHost::BoundedLogicalContinuationQueue continuationQueue{2U};
    SessionHost::LocalLogicalSessionTransport transport{
        hasher, codec, continuationQueue};
    const auto request = creationRequest(200U);
    const auto context = operationContext(
        *clock, 200U, "logical-create");
    const auto native = take(transport.createSession(request, context));
    const auto successorId = parse<Domain::SessionId>(uuidText(50'200U));
    const Domain::HostSession successor{
        successorId,
        request.projectId,
        request.operationId,
        request.predecessorSessionId,
        request.idempotencyKey,
        native.providerSessionId,
        native.model,
        Domain::HostSessionStatus::Active};
    const auto handoff = handoffFor(
        200U, request, successor, codec, *clock);
    const auto document = take(codec.encode(
        handoff,
        operationContext(*clock, 201U, "logical-canonical-handoff")));
    const Domain::NativeBootstrapRequest bootstrap{
        request.operationId,
        request.projectId,
        successorId,
        native.providerSessionId,
        handoff.handoffId,
        handoff.contentSha256,
        document.canonicalUtf8};
    const auto response = take(transport.bootstrap(
        bootstrap,
        operationContext(*clock, 202U, "logical-bootstrap")));
    REQUIRE(!response.chunks.empty());
    REQUIRE(continuationQueue.pendingCount() == 1U);

    // The continuation receipt, not the acknowledgement-shaped response,
    // proves that the canonical document was consumed and scheduled.
    const auto receipt = take(transport.continuation(
        native.providerSessionId,
        operationContext(*clock, 203U, "logical-continuation")));
    REQUIRE(receipt.has_value());
    REQUIRE(receipt->providerSessionId == native.providerSessionId);
    REQUIRE(receipt->handoffId == handoff.handoffId);
    REQUIRE(receipt->sequence == 1U);
    REQUIRE(receipt->action == "Run the scheduled continuation");
    REQUIRE(receipt->command == "forge continue --exact");
    REQUIRE(receipt->successCondition ==
            "The successor reports the next action complete");
    const auto scheduled = take(continuationQueue.takeNext(
        operationContext(*clock, 204U, "logical-take-next")));
    REQUIRE(scheduled.has_value());
    REQUIRE(scheduled->providerSessionId == receipt->providerSessionId);
    REQUIRE(scheduled->handoffId == receipt->handoffId);
    REQUIRE(scheduled->action == receipt->action);
    REQUIRE(scheduled->command == receipt->command);
    REQUIRE(scheduled->successCondition == receipt->successCondition);
    REQUIRE(continuationQueue.pendingCount() == 0U);
    REQUIRE(!take(continuationQueue.takeNext(
        operationContext(*clock, 205U, "logical-take-empty")))
                 .has_value());
    REQUIRE(take(transport.query(
                native.providerSessionId,
                operationContext(*clock, 206U, "logical-query-ready"))) ==
            Domain::HostSessionStatus::Ready);

    static_cast<void>(take(transport.bootstrap(
        bootstrap,
        operationContext(*clock, 207U, "logical-bootstrap-replay"))));
    REQUIRE(continuationQueue.pendingCount() == 0U);
    const auto replay = take(transport.continuation(
        native.providerSessionId,
        operationContext(*clock, 208U, "logical-continuation-replay")));
    REQUIRE(replay.has_value());
    REQUIRE(replay->sequence == 1U);
    REQUIRE(replay->action == receipt->action);

    auto otherHandoff = handoff;
    otherHandoff.handoffId = parse<Domain::ContinuityHandoffId>(
        uuidText(22'222U));
    const auto otherDocument = take(codec.encode(
        otherHandoff,
        operationContext(*clock, 209U, "logical-other-handoff")));
    auto conflicting = bootstrap;
    conflicting.handoffId = otherDocument.handoff.handoffId;
    conflicting.handoffSha256 = otherDocument.handoff.contentSha256;
    conflicting.canonicalHandoffUtf8 = otherDocument.canonicalUtf8;
    requireError(
        transport.bootstrap(
            conflicting,
            operationContext(*clock, 210U, "logical-conflicting-handoff")),
        Domain::ErrorCodes::Conflict);
    REQUIRE(continuationQueue.pendingCount() == 0U);

    transport.cancel(context.operationId, native.providerSessionId);
    REQUIRE(take(transport.query(
                native.providerSessionId,
                operationContext(*clock, 211U, "logical-query-cancelled"))) ==
            Domain::HostSessionStatus::Cancelled);
    transport.shutdown();
    requireError(
        transport.continuation(
            native.providerSessionId,
            operationContext(*clock, 212U, "logical-after-shutdown")),
        Domain::ErrorCodes::Cancelled);
}

void localLogicalTransportRejectsCanonicalAndBindingDefects()
{
    const auto monotonic = std::chrono::steady_clock::now();
    auto clock = std::make_shared<Fakes::FakeClock>(
        Domain::UtcTimePoint{1'800'000'000s}, monotonic);
    auto hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
    InfrastructureWindows::WindowsContinuityDocumentCodec strictCodec{
        hasher, clock};
    RecordingContinuityDocumentCodec injectedCodec{strictCodec};
    SessionHost::BoundedLogicalContinuationQueue continuationQueue{4U};
    SessionHost::LocalLogicalSessionTransport transport{
        hasher, injectedCodec, continuationQueue};
    const auto request = creationRequest(220U);
    const auto native = take(transport.createSession(
        request,
        operationContext(*clock, 220U, "logical-defect-create")));
    const auto successorId = parse<Domain::SessionId>(uuidText(50'220U));
    const Domain::HostSession successor{
        successorId,
        request.projectId,
        request.operationId,
        request.predecessorSessionId,
        request.idempotencyKey,
        native.providerSessionId,
        native.model,
        Domain::HostSessionStatus::Active};
    const auto handoff = handoffFor(
        220U, request, successor, strictCodec, *clock);
    const auto document = take(strictCodec.encode(
        handoff,
        operationContext(*clock, 221U, "logical-defect-encode")));
    const Domain::NativeBootstrapRequest bootstrap{
        request.operationId,
        request.projectId,
        successorId,
        native.providerSessionId,
        handoff.handoffId,
        handoff.contentSha256,
        document.canonicalUtf8};

    auto noncanonical = bootstrap;
    noncanonical.canonicalHandoffUtf8.insert(0U, " ");
    requireError(
        transport.bootstrap(
            noncanonical,
            operationContext(*clock, 222U, "logical-noncanonical")),
        Domain::ErrorCodes::InvalidRequest);
    REQUIRE(injectedCodec.decodeCalls() == 1U);
    REQUIRE(continuationQueue.pendingCount() == 0U);

    auto duplicateKey = bootstrap;
    duplicateKey.canonicalHandoffUtf8.insert(
        1U, "\"schema_version\":\"1.0\",");
    requireError(
        transport.bootstrap(
            duplicateKey,
            operationContext(*clock, 223U, "logical-duplicate-key")),
        Domain::ErrorCodes::InvalidRequest);
    REQUIRE(injectedCodec.decodeCalls() == 2U);
    REQUIRE(continuationQueue.pendingCount() == 0U);

    auto wrongSuccessor = bootstrap;
    wrongSuccessor.successorSessionId =
        parse<Domain::SessionId>(uuidText(50'221U));
    requireError(
        transport.bootstrap(
            wrongSuccessor,
            operationContext(*clock, 224U, "logical-wrong-successor")),
        Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(injectedCodec.decodeCalls() == 3U);
    REQUIRE(continuationQueue.pendingCount() == 0U);

    static_cast<void>(take(transport.bootstrap(
        bootstrap,
        operationContext(*clock, 225U, "logical-defect-bootstrap"))));
    REQUIRE(injectedCodec.decodeCalls() == 4U);
    REQUIRE(continuationQueue.pendingCount() == 1U);

    auto changedHandoff = handoff;
    changedHandoff.nextActions.front().action =
        "Run a different scheduled continuation";
    const auto changedDocument = take(strictCodec.encode(
        changedHandoff,
        operationContext(*clock, 226U, "logical-changed-encode")));
    REQUIRE(changedDocument.handoff.handoffId == handoff.handoffId);
    REQUIRE(changedDocument.handoff.contentSha256 != handoff.contentSha256);
    auto changedPayload = bootstrap;
    changedPayload.handoffSha256 = changedDocument.handoff.contentSha256;
    changedPayload.canonicalHandoffUtf8 = changedDocument.canonicalUtf8;
    requireError(
        transport.bootstrap(
            changedPayload,
            operationContext(*clock, 227U, "logical-changed-payload")),
        Domain::ErrorCodes::Conflict);
    REQUIRE(injectedCodec.decodeCalls() == 5U);
    REQUIRE(continuationQueue.pendingCount() == 1U);
}

void boundedLogicalQueueRejectsInvalidReplayAndShutdown()
{
    Fakes::FakeClock clock{
        Domain::UtcTimePoint{1'800'000'000s},
        std::chrono::steady_clock::now()};
    SessionHost::BoundedLogicalContinuationQueue queue{2U};
    const Domain::NativeLogicalContinuation continuation{
        parse<Domain::ProviderSessionId>("queue-provider-session"),
        parse<Domain::ContinuityHandoffId>(uuidText(24'000U)),
        1U,
        "Run the queued continuation",
        "forge continue --queued",
        "The queued continuation completes"};

    auto oversized = continuation;
    oversized.action.assign(4U * 1024U + 1U, 'x');
    requireError(
        queue.schedule(
            oversized,
            operationContext(clock, 240U, "queue-oversized")),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(queue.pendingCount() == 0U);

    auto invalidUtf8 = continuation;
    invalidUtf8.action.assign(1U, static_cast<char>(0xc3));
    requireError(
        queue.schedule(
            invalidUtf8,
            operationContext(clock, 241U, "queue-invalid-utf8")),
        Domain::ErrorCodes::InvalidRequest);
    REQUIRE(queue.pendingCount() == 0U);

    auto invalidSequence = continuation;
    invalidSequence.sequence = 0U;
    requireError(
        queue.schedule(
            invalidSequence,
            operationContext(clock, 242U, "queue-invalid-sequence")),
        Domain::ErrorCodes::InvalidRequest);
    REQUIRE(queue.pendingCount() == 0U);

    take(queue.schedule(
        continuation,
        operationContext(clock, 243U, "queue-first")));
    take(queue.schedule(
        continuation,
        operationContext(clock, 244U, "queue-exact-replay")));
    REQUIRE(queue.pendingCount() == 1U);

    auto changed = continuation;
    changed.command = "forge continue --changed";
    requireError(
        queue.schedule(
            changed,
            operationContext(clock, 245U, "queue-changed-replay")),
        Domain::ErrorCodes::Conflict);
    REQUIRE(queue.pendingCount() == 1U);

    const auto accepted = take(queue.takeNext(
        operationContext(clock, 246U, "queue-take")));
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->providerSessionId == continuation.providerSessionId);
    REQUIRE(accepted->handoffId == continuation.handoffId);
    REQUIRE(accepted->action == continuation.action);
    REQUIRE(queue.pendingCount() == 0U);

    take(queue.schedule(
        continuation,
        operationContext(clock, 247U, "queue-replay-after-take")));
    REQUIRE(queue.pendingCount() == 0U);
    REQUIRE(!take(queue.takeNext(
        operationContext(clock, 248U, "queue-empty-after-replay")))
                 .has_value());

    queue.shutdown();
    requireError(
        queue.schedule(
            continuation,
            operationContext(clock, 249U, "queue-after-shutdown")),
        Domain::ErrorCodes::Cancelled);
    requireError(
        queue.takeNext(
            operationContext(clock, 250U, "queue-take-after-shutdown")),
        Domain::ErrorCodes::Cancelled);
}

void logicalTransportShutdownWaitsForInFlightBootstrap()
{
    const auto monotonic = std::chrono::steady_clock::now();
    auto clock = std::make_shared<Fakes::FakeClock>(
        Domain::UtcTimePoint{1'800'000'000s}, monotonic);
    auto hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
    InfrastructureWindows::WindowsContinuityDocumentCodec codec{hasher, clock};
    BlockingContinuationScheduler scheduler;
    SessionHost::LocalLogicalSessionTransport transport{
        hasher, codec, scheduler};
    const auto request = creationRequest(260U);
    const auto native = take(transport.createSession(
        request,
        operationContext(*clock, 260U, "logical-shutdown-create")));
    const auto successorId = parse<Domain::SessionId>(uuidText(50'260U));
    const Domain::HostSession successor{
        successorId,
        request.projectId,
        request.operationId,
        request.predecessorSessionId,
        request.idempotencyKey,
        native.providerSessionId,
        native.model,
        Domain::HostSessionStatus::Active};
    const auto handoff = handoffFor(
        260U, request, successor, codec, *clock);
    const auto document = take(codec.encode(
        handoff,
        operationContext(*clock, 261U, "logical-shutdown-encode")));
    const Domain::NativeBootstrapRequest bootstrap{
        request.operationId,
        request.projectId,
        successorId,
        native.providerSessionId,
        handoff.handoffId,
        handoff.contentSha256,
        document.canonicalUtf8};

    std::optional<Domain::Result<Domain::NativeBootstrapResponse>> result;
    std::atomic_bool bootstrapReturned{};
    std::jthread bootstrapWorker{[&] {
        result.emplace(transport.bootstrap(
            bootstrap,
            operationContext(*clock, 262U, "logical-blocked-bootstrap")));
        bootstrapReturned.store(true, std::memory_order_release);
    }};
    const auto entered = scheduler.waitUntilScheduleEntered(5s);

    std::atomic_bool shutdownInvoked{};
    std::atomic_bool shutdownReturned{};
    std::jthread shutdownWorker{[&] {
        shutdownInvoked.store(true, std::memory_order_release);
        transport.shutdown();
        shutdownReturned.store(true, std::memory_order_release);
    }};

    bool observedShutdownCancellation{};
    const auto observationDeadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < observationDeadline) {
        if (!shutdownInvoked.load(std::memory_order_acquire)) {
            std::this_thread::yield();
            continue;
        }
        const auto status = transport.query(
            native.providerSessionId,
            operationContext(*clock, 263U, "logical-shutdown-probe"));
        if (!status && status.error().code == Domain::ErrorCodes::Cancelled) {
            observedShutdownCancellation = true;
            break;
        }
        std::this_thread::yield();
    }

    const auto acceptedBeforeRelease = scheduler.hasAcceptedWork();
    const auto schedulerShutdownBeforeRelease = scheduler.shutdownCalls();
    const auto bootstrapReturnedBeforeRelease =
        bootstrapReturned.load(std::memory_order_acquire);
    const auto shutdownReturnedBeforeRelease =
        shutdownReturned.load(std::memory_order_acquire);
    scheduler.release();
    bootstrapWorker.join();
    shutdownWorker.join();

    REQUIRE(entered);
    REQUIRE(observedShutdownCancellation);
    REQUIRE(acceptedBeforeRelease);
    REQUIRE(schedulerShutdownBeforeRelease == 0U);
    REQUIRE(!bootstrapReturnedBeforeRelease);
    REQUIRE(!shutdownReturnedBeforeRelease);
    REQUIRE(result.has_value());
    requireError(*result, Domain::ErrorCodes::Cancelled);
    REQUIRE(scheduler.scheduleCalls() == 1U);
    REQUIRE(scheduler.cancelCalls() == 1U);
    REQUIRE(scheduler.shutdownCalls() == 1U);
    REQUIRE(shutdownReturned.load(std::memory_order_acquire));
    REQUIRE(!scheduler.hasAcceptedWork());
}

} // namespace

int main()
{
    try {
        inMemoryLedgerUsesRevisionCasAndCommittedDigest();
        std::cout << "PASS native_session_host.ledger_revision_cas\n";
        exactCapabilitiesManifestAndHealth();
        std::cout << "PASS native_session_host.capabilities_manifest_health\n";
        createIntentReplayAndBindingConflicts();
        std::cout << "PASS native_session_host.create_replay_binding\n";
        retriesAreBoundedAndCreatingIntentRecovers();
        std::cout << "PASS native_session_host.retry_recovery\n";
        canonicalBootstrapValidationAndExactAcknowledgement();
        std::cout << "PASS native_session_host.bootstrap_ack_bounds\n";
        contextCancellationAndShutdownAreFailClosed();
        std::cout << "PASS native_session_host.context_shutdown\n";
        eightProjectsCreateConcurrently();
        std::cout << "PASS native_session_host.concurrent_projects\n";
        localLogicalTransportReturnsAContinuationReceipt();
        std::cout << "PASS native_session_host.logical_continuation_receipt\n";
        localLogicalTransportRejectsCanonicalAndBindingDefects();
        std::cout << "PASS native_session_host.logical_binding_defects\n";
        boundedLogicalQueueRejectsInvalidReplayAndShutdown();
        std::cout << "PASS native_session_host.logical_queue_contract\n";
        logicalTransportShutdownWaitsForInFlightBootstrap();
        std::cout << "PASS native_session_host.logical_shutdown_race\n";
        std::cout << "SUMMARY passed=11 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
