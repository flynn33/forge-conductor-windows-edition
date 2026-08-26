#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "ForgeConductor/Application/ContinuityAutomation.h"
#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"
#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"
#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"
#include "Fakes/ContinuityRepositoryFake.h"
#include "Fakes/ProjectRepositoryFakes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
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

[[nodiscard]] std::string uuidText(const std::uint64_t value)
{
    std::ostringstream stream;
    stream << "a1200000-0000-4000-8000-" << std::hex << std::nouppercase
           << std::setfill('0') << std::setw(12) << value;
    return stream.str();
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Contracts::IClock& clock,
    const std::uint64_t identifier,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(uuidText(90'000U + identifier)),
        clock.monotonicNow() + 5min,
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    REQUIRE(required > 0);
    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        converted.data(),
        required,
        nullptr,
        nullptr);
    REQUIRE(written == required);
    return converted;
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(buffer.size()), buffer.data());
        REQUIRE(length > 0U && length < buffer.size());
        const std::filesystem::path root{
            std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        for (std::uint64_t attempt = 0U; attempt < 32U; ++attempt) {
            path_ = root /
                (L"forge-g12-native-e2e-" +
                 std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(::GetCurrentThreadId()) + L"-" +
                 std::to_wstring(::GetTickCount64()) + L"-" +
                 std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw std::runtime_error{
            "Could not create the isolated G12 native-session directory."};
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::AuthorizedPath issue(
        const Domain::PathText& root,
        const Domain::PathText& target,
        const Domain::FileAccess access)
    {
        auto authority = take(issueAuthority(
            parse<Domain::AuthorityId>(
                "a1211111-1111-4111-8111-111111111111"),
            parse<Domain::ProjectId>(
                "a1222222-2222-4222-8222-222222222222"),
            parse<Domain::ClientId>("g12-native-session-e2e"),
            std::vector<Domain::PathText>{root},
            Domain::FileAccess::Read,
            std::vector<Domain::FileAccess>{
                Domain::FileAccess::Read,
                Domain::FileAccess::Write,
                Domain::FileAccess::Create},
            {},
            false,
            1U));
        return take(issueAuthorizedPath(authority, target, root, access));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::WorkspaceAuthority>();
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        const bool,
        const std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::WorkspaceAuthority>();
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::AuthorizedPath>();
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unsupported()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The G12 test capability issuer has no runtime authority service."));
    }
};

class LedgerPaths final {
public:
    explicit LedgerPaths(const std::filesystem::path& directory)
        : root{take(Domain::PathText::create(utf8(directory.native())))},
          primary{take(Domain::PathText::create(
              utf8((directory / L"native-session-ledger.json").native())))},
          backup{take(Domain::PathText::create(
              utf8((directory / L"native-session-ledger.json.bak").native())))},
          read{CapabilityIssuer::issue(
              root, primary, Domain::FileAccess::Read)},
          write{CapabilityIssuer::issue(
              root, primary, Domain::FileAccess::Write)},
          create{CapabilityIssuer::issue(
              root, primary, Domain::FileAccess::Create)},
          backupRead{CapabilityIssuer::issue(
              root, backup, Domain::FileAccess::Read)}
    {
    }

    Domain::PathText root;
    Domain::PathText primary;
    Domain::PathText backup;
    Contracts::AuthorizedPath read;
    Contracts::AuthorizedPath write;
    Contracts::AuthorizedPath create;
    Contracts::AuthorizedPath backupRead;
};

// The deterministic P11 repository fake cannot re-encode a handoff when it
// binds the successor. Reserve a real Windows UUID and replay it once through
// the injected allocation seam so the pre-bound canonical handoff stays exact.
class ReservedUuidGenerator final : public Contracts::IUuidGenerator {
public:
    explicit ReservedUuidGenerator(Domain::Uuid reserved)
        : reserved_{std::move(reserved)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        if (!reserved_) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The reserved G12 successor UUID was already consumed."));
        }
        auto value = *reserved_;
        reserved_.reset();
        return Domain::Result<Domain::Uuid>::success(std::move(value));
    }

    [[nodiscard]] bool consumed() const noexcept
    {
        return !reserved_.has_value();
    }

private:
    std::optional<Domain::Uuid> reserved_;
};

class SingleContinuityRepositoryFactory final
    : public Contracts::IContinuityRepositoryFactory {
public:
    explicit SingleContinuityRepositoryFactory(
        std::shared_ptr<Fakes::ContinuityRepositoryFake> repository)
        : repository_{std::move(repository)}
    {
    }

    [[nodiscard]] Domain::Result<
        std::shared_ptr<Contracts::IContinuityRepository>>
    openContinuity(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (shutdown_ || context.isCancellationRequested()) {
                return failure(
                    Domain::ErrorCodes::Cancelled,
                    "The G12 continuity repository factory is closed.");
            }
            if (projectId != repository_->projectId()) {
                return failure(
                    Domain::ErrorCodes::ProjectNotFound,
                    "The G12 project is not registered.");
            }
            opened_ = true;
            std::shared_ptr<Contracts::IContinuityRepository> result{
                repository_};
            return Domain::Result<
                std::shared_ptr<Contracts::IContinuityRepository>>::success(
                std::move(result));
        } catch (...) {
            return failure(
                Domain::ErrorCodes::InternalFailure,
                "The G12 continuity repository could not be opened.");
        }
    }

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        opened_ = false;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::size_t openCount() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return opened_ ? 1U : 0U;
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
        opened_ = false;
    }

private:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<Contracts::IContinuityRepository>>
    failure(const std::string_view code, std::string message)
    {
        return Domain::Result<
            std::shared_ptr<Contracts::IContinuityRepository>>::failure(
            Domain::makeError(code, std::move(message)));
    }

    std::shared_ptr<Fakes::ContinuityRepositoryFake> repository_;
    mutable std::mutex mutex_;
    bool opened_{};
    bool shutdown_{};
};

[[nodiscard]] Domain::ContextBudgetSignals rolloverSignals()
{
    return Domain::ContextBudgetSignals{
        10'000U,
        1'000U,
        std::nullopt,
        std::optional<std::uint64_t>{8'500U},
        std::nullopt,
        std::nullopt,
        false};
}

[[nodiscard]] Domain::ContinuityHandoff canonicalHandoff(
    const Domain::ProjectId& projectId,
    const Domain::SessionId& successorId,
    const Domain::PathText& projectRoot,
    const Domain::AdapterId& adapterId,
    InfrastructureWindows::WindowsContinuityDocumentCodec& codec,
    const Contracts::IClock& clock)
{
    Domain::ContinuityHandoff handoff{
        parse<Domain::ContinuityHandoffId>(uuidText(20'001U)),
        parse<Domain::ContinuityOperationId>(uuidText(10'001U)),
        clock.utcNow(),
        Domain::ContinuityProject{
            projectId,
            "G12 native continuity project",
            projectRoot,
            "main",
            "0123456789abcdef",
            {}},
        Domain::ContinuitySession{
            parse<Domain::SessionId>(uuidText(30'001U)),
            std::nullopt,
            std::optional<std::string>{"predecessor-model"},
            std::optional<std::string>{"local"}},
        Domain::ContinuitySession{
            successorId, std::nullopt, std::nullopt, std::nullopt},
        "G12-SENSITIVE-MISSION: resume the exact durable native successor",
        {"Create and activate exactly one successor"},
        Domain::ContinuityCurrentWork{
            "P12",
            "native-session-continuity-e2e",
            "Drive autonomous rollover from raw context signals",
            {take(Domain::PathText::create(
                "tests/SessionHost/NativeSessionContinuityEndToEndTests.cpp"))}},
        {{std::optional<std::string>{"checkpoint"},
          "The canonical handoff is ready",
          std::optional<std::string>{"complete"}}},
        {{std::optional<std::string>{"continue"},
          "Consume the successor continuation",
          std::optional<std::string>{"open"}}},
        {{"Use the durable native ledger for restart reconciliation",
          std::optional<std::string>{"Prevent duplicate successors"}}},
        Domain::ContinuityValidation{{"G11"}, {"G12"}, {}},
        {},
        {},
        {{1U,
          "Run the exact scheduled G12 continuation",
          "forge continue --g12-exact",
          "The successor consumes the continuation once"}},
        Domain::ContinuityHostState{
            adapterId,
            Domain::ContinuityState::Idle,
            "provider_usage",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        true};
    return take(codec.encode(
        handoff,
        operationContext(clock, 1U, "g12-canonical-handoff")))
        .handoff;
}

void requireCompletedBindings(
    Fakes::ContinuityRepositoryFake& repository,
    const Domain::ContinuityHandoff& handoff,
    const Domain::SessionId& successorId,
    const Domain::AdapterId& adapterId,
    const Domain::OperationContext& context)
{
    REQUIRE(repository.storedOperation().has_value());
    const auto& operation = *repository.storedOperation();
    REQUIRE(operation.projectId == handoff.project.projectId);
    REQUIRE(operation.operationId == handoff.operationId);
    REQUIRE(operation.predecessorSessionId ==
            handoff.predecessorSession.sessionId);
    REQUIRE(operation.handoffId == handoff.handoffId);
    REQUIRE(operation.adapterId == adapterId);
    REQUIRE(operation.state == Domain::ContinuityState::Completed);
    REQUIRE(operation.successorSessionId == successorId);
    REQUIRE(operation.acknowledgedSessionId == successorId);
    REQUIRE(operation.acknowledgedHandoffId == handoff.handoffId);
    REQUIRE(repository.storedHandoff().has_value());
    REQUIRE(repository.storedHandoff()->successorSession.has_value());
    REQUIRE(repository.storedHandoff()->successorSession->sessionId ==
            successorId);
    REQUIRE(repository.storedHandoff()->contentSha256 == handoff.contentSha256);
    REQUIRE(repository.lastAcknowledgement().has_value());
    REQUIRE(repository.lastAcknowledgement()->handoffId == handoff.handoffId);
    REQUIRE(repository.lastAcknowledgement()->successorSessionId == successorId);
    REQUIRE(repository.lastAcknowledgement()->adapterId == adapterId);
    REQUIRE(repository.lastAcknowledgement()->canonicalHandoffSha256 ==
            handoff.contentSha256);
    REQUIRE(take(repository.activeSession(
                handoff.project.projectId, context)) == successorId);
    REQUIRE(take(repository.transitionCount(
                handoff.operationId, context)) == 9U);
}

[[nodiscard]] const Domain::NativeSessionRecord& requireNativeBinding(
    const Domain::NativeSessionLedger& ledger,
    const Domain::ContinuityHandoff& handoff,
    const Domain::SessionId& successorId)
{
    take(Domain::validateNativeSessionLedger(ledger));
    REQUIRE(ledger.records.size() == 1U);
    const auto& record = ledger.records.front();
    REQUIRE(record.session.id == successorId);
    REQUIRE(record.session.projectId == handoff.project.projectId);
    REQUIRE(record.session.operationId == handoff.operationId);
    REQUIRE(record.session.predecessorSessionId ==
            handoff.predecessorSession.sessionId);
    REQUIRE(record.session.idempotencyKey.value() ==
            handoff.operationId.value());
    REQUIRE(record.session.providerSessionId.has_value());
    REQUIRE(record.session.status == Domain::HostSessionStatus::Ready);
    REQUIRE(record.handoffId == handoff.handoffId);
    REQUIRE(record.handoffSha256 == handoff.contentSha256);
    REQUIRE(record.inputTokens > 0U);
    REQUIRE(record.outputTokens == 16U);
    return record;
}

void requireContinuation(
    const Domain::NativeLogicalContinuation& continuation,
    const Domain::ProviderSessionId& providerSessionId,
    const Domain::ContinuityHandoff& handoff)
{
    REQUIRE(continuation.providerSessionId == providerSessionId);
    REQUIRE(continuation.handoffId == handoff.handoffId);
    REQUIRE(continuation.sequence == 1U);
    REQUIRE(continuation.action ==
            "Run the exact scheduled G12 continuation");
    REQUIRE(continuation.command == "forge continue --g12-exact");
    REQUIRE(continuation.successCondition ==
            "The successor consumes the continuation once");
}

[[nodiscard]] std::string text(const std::span<const std::byte> content)
{
    if (content.empty()) {
        return {};
    }
    return std::string{
        reinterpret_cast<const char*>(content.data()), content.size()};
}

void autonomousNativeRolloverSurvivesRestartWithoutDuplication()
{
    TemporaryDirectory temporary;
    LedgerPaths paths{temporary.path()};
    auto hasher =
        std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
    auto clock = std::make_shared<InfrastructureWindows::SystemClock>();
    InfrastructureWindows::WindowsContinuityDocumentCodec codec{hasher, clock};
    InfrastructureWindows::WindowsUuidGenerator windowsUuid;
    const auto reservedUuid = take(windowsUuid.next());
    const Domain::SessionId successorId{reservedUuid};
    const auto adapterId = parse<Domain::AdapterId>(
        SessionHost::ForgeNativeSessionHostAdapter::AdapterIdentifier);
    const auto projectId = parse<Domain::ProjectId>(uuidText(1'001U));
    const auto handoff = canonicalHandoff(
        projectId, successorId, paths.root, adapterId, codec, *clock);
    auto repository = std::make_shared<Fakes::ContinuityRepositoryFake>(
        projectId, clock->monotonicNow());
    SingleContinuityRepositoryFactory repositoryFactory{repository};
    Fakes::ProjectRegistryRepositoryFake projectRegistry{
        4U, clock->monotonicNow()};
    take(projectRegistry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        projectId,
        handoff.project.displayName,
        std::optional<std::string>{"g12-native-continuity"},
        {handoff.project.repositoryRoot}}));

    std::optional<Domain::ProviderSessionId> providerSessionId;
    std::uint64_t durableRevision{};
    {
        InfrastructureWindows::WindowsAtomicFileStore atomicStore;
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            atomicStore,
            *hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        SessionHost::BoundedLogicalContinuationQueue continuationQueue{4U};
        SessionHost::LocalLogicalSessionTransport transport{
            hasher, codec, continuationQueue};
        ReservedUuidGenerator uuidGenerator{reservedUuid};
        SessionHost::ForgeNativeSessionHostAdapter adapter{
            adapterId,
            ledger,
            transport,
            codec,
            uuidGenerator,
            *clock};
        Application::ContinuityCoordinator coordinator{
            projectRegistry,
            repositoryFactory,
            adapter,
            *clock};
        Application::ContinuityAutomation automation{coordinator, *clock};
        const auto context = operationContext(
            *clock, 10U, "g12-autonomous-rollover");
        const auto outcome = take(automation.observe(
            Domain::ContinuityAutomationObservation{
                handoff, rolloverSignals(), 0U, false},
            context));

        REQUIRE(outcome.action == Domain::ContextBudgetAction::Rollover);
        REQUIRE(outcome.projectId == projectId);
        REQUIRE(outcome.handoffId == handoff.handoffId);
        REQUIRE(outcome.operationId == handoff.operationId);
        REQUIRE(outcome.successorSessionId == successorId);
        REQUIRE(outcome.checkpointPersisted);
        REQUIRE(outcome.rolloverRequested);
        REQUIRE(outcome.successorActivated);
        REQUIRE(uuidGenerator.consumed());
        requireCompletedBindings(
            *repository, handoff, successorId, adapterId, context);

        const auto loaded = take(ledger.load(context));
        const auto& record = requireNativeBinding(
            loaded, handoff, successorId);
        providerSessionId = *record.session.providerSessionId;
        durableRevision = loaded.revision;
        REQUIRE(durableRevision >= 4U);
        REQUIRE(take(adapter.query(successorId, context)) ==
                Domain::HostSessionStatus::Ready);
        const auto byKey = take(adapter.queryByIdempotencyKey(
            projectId, record.session.idempotencyKey, context));
        REQUIRE(byKey.has_value());
        REQUIRE(byKey->id == successorId);

        REQUIRE(continuationQueue.pendingCount() == 1U);
        const auto continuation = take(continuationQueue.takeNext(context));
        REQUIRE(continuation.has_value());
        requireContinuation(*continuation, *providerSessionId, handoff);
        REQUIRE(continuationQueue.pendingCount() == 0U);
        REQUIRE(!take(continuationQueue.takeNext(context)).has_value());

        const auto encodedLedger = text(take(atomicStore.read(
            paths.read,
            InfrastructureWindows::WindowsNativeSessionLedger::
                MaximumDocumentBytes,
            context)));
        REQUIRE(encodedLedger.find("G12-SENSITIVE-MISSION") ==
                std::string::npos);
        REQUIRE(encodedLedger.find("forge continue --g12-exact") ==
                std::string::npos);
    }

    REQUIRE(providerSessionId.has_value());
    {
        InfrastructureWindows::WindowsAtomicFileStore atomicStore;
        InfrastructureWindows::WindowsNativeSessionLedger ledger{
            atomicStore,
            *hasher,
            paths.read,
            paths.write,
            paths.create,
            paths.backupRead};
        SessionHost::BoundedLogicalContinuationQueue continuationQueue{4U};
        SessionHost::LocalLogicalSessionTransport transport{
            hasher, codec, continuationQueue};
        InfrastructureWindows::WindowsUuidGenerator uuidGenerator;
        SessionHost::ForgeNativeSessionHostAdapter adapter{
            adapterId,
            ledger,
            transport,
            codec,
            uuidGenerator,
            *clock};
        Application::ContinuityCoordinator coordinator{
            projectRegistry,
            repositoryFactory,
            adapter,
            *clock};
        Application::ContinuityAutomation automation{coordinator, *clock};
        const auto context = operationContext(
            *clock, 20U, "g12-restart-recovery");

        const auto reloaded = take(ledger.load(context));
        const auto& reloadedRecord = requireNativeBinding(
            reloaded, handoff, successorId);
        REQUIRE(reloaded.revision == durableRevision);
        REQUIRE(reloadedRecord.session.providerSessionId == providerSessionId);
        const auto creation = Domain::SessionCreationRequest{
            handoff.operationId,
            handoff.project.projectId,
            handoff.predecessorSession.sessionId,
            reloadedRecord.session.idempotencyKey};
        const auto reconciled = take(adapter.createSession(creation, context));
        REQUIRE(reconciled.id == successorId);
        REQUIRE(reconciled.providerSessionId == providerSessionId);
        const auto afterCreateReplay = take(ledger.load(context));
        REQUIRE(afterCreateReplay.revision == durableRevision);
        REQUIRE(afterCreateReplay.records.size() == 1U);

        const auto replay = take(automation.observe(
            Domain::ContinuityAutomationObservation{
                handoff, rolloverSignals(), 0U, false},
            context));
        REQUIRE(replay.action == Domain::ContextBudgetAction::Rollover);
        REQUIRE(replay.operationId == handoff.operationId);
        REQUIRE(replay.successorSessionId == successorId);
        REQUIRE(replay.checkpointPersisted);
        REQUIRE(replay.rolloverRequested);
        REQUIRE(replay.successorActivated);
        requireCompletedBindings(
            *repository, handoff, successorId, adapterId, context);

        const auto recoveredLedger = take(ledger.load(context));
        const auto& recoveredRecord = requireNativeBinding(
            recoveredLedger, handoff, successorId);
        REQUIRE(recoveredLedger.records.size() == 1U);
        REQUIRE(recoveredRecord.session.providerSessionId == providerSessionId);
        REQUIRE(continuationQueue.pendingCount() == 1U);
        const auto continuation = take(continuationQueue.takeNext(context));
        REQUIRE(continuation.has_value());
        requireContinuation(*continuation, *providerSessionId, handoff);
        REQUIRE(continuationQueue.pendingCount() == 0U);
        REQUIRE(!take(continuationQueue.takeNext(context)).has_value());

        const auto resumed = take(coordinator.resume(
            Domain::HandoffResumeRequest{
                projectId, handoff.handoffId, successorId},
            context));
        REQUIRE(resumed.operation.state == Domain::ContinuityState::Completed);
        REQUIRE(resumed.operation.successorSessionId == successorId);
        REQUIRE(resumed.operation.acknowledgedSessionId == successorId);
        REQUIRE(resumed.operation.acknowledgedHandoffId == handoff.handoffId);
        REQUIRE(resumed.handoff.contentSha256 == handoff.contentSha256);
        REQUIRE(resumed.session.id == successorId);
        REQUIRE(resumed.session.status == Domain::HostSessionStatus::Ready);
        REQUIRE(continuationQueue.pendingCount() == 0U);
        REQUIRE(!take(continuationQueue.takeNext(context)).has_value());

        const auto hostRecovery = take(adapter.recover(
            Domain::HostRecoveryRequest{
                std::optional<Domain::ProjectId>{projectId},
                std::optional<Domain::ContinuityOperationId>{
                    handoff.operationId},
                false},
            context));
        REQUIRE(hostRecovery.inspected == 1U);
        REQUIRE(hostRecovery.recovered == 1U);
        REQUIRE(hostRecovery.failed == 0U);
        REQUIRE(hostRecovery.sessions.size() == 1U);
        REQUIRE(hostRecovery.sessions.front().id == successorId);
        const auto coordinatorRecovery = take(
            coordinator.recoverIncompleteOperations(
                Domain::ContinuityRecoveryRequest{
                    std::optional<Domain::ProjectId>{projectId}, true},
                context));
        REQUIRE(coordinatorRecovery.inspected == 0U);
        REQUIRE(coordinatorRecovery.resumed == 0U);
        REQUIRE(coordinatorRecovery.failed == 0U);
        const auto finalLedger = take(ledger.load(context));
        REQUIRE(finalLedger.records.size() == 1U);
        REQUIRE(finalLedger.records.front().session.id == successorId);
        REQUIRE(finalLedger.records.front().session.providerSessionId ==
                providerSessionId);
    }
}

} // namespace

int main()
{
    try {
        autonomousNativeRolloverSurvivesRestartWithoutDuplication();
        std::cout << "PASS g12.native_continuity_end_to_end\n";
        std::cout << "PASS " << assertionCount.load()
                  << " assertions\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
