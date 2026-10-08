#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/Domain.h"
#include <cstddef>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Contracts {

class ITextSearchService {
public:
    virtual ~ITextSearchService() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<std::string>> search(
        const AuthorizedPath& root,
        std::string_view query,
        std::size_t maximumMatches,
        std::size_t maximumResponseBytes,
        const Domain::OperationContext& context) noexcept = 0;
};

class IGitService {
public:
    virtual ~IGitService() = default;

    // A successfully launched command returns its complete bounded process
    // outcome even when Git exits nonzero. Contract failures represent request,
    // authority, admission, or process-supervisor failures.

    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> status(
        const AuthorizedPath& repository,
        const WorkspaceAuthority& authority,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> diff(
        const AuthorizedPath& repository,
        const WorkspaceAuthority& authority,
        std::span<const std::string> arguments,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> log(
        const AuthorizedPath& repository,
        const WorkspaceAuthority& authority,
        std::size_t maximumEntries,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> add(
        const AuthorizedPath& repository,
        const WorkspaceAuthority& authority,
        std::span<const AuthorizedPath> paths,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> commit(
        const AuthorizedPath& repository,
        const WorkspaceAuthority& authority,
        std::string_view message,
        const Domain::OperationContext& context) noexcept = 0;
};

class IShellService {
public:
    virtual ~IShellService() = default;

    [[nodiscard]] virtual bool supportsJobs() const noexcept { return false; }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> startCMakeTestRun(
        const Domain::CMakeTestRequest&, const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept { return unavailableJob(); }

    [[nodiscard]] virtual Domain::Result<Domain::CMakeTestRunStatus> getCMakeTestRun(
        std::string_view, std::uint64_t, std::size_t, const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept
    {
        return Domain::Result<Domain::CMakeTestRunStatus>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "Structured CMake/CTest jobs are unavailable from this shell service."));
    }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> startJob(
        const Domain::ProcessRequest&,
        const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept
    {
        return unavailableJob();
    }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> startProcess(
        const Domain::ProcessRequest&, const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept { return unavailableJob(); }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> adoptJob(
        std::string_view, const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept { return unavailableJob(); }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobLogPage> readJobLog(
        std::string_view, bool, std::optional<std::uint64_t>, std::size_t,
        const WorkspaceAuthority&, const Domain::OperationContext&) noexcept
    {
        return Domain::Result<Domain::ShellJobLogPage>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "Tracked process log reading is unavailable from this shell service."));
    }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> getJob(
        std::string_view,
        const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept
    {
        return unavailableJob();
    }

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::ShellJobSnapshot>> listJobs(
        const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept
    {
        return Domain::Result<std::vector<Domain::ShellJobSnapshot>>::failure(
            Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Tracked shell jobs are unavailable from this shell service."));
    }

    [[nodiscard]] virtual Domain::Result<Domain::ShellJobSnapshot> cancelJob(
        std::string_view,
        const WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept
    {
        return unavailableJob();
    }

private:
    [[nodiscard]] static Domain::Result<Domain::ShellJobSnapshot> unavailableJob() noexcept
    {
        return Domain::Result<Domain::ShellJobSnapshot>::failure(
            Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Tracked shell jobs are unavailable from this shell service."));
    }

public:

    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> execute(
        const Domain::ProcessRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class IPdfService {
public:
    static constexpr std::size_t MaximumTitleBytes = 512U;
    static constexpr std::size_t MaximumTextBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumPdfBytes = 16U * 1024U * 1024U;

    virtual ~IPdfService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::PdfWriteReceipt> write(
        std::string_view title,
        std::string_view body,
        const AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::PdfWriteReceipt> fromTextFile(
        std::string_view title,
        const AuthorizedPath& source,
        const AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
