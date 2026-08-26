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

    [[nodiscard]] virtual Domain::Result<std::string> status(
        const AuthorizedPath& repository,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<std::string> diff(
        const AuthorizedPath& repository,
        std::span<const std::string> arguments,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<std::string> log(
        const AuthorizedPath& repository,
        std::size_t maximumEntries,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> add(
        const AuthorizedPath& repository,
        std::span<const AuthorizedPath> paths,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<std::string> commit(
        const AuthorizedPath& repository,
        std::string_view message,
        const Domain::OperationContext& context) noexcept = 0;
};

class IShellService {
public:
    virtual ~IShellService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> execute(
        const Domain::ProcessRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class IPdfService {
public:
    virtual ~IPdfService() = default;

    [[nodiscard]] virtual Domain::Result<void> write(
        std::string_view title,
        std::string_view body,
        const AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> fromTextFile(
        const AuthorizedPath& source,
        const AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
