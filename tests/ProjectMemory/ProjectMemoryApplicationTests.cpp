#include "ForgeConductor/Application/ProjectMemoryService.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/ToolServiceFakes.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

struct Fixture final {
    Domain::ProjectId projectId{parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111")};
    Domain::ProjectId otherProjectId{parse<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222")};
    Domain::MemoryRecordId recordId{parse<Domain::MemoryRecordId>(
        "33333333-3333-4333-8333-333333333333")};
    Domain::PathText projectPath{take(Domain::PathText::create("C:\\workspace"))};
    Domain::PathText artifact{take(Domain::PathText::create(
        "C:\\workspace\\project-memory.json"))};
    Domain::Sha256Digest digest{parse<Domain::Sha256Digest>(std::string(64U, 'a'))};
    Domain::MonotonicTimePoint now{std::chrono::steady_clock::now()};
    Domain::ProjectMemoryLimits limits{
        Domain::projectMemoryLimitsForProfile(Domain::ResourceProfile::Constrained8GiB)};
    Fakes::ProjectRegistryRepositoryFake registry{1U, now};
    Fakes::ProjectMemoryRepositoryFactoryFake factory{1U, 1U, now};
    std::shared_ptr<Fakes::ProjectMemoryRepositoryFake> repository{
        std::make_shared<Fakes::ProjectMemoryRepositoryFake>(projectId, now)};
    Fakes::ScriptedRedactor redactor;

    Fixture()
    {
        const Domain::ProjectMemoryDescriptor descriptor{
            projectId, "Project", std::nullopt, {projectPath}};
        REQUIRE(registry.seedDescriptor(descriptor));
        REQUIRE(registry.seedInitialization(Domain::ProjectInitialization{
            descriptor,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion,
            limits,
            true,
            false,
            true}));
        REQUIRE(factory.addRepository(repository));

        repository->rememberResult.set(
            Domain::Result<Domain::MemoryWriteOutcome>::success(writeOutcome()));
        repository->rememberBatchResult.set(
            Domain::Result<Domain::MemoryBatchOutcome>::success(
                Domain::MemoryBatchOutcome{projectId, {writeOutcome()}}));
        repository->searchResult.set(
            Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
                projectId, {{record(), 1.0}}, std::nullopt, false, 512U,
                limits.defaultResponseBytes}));
        repository->getResult.set(
            Domain::Result<Domain::MemoryRecords>::success(Domain::MemoryRecords{
                projectId, {record()}, 512U, limits.defaultResponseBytes}));
        repository->updateResult.set(
            Domain::Result<Domain::ProjectMemoryRecord>::success(record()));
        repository->forgetResult.set(
            Domain::Result<Domain::ForgetOutcome>::success(
                Domain::ForgetOutcome{
                    projectId, recordId, Domain::ForgetDisposition::Tombstoned}));
        repository->listRecentResult.set(
            Domain::Result<Domain::MemoryPage>::success(Domain::MemoryPage{
                projectId, {{record(), 0.0}}, std::nullopt, false, 512U,
                limits.defaultResponseBytes}));
        repository->linkResult.set(
            Domain::Result<Domain::LinkOutcome>::success(
                Domain::LinkOutcome{projectId, Domain::LinkDisposition::Inserted}));
        repository->exportResult.set(
            Domain::Result<Domain::ProjectMemoryExport>::success(
                Domain::ProjectMemoryExport{projectId, artifact, digest, 1U}));
        repository->importResult.set(
            Domain::Result<Domain::ProjectMemoryImport>::success(
                Domain::ProjectMemoryImport{
                    projectId,
                    Domain::ImportDisposition::Preview,
                    1U,
                    1U,
                    digest,
                    {}}));
        repository->statusResult.set(
            Domain::Result<Domain::ProjectMemoryStatus>::success(status()));
        repository->quickCheckResult.set(Domain::Result<void>::success());
        repository->resetResult.set(
            Domain::Result<Domain::ResetReport>::success(Domain::ResetReport{
                "reset_project_memory",
                projectId.value(),
                1U,
                1U,
                1U,
                1U,
                true}));
    }

    [[nodiscard]] Domain::OperationContext context() const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>("44444444-4444-4444-8444-444444444444"),
            std::chrono::steady_clock::now() + 5s,
            std::stop_token{},
            parse<Domain::CorrelationId>("project-memory-application-test")};
    }

    [[nodiscard]] Domain::ProjectMemoryRecord record() const
    {
        return Domain::ProjectMemoryRecord{
            recordId,
            projectId,
            1U,
            "fact",
            "Title",
            "Summary",
            std::string{"Body"},
            {"tag"},
            0.5,
            1.0,
            "external_integration",
            std::nullopt,
            std::nullopt,
            Domain::UtcTimePoint{},
            Domain::UtcTimePoint{},
            Domain::UtcTimePoint{},
            std::nullopt,
            digest,
            false,
            Domain::ProjectMemorySchemaVersion};
    }

    [[nodiscard]] Domain::MemoryWriteOutcome writeOutcome() const
    {
        return Domain::MemoryWriteOutcome{
            projectId,
            recordId,
            1U,
            Domain::MemoryWriteDisposition::Inserted,
            digest};
    }

    [[nodiscard]] Domain::ProjectMemoryStatus status() const
    {
        return Domain::ProjectMemoryStatus{
            projectId,
            Domain::ProjectMemorySchemaVersion,
            Domain::ProjectMemoryCapabilityVersion,
            1U,
            0U,
            0U,
            0U,
            0U,
            false,
            true,
            0U,
            limits};
    }

    [[nodiscard]] Contracts::WorkspaceAuthority exportAuthority() const
    {
        Fakes::DeterministicWorkspaceAuthority issuer{
            parse<Domain::AuthorityId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            parse<Domain::ClientId>("project-memory-client"),
            {projectPath},
            Domain::FileAccess::Write,
            {Domain::FileAccess::Read, Domain::FileAccess::Write},
            {Domain::FileAccess::Delete},
            false,
            7U};
        return take(issuer.authorityFor(projectId, context()));
    }

    [[nodiscard]] Contracts::AuthorizedToolCall exportAuthorization(
        const Contracts::WorkspaceAuthority& authority) const
    {
        const Domain::ToolCallRequest call{
            Domain::McpRequestMetadata{
                parse<Domain::RequestId>("project-memory-export-request"),
                parse<Domain::CorrelationId>("project-memory-application-test"),
                parse<Domain::ClientId>("project-memory-client"),
                projectId,
                "1.0"},
            "project_memory.export",
            "{}"};
        const Domain::ToolAuthorizationRequest request{
            call,
            Domain::ToolEffect::Write,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}};
        Fakes::DeterministicToolAuthorizerFake authorizer{
            "project_memory.export", Domain::ToolEffect::Write, now};
        return take(authorizer.authorize(request, authority, context()));
    }
};

[[nodiscard]] Domain::ProjectMemoryWrite write(std::string title = "Title")
{
    Domain::ProjectMemoryWrite value;
    value.kind = "fact";
    value.title = std::move(title);
    value.summary = "Summary";
    value.body = "Body";
    value.tags = {"tag"};
    return value;
}

void allTwelveOperationsAndRedaction()
{
    Fixture fixture;
    Application::ProjectMemoryService service{
        fixture.registry, fixture.factory, fixture.redactor, fixture.limits};
    const auto operation = fixture.context();

    REQUIRE(service.initialize(
        Domain::InitializeProjectRequest{
            fixture.projectPath,
            fixture.projectId,
            "Project",
            std::nullopt,
            std::nullopt},
        operation));

    REQUIRE(service.remember(
        Domain::RememberProjectMemoryRequest{
            fixture.projectId, write("secret-title")}, operation));
    const auto* remembered = std::get_if<Domain::RememberProjectMemoryRequest>(
        &fixture.repository->lastRequest().value());
    REQUIRE(remembered != nullptr);
    REQUIRE(remembered->write.title == "<redacted>");

    REQUIRE(service.rememberBatch(
        Domain::RememberProjectMemoryBatchRequest{
            fixture.projectId, {write()}}, operation));
    REQUIRE(service.search(
        Domain::SearchProjectMemoryRequest{
            fixture.projectId, "Title", {}, {}, std::nullopt, 20U,
            std::nullopt, true, fixture.limits.defaultResponseBytes},
        operation));
    REQUIRE(service.get(
        Domain::GetProjectMemoryRequest{
            fixture.projectId,
            {fixture.recordId},
            true,
            fixture.limits.defaultResponseBytes},
        operation));
    REQUIRE(service.update(
        Domain::UpdateProjectMemoryRequest{
            fixture.projectId,
            fixture.recordId,
            1U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt},
        operation));
    REQUIRE(service.forget(
        Domain::ForgetProjectMemoryRequest{fixture.projectId, fixture.recordId},
        operation));
    REQUIRE(service.listRecent(
        Domain::ListRecentProjectMemoryRequest{
            fixture.projectId, {}, std::nullopt, 20U, std::nullopt, true,
            fixture.limits.defaultResponseBytes},
        operation));
    REQUIRE(service.link(
        Domain::LinkProjectMemoryRequest{
            fixture.projectId, fixture.recordId, fixture.recordId, "related"},
        operation));

    const auto authority = fixture.exportAuthority();
    const auto authorization = fixture.exportAuthorization(authority);
    REQUIRE(service.exportMemory(
        Domain::ExportProjectMemoryRequest{fixture.projectId},
        authority,
        authorization,
        operation));
    REQUIRE(service.importMemory(
        Domain::ImportProjectMemoryRequest{
            fixture.projectId, fixture.artifact, true, false},
        operation));
    const auto status = service.status(
        Domain::ProjectMemoryStatusRequest{fixture.projectId}, operation);
    REQUIRE(status);
    REQUIRE(status.value().openRepositories == 1U);

    REQUIRE(fixture.redactor.calls() >= 9U);
}

void rejectsSecretsAndCrossProjectAccessBeforePersistence()
{
    Fixture fixture;
    Application::ProjectMemoryService service{
        fixture.registry, fixture.factory, fixture.redactor, fixture.limits};
    auto privateKey = write();
    privateKey.body = "-----BEGIN PRIVATE KEY-----";
    const auto rejected = service.remember(
        Domain::RememberProjectMemoryRequest{
            fixture.projectId, std::move(privateKey)}, fixture.context());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::RedactionRejected);
    REQUIRE(fixture.repository->lastCall() ==
            Fakes::ProjectMemoryRepositoryCall::None);

    const auto crossProject = service.search(
        Domain::SearchProjectMemoryRequest{
            fixture.otherProjectId, "query", {}, {}, std::nullopt, 20U,
            std::nullopt, false, fixture.limits.defaultResponseBytes},
        fixture.context());
    REQUIRE(!crossProject);
    REQUIRE(crossProject.error().code == Domain::ErrorCodes::ProjectNotFound);
}

void scopedAndGlobalResetConfirmations()
{
    Fixture fixture;
    Application::ProjectMemoryService service{
        fixture.registry, fixture.factory, fixture.redactor, fixture.limits};
    const auto operation = fixture.context();

    const auto wrong = service.resetProjectMemory(
        fixture.projectId,
        Domain::DestructiveConfirmation{
            "reset_project_memory", fixture.projectId.value(), "wrong"},
        operation);
    REQUIRE(!wrong);
    REQUIRE(wrong.error().code == Domain::ErrorCodes::Unauthorized);

    const auto one = service.resetProjectMemory(
        fixture.projectId,
        Domain::DestructiveConfirmation{
            "reset_project_memory",
            fixture.projectId.value(),
            "RESET PROJECT MEMORY " + fixture.projectId.value()},
        operation);
    REQUIRE(one);
    REQUIRE(one.value().verified);
    REQUIRE(fixture.factory.openCount() == 0U);

    const auto all = service.resetAllProjectMemory(
        Domain::DestructiveConfirmation{
            "reset_all_project_memory",
            "all-projects",
            "RESET ALL PROJECT MEMORY"},
        operation);
    REQUIRE(all);
    REQUIRE(all.value().projectsAffected == 1U);
    REQUIRE(all.value().recordsRemoved == 1U);
    REQUIRE(all.value().verified);
    REQUIRE(fixture.factory.openCount() == 0U);
}

void shutdownIsIdempotentAndStopsAdmission()
{
    Fixture fixture;
    Application::ProjectMemoryService service{
        fixture.registry, fixture.factory, fixture.redactor, fixture.limits};
    service.shutdown();
    service.shutdown();
    const auto rejected = service.status(
        Domain::ProjectMemoryStatusRequest{fixture.projectId}, fixture.context());
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::Cancelled);
}

} // namespace

int main()
{
    try {
        allTwelveOperationsAndRedaction();
        std::cout << "PASS project_memory_application.twelve_operations\n";
        rejectsSecretsAndCrossProjectAccessBeforePersistence();
        std::cout << "PASS project_memory_application.security_scope\n";
        scopedAndGlobalResetConfirmations();
        std::cout << "PASS project_memory_application.reset\n";
        shutdownIsIdempotentAndStopsAdmission();
        std::cout << "PASS project_memory_application.shutdown\n";
        std::cout << "SUMMARY passed=4 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
