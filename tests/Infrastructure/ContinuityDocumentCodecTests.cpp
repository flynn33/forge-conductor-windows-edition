#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"Requirement failed: "} + #condition};              \
        }                                                                         \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{1'735'789'855s};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return Domain::MonotonicTimePoint{1s};
    }
};

[[nodiscard]] Domain::OperationContext context(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        Domain::MonotonicTimePoint{2s},
        cancellation,
        take(Domain::CorrelationId::parse("continuity-codec-test"))};
}

[[nodiscard]] std::unique_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec>
codec()
{
    return std::make_unique<
        InfrastructureWindows::WindowsContinuityDocumentCodec>(
        std::make_shared<InfrastructureWindows::BCryptSha256Hasher>(),
        std::make_shared<FixedClock>());
}

constexpr std::string_view ExpectedDigest =
    "fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f";

constexpr std::string_view ProjectV1Handoff = R"json({"completed_work":[{"summary":"Legacy checkpoint persisted"}],"constraints":["Preserve durable state"],"created_at":"2025-01-02T03:04:15Z","current_work":{"active_files":["README.md"],"phase_id":"P07","summary":"Migrate legacy persistence","work_item_id":"legacy-fixture"},"decisions":[{"decision":"Retain legacy rows"}],"evidence_references":[{"path":"legacy-evidence"}],"handoff_id":"66666666-6666-4666-8666-666666666666","host_state":{"adapter_id":"legacy-adapter","context_budget_source":"legacy-fixture","continuity_state":"checkpointPersisted","retry":{"attempt":2}},"integrity":{"content_sha256":"fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f","redaction_complete":true},"memory_references":[{"record_id":"22222222-2222-4222-8222-222222222222"}],"mission":"Preserve legacy project state","next_actions":[{"action":"Continue migration","command":"","order":1,"success_condition":"Legacy rows remain readable"}],"open_work":[{"summary":"Apply Windows migration"}],"operation_id":"55555555-5555-4555-8555-555555555555","predecessor_session":{"model":"legacy-model","provider_session_id":null,"session_id":"77777777-7777-4777-8777-777777777777"},"project":{"branch":"legacy","commit":"0123456789abcdef","dirty_summary":[],"display_name":"Legacy Project","project_id":"11111111-1111-4111-8111-111111111111","repository_root":"legacy-root"},"schema_version":"1.0","successor_session":null,"validation":{"commands":[],"open_gates":["G07"],"passed_gates":["G06"]}})json";

void projectV1GoldenRoundTrip()
{
    auto subject = codec();
    const auto decoded = take(subject->decode(ProjectV1Handoff, context()));
    REQUIRE(decoded.canonicalUtf8 == ProjectV1Handoff);
    REQUIRE(decoded.handoff.contentSha256.value() == ExpectedDigest);
    REQUIRE(decoded.handoff.nextActions.size() == 1U);
    REQUIRE(decoded.handoff.nextActions.front().command.empty());
    REQUIRE(decoded.handoff.evidenceReferences.size() == 1U);
    REQUIRE(decoded.handoff.evidenceReferences.front().path.has_value());
    REQUIRE(decoded.handoff.hostState.persistedContinuityStateName ==
            std::optional<std::string>{"checkpointPersisted"});
    REQUIRE(decoded.handoff.hostState.continuityState ==
            Domain::ContinuityState::CheckpointPersisted);

    const auto encoded = take(subject->encode(decoded.handoff, context()));
    REQUIRE(encoded.canonicalUtf8 == ProjectV1Handoff);
    REQUIRE(encoded.handoff.contentSha256.value() == ExpectedDigest);
}

void typedEncodingPreservesRequiredEmptyCommandAndNullableSession()
{
    auto subject = codec();
    auto handoff = take(subject->decode(ProjectV1Handoff, context())).handoff;
    handoff.mission = "Preserve Unicode \xc3\xa9 and escaped line\nstate";
    handoff.predecessorSession.provider = "local";
    handoff.successorSession.reset();
    handoff.nextActions.front().command.clear();
    handoff.hostState.persistedContinuityStateName.reset();

    const auto encoded = take(subject->encode(handoff, context()));
    REQUIRE(encoded.canonicalUtf8.find("\"command\":\"\"") !=
            std::string::npos);
    REQUIRE(encoded.canonicalUtf8.find("\"successor_session\":null") !=
            std::string::npos);
    REQUIRE(encoded.canonicalUtf8.find("\"provider\":\"local\"") !=
            std::string::npos);
    REQUIRE(encoded.canonicalUtf8.find("\\nstate") != std::string::npos);
    REQUIRE(take(subject->decode(encoded.canonicalUtf8, context())).canonicalUtf8 ==
            encoded.canonicalUtf8);
}

void malformedDocumentsFailClosed()
{
    auto subject = codec();

    std::string duplicate{ProjectV1Handoff};
    const auto retry = duplicate.find("\"retry\":{\"attempt\":2}");
    REQUIRE(retry != std::string::npos);
    duplicate.replace(
        retry,
        std::string_view{"\"retry\":{\"attempt\":2}"}.size(),
        "\"retry\":{\"attempt\":2,\"attempt\":3}");
    const auto duplicateResult = subject->decode(duplicate, context());
    REQUIRE(!duplicateResult);
    REQUIRE(duplicateResult.error().code == Domain::ErrorCodes::InvalidRequest);

    std::string nonCanonical{ProjectV1Handoff};
    nonCanonical.insert(1U, " ");
    REQUIRE(!subject->decode(nonCanonical, context()));

    std::string wrongHash{ProjectV1Handoff};
    const auto digest = wrongHash.find(ExpectedDigest);
    REQUIRE(digest != std::string::npos);
    wrongHash[digest] = 'a';
    if (ExpectedDigest.front() == 'a') {
        wrongHash[digest] = 'b';
    }
    const auto hashResult = subject->decode(wrongHash, context());
    REQUIRE(!hashResult);
    REQUIRE(hashResult.error().code == Domain::ErrorCodes::IntegrityFailure);

    std::string invalidUtf8{ProjectV1Handoff};
    invalidUtf8.insert(1U, 1U, static_cast<char>(0xff));
    REQUIRE(!subject->decode(invalidUtf8, context()));

    std::string wrongType{ProjectV1Handoff};
    const auto order = wrongType.find("\"order\":1");
    REQUIRE(order != std::string::npos);
    wrongType.replace(order, 9U, "\"order\":\"1\"");
    REQUIRE(!subject->decode(wrongType, context()));

    std::string invalidAdapter{ProjectV1Handoff};
    const auto adapter = invalidAdapter.find("legacy-adapter");
    REQUIRE(adapter != std::string::npos);
    invalidAdapter.replace(adapter, 14U, "../escape");
    const auto adapterResult = subject->decode(invalidAdapter, context());
    REQUIRE(!adapterResult);
    REQUIRE(adapterResult.error().code == Domain::ErrorCodes::InvalidRequest);

    std::string unsupported{ProjectV1Handoff};
    const auto schema = unsupported.find("\"schema_version\":\"1.0\"");
    REQUIRE(schema != std::string::npos);
    unsupported.replace(schema, 22U, "\"schema_version\":\"2.0\"");
    const auto unsupportedResult = subject->decode(unsupported, context());
    REQUIRE(!unsupportedResult);
    REQUIRE(unsupportedResult.error().code ==
            Domain::ErrorCodes::UnsupportedVersion);

    std::string escapedNul{ProjectV1Handoff};
    const auto mission = escapedNul.find("Preserve legacy project state");
    REQUIRE(mission != std::string::npos);
    escapedNul.replace(
        mission,
        std::string_view{"Preserve legacy project state"}.size(),
        "Preserve\\u0000state");
    REQUIRE(!subject->decode(escapedNul, context()));

    std::string tooDeep{"{\"a\":"};
    tooDeep.append(66U, '[');
    tooDeep += "null";
    tooDeep.append(66U, ']');
    tooDeep += '}';
    REQUIRE(!subject->decode(tooDeep, context()));
}

void domainRetryAndSemanticValidation()
{
    auto subject = codec();
    auto handoff = take(subject->decode(ProjectV1Handoff, context())).handoff;
    handoff.project.repositoryRoot = take(Domain::PathText::create("   "));
    REQUIRE(!subject->encode(handoff, context()));

    handoff = take(subject->decode(ProjectV1Handoff, context())).handoff;
    handoff.hostState.contextBudgetSource = " \r\n ";
    REQUIRE(!subject->encode(handoff, context()));

    handoff = take(subject->decode(ProjectV1Handoff, context())).handoff;
    handoff.hostState.continuityState = Domain::ContinuityState::RetryWait;
    handoff.hostState.persistedContinuityStateName.reset();
    handoff.hostState.retry.retryAt = handoff.createdAt + 5s;
    handoff.hostState.retry.retryResumeState =
        Domain::ContinuityState::BootstrapSending;
    const auto retryDocument = take(subject->encode(handoff, context()));
    REQUIRE(retryDocument.canonicalUtf8.find(
                "\"retry_resume_state\":\"bootstrap_sending\"") !=
            std::string::npos);
    const auto decodedRetry = take(subject->decode(
        retryDocument.canonicalUtf8, context()));
    REQUIRE(decodedRetry.handoff.hostState.retry.retryResumeState ==
            std::optional<Domain::ContinuityState>{
                Domain::ContinuityState::BootstrapSending});

    Domain::ContinuityOperation operation{
        handoff.operationId,
        handoff.project.projectId,
        handoff.predecessorSession.sessionId,
        std::nullopt,
        handoff.handoffId,
        Domain::ContinuityState::RetryWait,
        1U,
        handoff.hostState.adapterId,
        take(Domain::IdempotencyKey::create("continuity-retry")),
        std::nullopt,
        std::nullopt,
        handoff.createdAt,
        handoff.createdAt,
        std::string{"retry"},
        handoff.createdAt,
        handoff.contentSha256,
        Domain::ContinuityState::BootstrapSending};
    REQUIRE(Domain::validateContinuityOperationRetryState(operation));
    operation.retryResumeState.reset();
    REQUIRE(!Domain::validateContinuityOperationRetryState(operation));
    operation.state = Domain::ContinuityState::FailedRecoverable;
    REQUIRE(Domain::validateContinuityOperationRetryState(operation));
    operation.retryResumeState = Domain::ContinuityState::Completed;
    REQUIRE(!Domain::validateContinuityOperationRetryState(operation));
}

void operationContextAndMissingDependenciesFailClosed()
{
    auto subject = codec();
    std::stop_source source;
    source.request_stop();
    const auto cancelled = subject->decode(ProjectV1Handoff, context(source.get_token()));
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);

    auto expired = context();
    expired.deadline = Domain::MonotonicTimePoint{1s};
    const auto deadline = subject->decode(ProjectV1Handoff, expired);
    REQUIRE(!deadline);
    REQUIRE(deadline.error().code == Domain::ErrorCodes::DeadlineExceeded);

    InfrastructureWindows::WindowsContinuityDocumentCodec missing{
        nullptr, nullptr};
    const auto missingResult = missing.decode(ProjectV1Handoff, context());
    REQUIRE(!missingResult);
    REQUIRE(missingResult.error().code == Domain::ErrorCodes::InternalFailure);
}

} // namespace

int main()
{
    try {
        projectV1GoldenRoundTrip();
        typedEncodingPreservesRequiredEmptyCommandAndNullableSession();
        malformedDocumentsFailClosed();
        domainRetryAndSemanticValidation();
        operationContextAndMissingDependenciesFailClosed();
        std::cout << "Continuity document codec tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
