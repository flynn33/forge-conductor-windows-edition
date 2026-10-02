#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Persistence/PersistenceTestSupport.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace Support = ForgeConductor::Tests::PersistenceSupport;

constexpr DWORD CrashExitCode = 197U;
constexpr auto ChildTimeout = 30s;
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
    stream << "20000000-0000-4000-8000-" << std::hex << std::nouppercase
           << std::setfill('0') << std::setw(12) << value;
    return stream.str();
}

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_{handle} {}
    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    void reset() noexcept
    {
        if (*this) {
            static_cast<void>(CloseHandle(handle_));
        }
        handle_ = nullptr;
    }

    HANDLE handle_{};
};

enum class CrashBoundary : std::uint32_t {
    CheckpointIntent,
    CheckpointPersisted,
    SuccessorCreateIntent,
    CreateEffectBeforeCommit,
    SuccessorCommit,
    BootstrapIntent,
    BootstrapEffectBeforeAcknowledgement,
    AcknowledgementCommit,
    PredecessorSealIntent,
    CompletedPointerCommit,
    Count
};

[[nodiscard]] std::string_view boundaryName(const CrashBoundary boundary) noexcept
{
    switch (boundary) {
    case CrashBoundary::CheckpointIntent:
        return "checkpoint_intent";
    case CrashBoundary::CheckpointPersisted:
        return "checkpoint_persisted";
    case CrashBoundary::SuccessorCreateIntent:
        return "successor_create_intent";
    case CrashBoundary::CreateEffectBeforeCommit:
        return "create_effect_before_commit";
    case CrashBoundary::SuccessorCommit:
        return "successor_commit";
    case CrashBoundary::BootstrapIntent:
        return "bootstrap_intent";
    case CrashBoundary::BootstrapEffectBeforeAcknowledgement:
        return "bootstrap_effect_before_acknowledgement";
    case CrashBoundary::AcknowledgementCommit:
        return "acknowledgement_commit";
    case CrashBoundary::PredecessorSealIntent:
        return "predecessor_seal_intent";
    case CrashBoundary::CompletedPointerCommit:
        return "completed_pointer_commit";
    case CrashBoundary::Count:
        return "count";
    }
    return "unknown";
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Contracts::IClock& clock,
    const std::uint64_t identifier,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(uuidText(50'000U + identifier)),
        clock.monotonicNow() + 5min,
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] std::vector<Domain::Uuid> uuidSequence()
{
    std::vector<Domain::Uuid> values;
    values.reserve(32U);
    for (std::uint64_t value = 1U; value <= 32U; ++value) {
        values.push_back(parse<Domain::Uuid>(uuidText(80'000U + value)));
    }
    return values;
}

class CrashSignal final {
public:
    CrashSignal(
        const CrashBoundary selected,
        const std::wstring_view eventName)
        : selected_{selected},
          event_{OpenEventW(
              EVENT_MODIFY_STATE | SYNCHRONIZE,
              FALSE,
              std::wstring{eventName}.c_str())}
    {
        if (!event_) {
            throw std::runtime_error{"The parent crash event could not be opened."};
        }
    }

    void reach(const CrashBoundary boundary) const noexcept
    {
        if (boundary != selected_) {
            return;
        }
        if (SetEvent(event_.get()) == FALSE) {
            TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
        }
        Sleep(INFINITE);
        TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    }

private:
    CrashBoundary selected_;
    UniqueHandle event_;
};

[[nodiscard]] Domain::ContinuityHandoff rawHandoff(
    const std::uint64_t identifier,
    const Domain::UtcTimePoint createdAt,
    const Domain::AdapterId& adapterId)
{
    return Domain::ContinuityHandoff{
        parse<Domain::ContinuityHandoffId>(uuidText(20'000U + identifier)),
        parse<Domain::ContinuityOperationId>(uuidText(10'000U + identifier)),
        createdAt,
        Domain::ContinuityProject{
            parse<Domain::ProjectId>(uuidText(1'000U + identifier)),
            "Abrupt recovery project " + std::to_string(identifier),
            take(Domain::PathText::create(
                "D:/continuity/process-project-" + std::to_string(identifier))),
            "main",
            "fedcba9876543210",
            {}},
        Domain::ContinuitySession{
            parse<Domain::SessionId>(uuidText(30'000U + identifier)),
            std::nullopt,
            std::optional<std::string>{"process-model"},
            std::optional<std::string>{"process-provider"}},
        std::nullopt,
        "Recover after an abrupt native process termination",
        {"One physical successor", "One distinct bootstrap"},
        Domain::ContinuityCurrentWork{
            "P11",
            "abrupt-process-fixture",
            "Terminate at a committed continuity boundary",
            {take(Domain::PathText::create(
                "tests/Continuity/ContinuityCoordinatorProcessFixture.cpp"))}},
        {{std::optional<std::string>{"checkpoint"},
          "Prepared canonical handoff",
          std::optional<std::string>{"complete"}}},
        {{std::optional<std::string>{"recovery"},
          "Restart and complete the rollover",
          std::optional<std::string>{"open"}}},
        {{"Persist every intent before its effect", std::nullopt}},
        Domain::ContinuityValidation{{"G10"}, {"G11"}, {}},
        {},
        {},
        {{1U, "Restart recovery", "", "The active pointer is committed"}},
        Domain::ContinuityHostState{
            adapterId,
            Domain::ContinuityState::Idle,
            "abrupt-process-test",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(std::string(64U, '0')),
        true};
}

void appendChildProgress(
    const std::filesystem::path& root,
    std::string_view step);

struct RepositoryRuntime final {
    RepositoryRuntime(
        const std::filesystem::path& root,
        const std::uint64_t identifier)
        : projectId{parse<Domain::ProjectId>(uuidText(1'000U + identifier))}
    {
        // Native file-lock and SQLite boundaries compare the operation deadline
        // with the process steady clock, so this test clock must share that epoch.
        const auto monotonic = std::chrono::steady_clock::now();
        appendChildProgress(root, "repository-runtime-entered");
        paths = std::make_shared<Fakes::RecordingApplicationPathsFake>();
        paths->setNow(monotonic);
        paths->projectRootResult.set(
            Domain::Result<Domain::PathText>::success(Support::pathText(root)));
        appendChildProgress(root, "repository-paths-ready");
        diagnostics = std::make_shared<Fakes::RuntimeDiagnosticsFake>(monotonic);
        clock = std::make_shared<Support::FixedClock>(
            Domain::UtcTimePoint{std::chrono::seconds{1'700'200'000}},
            monotonic);
        appendChildProgress(root, "repository-clock-ready");
        redactor = std::make_shared<InfrastructureWindows::SecretRedactor>();
        hasher = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
        codec = std::make_shared<
            InfrastructureWindows::WindowsContinuityDocumentCodec>(hasher, clock);
        uuidGenerator = std::make_shared<Fakes::SequenceUuidGenerator>(
            uuidSequence());
        appendChildProgress(root, "repository-dependencies-ready");
        repository = take(PersistenceWindows::WindowsProjectMemoryRepository::open(
            projectId,
            paths,
            diagnostics,
            redactor,
            hasher,
            uuidGenerator,
            clock,
            {},
            operationContext(*clock, identifier, "process-repository-open")));
        appendChildProgress(root, "repository-open-call-returned");
    }

    [[nodiscard]] Domain::ContinuityHandoff handoff(
        const std::uint64_t identifier,
        const Domain::AdapterId& adapterId) const
    {
        return take(codec->encode(
            rawHandoff(identifier, clock->utcNow(), adapterId),
            operationContext(*clock, identifier + 100U, "process-handoff-encode")))
            .handoff;
    }

    Domain::ProjectId projectId;
    std::shared_ptr<Fakes::RecordingApplicationPathsFake> paths;
    std::shared_ptr<Fakes::RuntimeDiagnosticsFake> diagnostics;
    std::shared_ptr<Support::FixedClock> clock;
    std::shared_ptr<Contracts::IRedactor> redactor;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher;
    std::shared_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec> codec;
    std::shared_ptr<Fakes::SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepository> repository;
};

[[nodiscard]] Domain::Result<void> writeMarkerOnce(
    const std::filesystem::path& path,
    const std::string_view expected) noexcept
{
    try {
        const UniqueHandle file{CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr)};
        if (!file) {
            if (GetLastError() != ERROR_FILE_EXISTS) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The durable host marker could not be created."));
            }
            std::ifstream input{path, std::ios::binary};
            const std::string retained{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}};
            // streambuf iteration does not promise to set eofbit on the owning
            // stream, so badbit (plus the exact bytes) is the reliable replay
            // check here.
            if (!input.is_open() || input.bad() || retained != expected) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable host marker conflicts with the requested effect."));
            }
            return Domain::Result<void>::success();
        }
        DWORD written{};
        if (expected.size() > static_cast<std::size_t>((MAXDWORD))) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The durable host marker exceeded the Win32 write bound."));
        }
        if (WriteFile(
                file.get(),
                expected.data(),
                static_cast<DWORD>(expected.size()),
                &written,
                nullptr) == FALSE ||
            written != expected.size() || FlushFileBuffers(file.get()) == FALSE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The durable host marker could not be flushed."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The durable host marker failed safely."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::string>> readMarkerLines(
    const std::filesystem::path& path) noexcept
{
    try {
        if (!std::filesystem::exists(path)) {
            return Domain::Result<std::vector<std::string>>::success({});
        }
        std::ifstream input{path, std::ios::binary};
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(std::move(line));
        }
        if (!input.eof()) {
            return Domain::Result<std::vector<std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable host marker could not be read completely."));
        }
        return Domain::Result<std::vector<std::string>>::success(std::move(lines));
    } catch (...) {
        return Domain::Result<std::vector<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The durable host marker could not be parsed."));
    }
}

class FileBackedSessionHost final : public Contracts::ISessionHostAdapter {
public:
    FileBackedSessionHost(
        std::filesystem::path root,
        const CrashSignal* crashSignal)
        : root_{std::move(root)},
          crashSignal_{crashSignal},
          adapterId_{parse<Domain::AdapterId>("p11-process-host")}
    {
        std::filesystem::create_directories(root_);
    }

    [[nodiscard]] const Domain::AdapterId& identifier() const noexcept override
    {
        return adapterId_;
    }

    [[nodiscard]] std::string_view version() const noexcept override
    {
        return "p11-process-host-1";
    }

    [[nodiscard]] Domain::Result<Domain::HostCapabilities> capabilities(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::HostCapabilities>::success(
            Domain::HostCapabilities{
                true, true, true, true, true, true, true, true});
    }

    [[nodiscard]] Domain::Result<Domain::HostSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        try {
            const auto sessionId = parse<Domain::SessionId>(
                request.operationId.value());
            const auto marker = sessionMarker(request, sessionId);
            auto written = writeMarkerOnce(root_ / L"session.marker", marker);
            if (!written) {
                return propagate<Domain::HostSession>(std::move(written));
            }
            if (crashSignal_ != nullptr) {
                crashSignal_->reach(CrashBoundary::CreateEffectBeforeCommit);
            }
            return Domain::Result<Domain::HostSession>::success(
                sessionFor(request, sessionId));
        } catch (...) {
            return internalFailure<Domain::HostSession>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::HostSession>>
    queryByIdempotencyKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext&) noexcept override
    {
        auto lines = readMarkerLines(root_ / L"session.marker");
        if (!lines) {
            return propagate<std::optional<Domain::HostSession>>(std::move(lines));
        }
        if (lines.value().empty()) {
            return Domain::Result<std::optional<Domain::HostSession>>::success(
                std::nullopt);
        }
        if (lines.value().size() != 5U || lines.value()[1] != projectId.value() ||
            lines.value()[3] != key.value()) {
            return Domain::Result<std::optional<Domain::HostSession>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable host session marker has another binding."));
        }
        try {
            const Domain::SessionCreationRequest request{
                parse<Domain::ContinuityOperationId>(lines.value()[0]),
                parse<Domain::ProjectId>(lines.value()[1]),
                parse<Domain::SessionId>(lines.value()[2]),
                take(Domain::IdempotencyKey::create(lines.value()[3]))};
            const auto sessionId = parse<Domain::SessionId>(lines.value()[4]);
            return Domain::Result<std::optional<Domain::HostSession>>::success(
                sessionFor(request, sessionId));
        } catch (...) {
            return internalFailure<std::optional<Domain::HostSession>>();
        }
    }

    [[nodiscard]] Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext&) noexcept override
    {
        auto compatible = Domain::validateBootstrapCompatibility(session, handoff);
        if (!compatible) {
            return compatible;
        }
        const std::string marker = session.id.value() + "\n" +
            handoff.handoffId.value() + "\n" +
            handoff.contentSha256.value() + "\n";
        auto written = writeMarkerOnce(root_ / L"bootstrap.marker", marker);
        if (!written) {
            return written;
        }
        if (crashSignal_ != nullptr) {
            crashSignal_->reach(
                CrashBoundary::BootstrapEffectBeforeAcknowledgement);
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Domain::HandoffAcknowledgement>
    awaitAcknowledgement(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext&) noexcept override
    {
        const std::string expected = session.id.value() + "\n" +
            handoffId.value() + "\n" + handoffSha256.value() + "\n";
        auto retained = readMarkerLines(root_ / L"bootstrap.marker");
        if (!retained) {
            return propagate<Domain::HandoffAcknowledgement>(std::move(retained));
        }
        if (retained.value().size() != 3U ||
            retained.value()[0] != session.id.value() ||
            retained.value()[1] != handoffId.value() ||
            retained.value()[2] != handoffSha256.value()) {
            static_cast<void>(expected);
            return Domain::Result<Domain::HandoffAcknowledgement>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable bootstrap marker does not match acknowledgement."));
        }
        return Domain::Result<Domain::HandoffAcknowledgement>::success(
            Domain::HandoffAcknowledgement{
                handoffId, session.id, adapterId_, handoffSha256});
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext&) noexcept override
    {
        auto lines = readMarkerLines(root_ / L"session.marker");
        if (!lines) {
            return propagate<Domain::HostSessionStatus>(std::move(lines));
        }
        if (lines.value().size() != 5U || lines.value()[4] != sessionId.value()) {
            return Domain::Result<Domain::HostSessionStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::SessionNotFound,
                    "The durable session is not present."));
        }
        return Domain::Result<Domain::HostSessionStatus>::success(
            Domain::HostSessionStatus::Ready);
    }

    [[nodiscard]] Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest&,
        const Domain::OperationContext&) noexcept override
    {
        Domain::HostRecoveryReport report{};
        report.inspected = std::filesystem::exists(root_ / L"session.marker")
            ? 1U
            : 0U;
        report.recovered = report.inspected;
        return Domain::Result<Domain::HostRecoveryReport>::success(
            std::move(report));
    }

    void cancel(const Domain::OperationId&) noexcept override {}
    void shutdown() noexcept override {}

private:
    template <typename T, typename U>
    [[nodiscard]] static Domain::Result<T> propagate(Domain::Result<U>&& source)
    {
        return Domain::Result<T>::failure(std::move(source).error());
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> internalFailure()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The file-backed session host failed safely."));
    }

    [[nodiscard]] static Domain::HostSession sessionFor(
        const Domain::SessionCreationRequest& request,
        const Domain::SessionId& sessionId)
    {
        return Domain::HostSession{
            sessionId,
            request.projectId,
            request.operationId,
            request.predecessorSessionId,
            request.idempotencyKey,
            std::nullopt,
            std::optional<std::string>{"process-model"},
            Domain::HostSessionStatus::Ready};
    }

    [[nodiscard]] static std::string sessionMarker(
        const Domain::SessionCreationRequest& request,
        const Domain::SessionId& sessionId)
    {
        return request.operationId.value() + "\n" + request.projectId.value() +
            "\n" + request.predecessorSessionId.value() + "\n" +
            request.idempotencyKey.value() + "\n" + sessionId.value() + "\n";
    }

    std::filesystem::path root_;
    const CrashSignal* crashSignal_{};
    Domain::AdapterId adapterId_;
};

class CrashAwareRepository final : public Contracts::IContinuityRepository {
public:
    CrashAwareRepository(
        std::shared_ptr<Contracts::IContinuityRepository> inner,
        const CrashSignal* crashSignal)
        : inner_{std::move(inner)}, crashSignal_{crashSignal}
    {
    }

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept override
    {
        return inner_->projectId();
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> createOperation(
        const Domain::ContinuityHandoff& handoff,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->createOperation(handoff, key, context);
    }

    [[nodiscard]] Domain::Result<void> storeHandoff(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->storeHandoff(handoff, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>> handoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->handoff(projectId, handoffId, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    operation(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->operation(projectId, operationId, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    activeOperation(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->activeOperation(projectId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> compareAndSet(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState expected,
        const Domain::ContinuityState next,
        std::optional<Domain::SessionId> successorSessionId,
        std::optional<std::string> evidence,
        const Domain::OperationContext& context) noexcept override
    {
        auto result = inner_->compareAndSet(
            operationId,
            expected,
            next,
            std::move(successorSessionId),
            std::move(evidence),
            context);
        if (result && crashSignal_ != nullptr) {
            switch (next) {
            case Domain::ContinuityState::CheckpointPreparing:
                crashSignal_->reach(CrashBoundary::CheckpointIntent);
                break;
            case Domain::ContinuityState::CheckpointPersisted:
                crashSignal_->reach(CrashBoundary::CheckpointPersisted);
                break;
            case Domain::ContinuityState::SuccessorCreating:
                crashSignal_->reach(CrashBoundary::SuccessorCreateIntent);
                break;
            case Domain::ContinuityState::SuccessorCreated:
                crashSignal_->reach(CrashBoundary::SuccessorCommit);
                break;
            case Domain::ContinuityState::BootstrapSending:
                crashSignal_->reach(CrashBoundary::BootstrapIntent);
                break;
            case Domain::ContinuityState::PredecessorSealing:
                crashSignal_->reach(CrashBoundary::PredecessorSealIntent);
                break;
            case Domain::ContinuityState::Completed:
                crashSignal_->reach(CrashBoundary::CompletedPointerCommit);
                break;
            case Domain::ContinuityState::Idle:
            case Domain::ContinuityState::Acknowledged:
            case Domain::ContinuityState::RetryWait:
            case Domain::ContinuityState::FailedRecoverable:
            case Domain::ContinuityState::Cancelling:
            case Domain::ContinuityState::Cancelled:
                break;
            }
        }
        return result;
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> acknowledge(
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override
    {
        auto result = inner_->acknowledge(operationId, acknowledgement, context);
        if (result && crashSignal_ != nullptr) {
            crashSignal_->reach(CrashBoundary::AcknowledgementCommit);
        }
        return result;
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> recordRetry(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState resumeState,
        std::string error,
        std::optional<Domain::UtcTimePoint> retryAt,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->recordRetry(
            operationId,
            resumeState,
            std::move(error),
            retryAt,
            context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::SessionId>> activeSession(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->activeSession(projectId, context);
    }

    [[nodiscard]] Domain::Result<std::size_t> transitionCount(
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->transitionCount(operationId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->status(projectId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport> resetContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return inner_->resetContinuity(request, context);
    }

    void close() noexcept override { inner_->close(); }

private:
    std::shared_ptr<Contracts::IContinuityRepository> inner_;
    const CrashSignal* crashSignal_{};
};

class SingleRepositoryFactory final
    : public Contracts::IContinuityRepositoryFactory {
public:
    explicit SingleRepositoryFactory(
        std::shared_ptr<Contracts::IContinuityRepository> repository)
        : repository_{std::move(repository)}
    {
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IContinuityRepository>>
    openContinuity(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        if (projectId != repository_->projectId()) {
            return Domain::Result<
                std::shared_ptr<Contracts::IContinuityRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The single continuity repository has another project."));
        }
        open_ = true;
        return Domain::Result<
            std::shared_ptr<Contracts::IContinuityRepository>>::success(
                repository_);
    }

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        open_ = false;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::size_t openCount() const noexcept override
    {
        return open_ ? 1U : 0U;
    }

    void shutdown() noexcept override { open_ = false; }

private:
    std::shared_ptr<Contracts::IContinuityRepository> repository_;
    bool open_{};
};

[[nodiscard]] Fakes::ProjectRegistryRepositoryFake registryFor(
    const Domain::ContinuityHandoff& handoff,
    const Domain::MonotonicTimePoint now)
{
    Fakes::ProjectRegistryRepositoryFake registry{1U, now};
    take(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
        handoff.project.projectId,
        handoff.project.displayName,
        std::optional<std::string>{"abrupt-process-repository"},
        {handoff.project.repositoryRoot}}));
    return registry;
}

[[nodiscard]] std::filesystem::path hostRoot(
    const std::filesystem::path& root)
{
    return root / L"durable-host";
}

void appendChildProgress(
    const std::filesystem::path& root,
    const std::string_view step)
{
    std::ofstream output{
        root / L"child-progress.log", std::ios::binary | std::ios::app};
    output << step << '\n';
    output.flush();
}

int runCrashChild(
    const CrashBoundary boundary,
    const std::filesystem::path& root,
    const std::wstring_view eventName)
{
    const auto identifier = 1'000U + static_cast<std::uint32_t>(boundary);
    appendChildProgress(root, "crash-child-entered");
    CrashSignal signal{boundary, eventName};
    appendChildProgress(root, "crash-event-opened");
    RepositoryRuntime runtime{root, identifier};
    appendChildProgress(root, "repository-opened");
    FileBackedSessionHost host{hostRoot(root), &signal};
    appendChildProgress(root, "host-opened");
    const auto handoff = runtime.handoff(identifier, host.identifier());
    appendChildProgress(root, "handoff-encoded");
    auto registry = registryFor(handoff, runtime.clock->monotonicNow());
    auto wrapped = std::make_shared<CrashAwareRepository>(
        runtime.repository, &signal);
    SingleRepositoryFactory factory{wrapped};
    Application::ContinuityCoordinator coordinator{
        registry, factory, host, *runtime.clock};
    const auto context = operationContext(
        *runtime.clock, identifier, "abrupt-crash-child");
    auto checkpoint = coordinator.checkpoint(
        Domain::CheckpointRequest{handoff}, context);
    appendChildProgress(root, "checkpoint-returned");
    if (!checkpoint) {
        return 31;
    }
    auto rollover = coordinator.requestRollover(
        Domain::RolloverRequest{handoff.project.projectId, handoff.operationId},
        context);
    if (!rollover) {
        return 32;
    }
    return 33;
}

int runRecoveryChild(
    const CrashBoundary boundary,
    const std::filesystem::path& root)
{
    try {
        appendChildProgress(root, "recovery-child-entered");
        const auto identifier = 1'000U + static_cast<std::uint32_t>(boundary);
        RepositoryRuntime runtime{root, identifier};
        appendChildProgress(root, "recovery-repository-opened");
        FileBackedSessionHost host{hostRoot(root), nullptr};
        const auto handoff = runtime.handoff(identifier, host.identifier());
        auto registry = registryFor(handoff, runtime.clock->monotonicNow());
        SingleRepositoryFactory factory{runtime.repository};
        Application::ContinuityCoordinator coordinator{
            registry, factory, host, *runtime.clock};
        const auto context = operationContext(
            *runtime.clock, identifier + 200U, "abrupt-recovery-child");
        const auto recovered = take(coordinator.recoverIncompleteOperations(
            Domain::ContinuityRecoveryRequest{handoff.project.projectId, true},
            context));
        appendChildProgress(root, "recovery-coordinator-returned");
        if (boundary == CrashBoundary::CompletedPointerCommit) {
            if (recovered.inspected != 0U || recovered.resumed != 0U) {
                return 41;
            }
        } else if (recovered.inspected != 1U || recovered.resumed != 1U ||
                   recovered.failed != 0U) {
            return 42;
        }
        const auto operation = take(runtime.repository->operation(
            handoff.project.projectId, handoff.operationId, context));
        if (!operation ||
            operation->state != Domain::ContinuityState::Completed) {
            return 43;
        }
        appendChildProgress(root, "recovery-operation-completed");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        appendChildProgress(root, "recovery-exception:");
        appendChildProgress(root, error.what());
        return 44;
    } catch (...) {
        appendChildProgress(root, "recovery-unknown-exception");
        return 44;
    }
}

[[nodiscard]] std::wstring quoteArgument(const std::wstring_view value)
{
    std::wstring quoted{L"\""};
    std::size_t slashes{};
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(character);
            slashes = 0U;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0U;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

[[nodiscard]] std::filesystem::path executablePath()
{
    std::wstring buffer(32'768U, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        throw std::runtime_error{"The process fixture executable path is unavailable."};
    }
    buffer.resize(length);
    return std::filesystem::path{std::move(buffer)};
}

[[nodiscard]] UniqueHandle launchChild(
    const std::vector<std::wstring>& arguments)
{
    std::wstring command = quoteArgument(executablePath().wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command.append(quoteArgument(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process) == FALSE) {
        throw std::runtime_error{"The process fixture child could not be launched."};
    }
    UniqueHandle thread{process.hThread};
    return UniqueHandle{process.hProcess};
}

void waitForSuccessfulChild(
    UniqueHandle& process,
    const std::filesystem::path& root)
{
    const auto waitResult = WaitForSingleObject(
        process.get(),
        static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(ChildTimeout)
                .count()));
    if (waitResult != WAIT_OBJECT_0) {
        static_cast<void>(TerminateProcess(process.get(), CrashExitCode));
        static_cast<void>(WaitForSingleObject(process.get(), 10'000U));
    }
    REQUIRE(waitResult == WAIT_OBJECT_0);
    DWORD exitCode{};
    REQUIRE(GetExitCodeProcess(process.get(), &exitCode) != FALSE);
    if (exitCode != EXIT_SUCCESS) {
        std::ifstream input{root / L"child-progress.log", std::ios::binary};
        const std::string progress{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
        throw std::runtime_error{
            "Recovery child exited " + std::to_string(exitCode) + ": " +
            progress};
    }
    REQUIRE(exitCode == EXIT_SUCCESS);
}

void launchCrashAndTerminate(
    const CrashBoundary boundary,
    const std::filesystem::path& root)
{
    const auto eventName = L"Local\\ForgeConductor-P11-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(static_cast<std::uint32_t>(boundary));
    UniqueHandle event{CreateEventW(nullptr, TRUE, FALSE, eventName.c_str())};
    REQUIRE(event);
    auto child = launchChild({
        L"--child",
        L"crash",
        std::to_wstring(static_cast<std::uint32_t>(boundary)),
        root.wstring(),
        eventName});
    const HANDLE waits[]{event.get(), child.get()};
    const auto waitResult = WaitForMultipleObjects(
        2U,
        waits,
        FALSE,
        static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(ChildTimeout)
                .count()));
    if (waitResult != WAIT_OBJECT_0) {
        static_cast<void>(TerminateProcess(child.get(), CrashExitCode));
        static_cast<void>(WaitForSingleObject(child.get(), 10'000U));
    }
    REQUIRE(waitResult == WAIT_OBJECT_0);
    REQUIRE(TerminateProcess(child.get(), CrashExitCode) != FALSE);
    REQUIRE(WaitForSingleObject(child.get(), 10'000U) == WAIT_OBJECT_0);
    DWORD exitCode{};
    REQUIRE(GetExitCodeProcess(child.get(), &exitCode) != FALSE);
    REQUIRE(exitCode == CrashExitCode);
}

void inspectRecoveredBoundary(
    const CrashBoundary boundary,
    const std::filesystem::path& root)
{
    const auto identifier = 1'000U + static_cast<std::uint32_t>(boundary);
    RepositoryRuntime runtime{root, identifier};
    FileBackedSessionHost host{hostRoot(root), nullptr};
    const auto handoffTemplate = runtime.handoff(identifier, host.identifier());
    const auto context = operationContext(
        *runtime.clock, identifier + 300U, "abrupt-parent-inspection");
    const auto operation = take(runtime.repository->operation(
        handoffTemplate.project.projectId,
        handoffTemplate.operationId,
        context));
    REQUIRE(operation.has_value());
    REQUIRE(operation->state == Domain::ContinuityState::Completed);
    REQUIRE(operation->successorSessionId.has_value());
    REQUIRE(operation->acknowledgedSessionId == operation->successorSessionId);
    REQUIRE(operation->acknowledgedHandoffId ==
            std::optional<Domain::ContinuityHandoffId>{operation->handoffId});
    REQUIRE(operation->adapterId == host.identifier());
    take(Domain::validateContinuityOperationRetryState(*operation));

    const auto durableHandoff = take(runtime.repository->handoff(
        operation->projectId, operation->handoffId, context));
    REQUIRE(durableHandoff.has_value());
    REQUIRE(durableHandoff->successorSession.has_value());
    REQUIRE(durableHandoff->successorSession->sessionId ==
            *operation->successorSessionId);
    take(Domain::validateHandoffAcknowledgement(
        *operation,
        *durableHandoff,
        Domain::HandoffAcknowledgement{
            operation->handoffId,
            *operation->successorSessionId,
            operation->adapterId,
            durableHandoff->contentSha256}));

    const auto active = take(runtime.repository->activeSession(
        operation->projectId, context));
    REQUIRE(active == operation->successorSessionId);
    REQUIRE(take(runtime.repository->transitionCount(
                operation->operationId, context)) == 9U);
    const auto status = take(runtime.repository->status(
        operation->projectId, context));
    REQUIRE(!status.activeOperation);
    REQUIRE(status.operationCount == 1U);
    REQUIRE(status.handoffCount == 1U);
    REQUIRE(!status.recoveryRequired);

    const auto sessionMarker = take(readMarkerLines(
        hostRoot(root) / L"session.marker"));
    const auto bootstrapMarker = take(readMarkerLines(
        hostRoot(root) / L"bootstrap.marker"));
    REQUIRE(sessionMarker.size() == 5U);
    REQUIRE(bootstrapMarker.size() == 3U);
    REQUIRE(sessionMarker[4] == operation->successorSessionId->value());
    REQUIRE(bootstrapMarker[0] == operation->successorSessionId->value());
    REQUIRE(bootstrapMarker[1] == operation->handoffId.value());
    REQUIRE(bootstrapMarker[2] == durableHandoff->contentSha256.value());
}

void abruptTerminationAtEveryBoundaryRecoversExactlyOnce()
{
    for (std::uint32_t index = 0U;
         index < static_cast<std::uint32_t>(CrashBoundary::Count);
         ++index) {
        const auto boundary = static_cast<CrashBoundary>(index);
        Support::ScopedTestDirectory directory{
            L"continuity-coordinator-process-" + std::to_wstring(index)};
        launchCrashAndTerminate(boundary, directory.path());
        auto recovery = launchChild({
            L"--child",
            L"recover",
            std::to_wstring(index),
            directory.path().wstring()});
        waitForSuccessfulChild(recovery, directory.path());
        inspectRecoveredBoundary(boundary, directory.path());
        std::cout << "BOUNDARY PASS " << boundaryName(boundary) << '\n';
    }
}

[[nodiscard]] CrashBoundary parseBoundary(const std::wstring_view value)
{
    const auto parsed = std::stoul(std::wstring{value});
    if (parsed >= static_cast<std::uint32_t>(CrashBoundary::Count)) {
        throw std::runtime_error{"The requested crash boundary is out of range."};
    }
    return static_cast<CrashBoundary>(parsed);
}

} // namespace

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount >= 2 && std::wstring_view{arguments[1]} == L"--child") {
        if (argumentCount == 6 && std::wstring_view{arguments[2]} == L"crash") {
            return runCrashChild(
                parseBoundary(arguments[3]),
                std::filesystem::path{arguments[4]},
                arguments[5]);
        }
        if (argumentCount == 5 && std::wstring_view{arguments[2]} == L"recover") {
            return runRecoveryChild(
                parseBoundary(arguments[3]),
                std::filesystem::path{arguments[4]});
        }
        return 64;
    }

    try {
        abruptTerminationAtEveryBoundaryRecoversExactlyOnce();
        std::cout << "PASS abrupt_termination_at_all_ten_boundaries\n"
                  << "SUMMARY passed=1 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL abrupt_termination_at_all_ten_boundaries: "
                  << error.what() << '\n'
                  << "SUMMARY passed=0 failed=1 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_FAILURE;
    }
}
