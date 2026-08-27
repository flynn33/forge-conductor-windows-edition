#pragma once

#include "PlatformBoundaryFakeTestSupport.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FileSystemFake.h"
#include "Fakes/GitServiceFake.h"
#include "Fakes/PdfServiceFake.h"
#include "Fakes/ShellServiceFake.h"
#include "Fakes/TextSearchServiceFake.h"

#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {

inline void runNativeToolBoundaryFakeContractTests()
{
    namespace Support = PlatformBoundaryTestSupport;
    namespace Fakes = ForgeConductor::Tests::Fakes;

    static_assert(std::is_final_v<Fakes::RecordingTextSearchServiceFake>);
    static_assert(std::is_final_v<Fakes::RecordingFileSystemFake>);
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

    Fakes::RecordingFileSystemFake filesystem{32, 2};
    filesystem.setNow(now);
    filesystem.listResult.set(
        Domain::Result<std::vector<Domain::PathText>>::success({file, file}));
    filesystem.listResultTruncated = true;
    const auto directoryListing =
        Support::take(filesystem.list(readPath, 1, context));
    Support::require(
        directoryListing.truncated && directoryListing.entries.size() == 1U,
        "filesystem fake did not preserve sorted-prefix truncation metadata");
    Support::require(
        filesystem.lastCapture() &&
            filesystem.lastCapture()->requestedBound == 1U &&
            filesystem.lastCapture()->primary.authorityId() ==
                authority.authorityId(),
        "filesystem fake did not retain listing authority and bounds");

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
    Domain::ProcessResult scriptedGitStatus;
    scriptedGitStatus.exitCode = 23;
    scriptedGitStatus.stdoutUtf8 = "dirty";
    scriptedGitStatus.stderrUtf8 = "notice";
    scriptedGitStatus.timedOut = true;
    scriptedGitStatus.cancelled = true;
    scriptedGitStatus.stdoutTruncated = true;
    scriptedGitStatus.stderrTruncated = true;
    scriptedGitStatus.terminationConfirmed = false;
    scriptedGitStatus.elapsed = std::chrono::milliseconds{17};
    git.statusResult.set(
        Domain::Result<Domain::ProcessResult>::success(scriptedGitStatus));
    const auto gitStatus = Support::take(
        git.status(readPath, authority, 8, context));
    Support::require(
        gitStatus.exitCode == scriptedGitStatus.exitCode &&
            gitStatus.stdoutUtf8 == scriptedGitStatus.stdoutUtf8 &&
            gitStatus.stderrUtf8 == scriptedGitStatus.stderrUtf8 &&
            gitStatus.timedOut == scriptedGitStatus.timedOut &&
            gitStatus.cancelled == scriptedGitStatus.cancelled &&
            gitStatus.stdoutTruncated == scriptedGitStatus.stdoutTruncated &&
            gitStatus.stderrTruncated == scriptedGitStatus.stderrTruncated &&
            gitStatus.terminationConfirmed ==
                scriptedGitStatus.terminationConfirmed &&
            gitStatus.elapsed == scriptedGitStatus.elapsed,
        "git status did not preserve its configured nonzero process result");
    Support::require(
        git.lastCapture() &&
            git.lastCapture()->repository.authorityId() ==
                authority.authorityId(),
        "git fake did not retain repository authority binding");
    Domain::ProcessResult oversizedGitDiff;
    oversizedGitDiff.exitCode = 9;
    oversizedGitDiff.stdoutUtf8 = "0123456789";
    oversizedGitDiff.stderrUtf8 = "abcdefghij";
    git.diffResult.set(Domain::Result<Domain::ProcessResult>::success(
        std::move(oversizedGitDiff)));
    const auto boundedGitDiff = Support::take(
        git.diff(readPath, authority, {}, 4, context));
    Support::require(
        boundedGitDiff.exitCode == 9 &&
            boundedGitDiff.stdoutUtf8 == "0123" &&
            boundedGitDiff.stderrUtf8 == "abcdefgh" &&
            boundedGitDiff.stdoutTruncated &&
            boundedGitDiff.stderrTruncated,
        "git fake did not cap structured output and retain process status");
    const std::vector<std::string> tooManyArguments{"a", "b", "c"};
    Support::requireError(
        git.diff(readPath, authority, tooManyArguments, 8, context),
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
    const auto pdfPath = Support::path("C:/platform-boundary/file.pdf");
    pdf.writeResult.set(Domain::Result<Domain::PdfWriteReceipt>::success(
        Domain::PdfWriteReceipt{pdfPath, 123, 1, "test", "title"}));
    pdf.fromTextFileResult.set(
        Domain::Result<Domain::PdfWriteReceipt>::success(
            Domain::PdfWriteReceipt{pdfPath, 124, 1, "test", "title"}));
    Support::require(
        pdf.write("title", "body", writePath, context).hasValue(),
        "PDF fake rejected bounded text");
    Support::require(
        pdf.lastCapture() &&
            pdf.lastCapture()->primary.authorityId() ==
                authority.authorityId(),
        "PDF fake did not retain destination authority binding");
    Support::require(
        pdf.fromTextFile("title", readPath, writePath, context).hasValue() &&
            pdf.lastCapture() &&
            pdf.lastCapture()->secondary &&
            pdf.lastCapture()->secondary->authorityId() ==
                authority.authorityId(),
        "PDF fake did not retain source/destination binding");
}

} // namespace ForgeConductor::Tests
