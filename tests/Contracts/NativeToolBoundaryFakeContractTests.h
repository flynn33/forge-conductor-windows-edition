#pragma once

#include "PlatformBoundaryFakeTestSupport.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/GitServiceFake.h"
#include "Fakes/PdfServiceFake.h"
#include "Fakes/ShellServiceFake.h"
#include "Fakes/TextSearchServiceFake.h"

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {

inline void runNativeToolBoundaryFakeContractTests()
{
    namespace Support = PlatformBoundaryTestSupport;
    namespace Fakes = ForgeConductor::Tests::Fakes;

    static_assert(std::is_final_v<Fakes::RecordingTextSearchServiceFake>);
    static_assert(std::is_final_v<Fakes::RecordingGitServiceFake>);
    static_assert(std::is_final_v<Fakes::RecordingShellServiceFake>);
    static_assert(std::is_final_v<Fakes::RecordingPdfServiceFake>);

    const Domain::MonotonicTimePoint now{};
    const auto context = Support::activeContext(now);
    const auto root = Support::path("C:/platform-boundary");
    const auto file = Support::path("C:/platform-boundary/file.txt");

    Fakes::DeterministicWorkspaceAuthority issuer{
        Support::authorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        1};
    issuer.setNow(now);
    const auto authority = Support::take(
        issuer.authorityFor(Support::projectId(), context));
    const auto readPath = Support::take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            file,
            std::optional<Domain::PathText>{root},
            Domain::FileAccess::Read,
            false},
        context));
    const auto writePath = Support::take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            file,
            std::optional<Domain::PathText>{root},
            Domain::FileAccess::Write,
            false},
        context));

    Fakes::RecordingTextSearchServiceFake search{2, 32, 8};
    search.setNow(now);
    search.searchResult.set(
        Domain::Result<std::vector<std::string>>::success({"match"}));
    Support::require(
        Support::take(search.search(readPath, "needle", 2, 8, context)).size() == 1,
        "text search did not return its bounded script");
    Support::require(
        search.lastCapture() &&
            search.lastCapture()->root.authorityId() == authority.authorityId(),
        "text search did not retain path authority binding");
    search.searchResult.set(
        Domain::Result<std::vector<std::string>>::success({"too-large"}));
    Support::requireError(
        search.search(readPath, "needle", 2, 4, context),
        Domain::ErrorCodes::LimitExceeded,
        "text search returned an over-cap response");

    Fakes::RecordingGitServiceFake git{2, 32, 8};
    git.setNow(now);
    git.statusResult.set(
        Domain::Result<std::string>::success("clean"));
    Support::require(
        Support::take(git.status(readPath, 8, context)) == "clean",
        "git status did not return its bounded script");
    Support::require(
        git.lastCapture() &&
            git.lastCapture()->repository.authorityId() ==
                authority.authorityId(),
        "git fake did not retain repository authority binding");
    const std::vector<std::string> tooManyArguments{"a", "b", "c"};
    Support::requireError(
        git.diff(readPath, tooManyArguments, 8, context),
        Domain::ErrorCodes::LimitExceeded,
        "git diff exceeded its argument cap");

    Fakes::RecordingShellServiceFake shell{4, 128, 8};
    shell.setNow(now);
    shell.executeResult.set(
        Domain::Result<Domain::ProcessResult>::success(
            Domain::ProcessResult{0, "ok", ""}));
    Domain::ProcessRequest process{Support::path("tool.exe")};
    process.maximumStdoutBytes = 8;
    process.maximumStderrBytes = 8;
    Support::require(
        Support::take(shell.execute(process, authority, context)).exitCode == 0,
        "shell fake did not return its bounded script");
    Support::require(
        shell.lastExecution() &&
            shell.lastExecution()->authority.authorityId() ==
                authority.authorityId() &&
            shell.lastExecution()->request,
        "shell fake did not retain request/authority binding");

    shell.cancel(context.operationId);
    Support::requireError(
        shell.execute(process, authority, context),
        Domain::ErrorCodes::Cancelled,
        "shell fake ignored explicit cancellation");
    shell.shutdown();
    Support::requireError(
        shell.execute(
            process,
            authority,
            Support::activeContext(now, Support::otherOperationId())),
        Domain::ErrorCodes::Cancelled,
        "shell fake accepted work after shutdown");

    Fakes::RecordingPdfServiceFake pdf{32};
    pdf.setNow(now);
    pdf.writeResult.set(Domain::Result<void>::success());
    pdf.fromTextFileResult.set(Domain::Result<void>::success());
    Support::require(
        pdf.write("title", "body", writePath, context).hasValue(),
        "PDF fake rejected bounded text");
    Support::require(
        pdf.lastCapture() &&
            pdf.lastCapture()->primary.authorityId() ==
                authority.authorityId(),
        "PDF fake did not retain destination authority binding");
    Support::require(
        pdf.fromTextFile(readPath, writePath, context).hasValue() &&
            pdf.lastCapture() &&
            pdf.lastCapture()->secondary &&
            pdf.lastCapture()->secondary->authorityId() ==
                authority.authorityId(),
        "PDF fake did not retain source/destination binding");
}

} // namespace ForgeConductor::Tests
