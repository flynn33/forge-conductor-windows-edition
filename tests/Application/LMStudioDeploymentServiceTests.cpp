#include "../Infrastructure/TestSupport.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/DiagnosticsFakes.h"
#include "Fakes/ExternalServiceFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/PlatformPathFakes.h"
#include "Fakes/ToolServiceFakes.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::LMStudioFallbackServerId;
using Infrastructure::Windows::LMStudioPrimaryServerId;
using Infrastructure::Windows::WindowsLMStudioDeploymentService;
using Json = nlohmann::json;
using namespace std::chrono_literals;

static_assert(std::is_final_v<WindowsLMStudioDeploymentService>);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::PathText parentOf(const Domain::PathText& value)
{
    const auto separator = value.value().find_last_of("\\/");
    require(separator != std::string::npos && separator != 0U,
            "The test source path has no usable parent.");
    return path(value.value().substr(0U, separator));
}

[[nodiscard]] Domain::PathText currentTestExecutable()
{
    std::array<wchar_t, 32U * 1024U> native{};
    const auto length = ::GetModuleFileNameW(
        nullptr, native.data(), static_cast<DWORD>(native.size()));
    require(length != 0U && length < native.size(),
            "The test executable path could not be resolved.");
    const auto required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, native.data(), static_cast<int>(length),
        nullptr, 0, nullptr, nullptr);
    require(required > 0, "The test executable path is not valid UTF-16.");
    std::string utf8(static_cast<std::size_t>(required), '\0');
    require(::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, native.data(), static_cast<int>(length),
                utf8.data(), required, nullptr, nullptr) == required,
            "The test executable path could not be represented as UTF-8.");
    return path(utf8);
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> encoded(value.size());
    if (!value.empty()) {
        std::memcpy(encoded.data(), value.data(), value.size());
    }
    return encoded;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    std::string decoded(value.size(), '\0');
    if (!value.empty()) {
        std::memcpy(decoded.data(), value.data(), value.size());
    }
    return decoded;
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Domain::MonotonicTimePoint now,
    const std::stop_token cancellation = {},
    const std::chrono::milliseconds lifetime = 5min)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("95000000-0000-4000-8000-000000000001"),
        now + lifetime,
        cancellation,
        parse<Domain::CorrelationId>("p15-lmstudio-transaction")};
}

class MemoryStorage final : public Contracts::IFileSystem,
                            public Contracts::IAtomicFileStore {
public:
    enum class FailurePhase {
        BeforeMutation,
        AfterMutation
    };

    struct Snapshot final {
        std::map<std::string, std::vector<std::byte>> files;
        std::set<std::string> directories;

        bool operator==(const Snapshot&) const = default;
    };

    void seedFile(const std::string_view file, const std::string_view content)
    {
        std::scoped_lock lock{mutex_};
        files_.insert_or_assign(std::string{file}, bytes(content));
    }

    void seedDirectory(const std::string_view directory)
    {
        std::scoped_lock lock{mutex_};
        directories_.insert(std::string{directory});
    }

    [[nodiscard]] std::optional<std::string> fileText(
        const std::string_view file) const
    {
        std::scoped_lock lock{mutex_};
        const auto found = files_.find(std::string{file});
        return found == files_.end()
            ? std::nullopt
            : std::optional<std::string>{text(found->second)};
    }

    [[nodiscard]] Snapshot snapshot() const
    {
        std::scoped_lock lock{mutex_};
        return Snapshot{files_, directories_};
    }

    void failNextAtomicReplace() noexcept
    {
        std::scoped_lock lock{mutex_};
        failAtomicReplace_ = true;
    }

    void failMoveAt(
        const std::size_t call,
        const FailurePhase phase = FailurePhase::BeforeMutation) noexcept
    {
        std::scoped_lock lock{mutex_};
        failMoveCall_ = call;
        failMovePhase_ = phase;
    }

    void failMutationAt(
        const std::size_t call,
        const FailurePhase phase = FailurePhase::BeforeMutation) noexcept
    {
        std::scoped_lock lock{mutex_};
        failMutationCall_ = call;
        failMutationPhase_ = phase;
    }

    void resetMutationObservations() noexcept
    {
        std::scoped_lock lock{mutex_};
        mutationCalls_ = 0U;
        moveCalls_ = 0U;
        successfulMoveDestinations_.clear();
        replaceRetainBackup_.clear();
        failMoveCall_.reset();
        failMutationCall_.reset();
    }

    [[nodiscard]] std::size_t mutationCalls() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return mutationCalls_;
    }

    [[nodiscard]] std::size_t moveCalls() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return moveCalls_;
    }

    [[nodiscard]] std::vector<std::string> successfulMoveDestinations() const
    {
        std::scoped_lock lock{mutex_};
        return successfulMoveDestinations_;
    }

    [[nodiscard]] std::vector<bool> replaceRetainBackup() const
    {
        std::scoped_lock lock{mutex_};
        return replaceRetainBackup_;
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        return readImpl(file, maximumBytes, context);
    }

    [[nodiscard]] Domain::Result<void> replace(
        const Contracts::AuthorizedPath& file,
        const std::span<const std::byte> content,
        const bool retainBackup,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return live;
            }
            auto mutation = beginMutation();
            if (!mutation) {
                return mutation;
            }
            replaceRetainBackup_.push_back(retainBackup);
            if (failAtomicReplace_) {
                failAtomicReplace_ = false;
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Injected atomic replacement failure."));
            }
            const auto target = file.canonicalPath().value();
            if (retainBackup) {
                const auto previous = files_.find(target);
                if (previous != files_.end()) {
                    files_.insert_or_assign(target + ".bak", previous->second);
                }
            }
            files_.insert_or_assign(
                target,
                std::vector<std::byte>{content.begin(), content.end()});
            return finishMutation();
        } catch (...) {
            return internalFailure();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override
    {
        return readImpl(file, maximumBytes, context);
    }

    [[nodiscard]] Domain::Result<void> writeFile(
        const Contracts::AuthorizedPath& file,
        const std::span<const std::byte> content,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return live;
            }
            auto mutation = beginMutation();
            if (!mutation) {
                return mutation;
            }
            if (files_.contains(file.canonicalPath().value())) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The scripted create target already exists."));
            }
            files_.emplace(
                file.canonicalPath().value(),
                std::vector<std::byte>{content.begin(), content.end()});
            return finishMutation();
        } catch (...) {
            return internalFailure();
        }
    }

    [[nodiscard]] Domain::Result<Domain::DirectoryListing> list(
        const Contracts::AuthorizedPath&,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DirectoryListing>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "List is unsupported."));
    }

    [[nodiscard]] Domain::Result<void> createDirectory(
        const Contracts::AuthorizedPath& directory,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return live;
            }
            auto mutation = beginMutation();
            if (!mutation) {
                return mutation;
            }
            directories_.insert(directory.canonicalPath().value());
            return finishMutation();
        } catch (...) {
            return internalFailure();
        }
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Contracts::AuthorizedPath& target,
        const bool recursive,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return live;
            }
            auto mutation = beginMutation();
            if (!mutation) {
                return mutation;
            }
            const auto name = target.canonicalPath().value();
            bool removed = files_.erase(name) != 0U || directories_.erase(name) != 0U;
            if (recursive) {
                const auto prefix = name + "\\";
                for (auto file = files_.begin(); file != files_.end();) {
                    if (file->first.starts_with(prefix)) {
                        file = files_.erase(file);
                        removed = true;
                    } else {
                        ++file;
                    }
                }
                for (auto directory = directories_.begin(); directory != directories_.end();) {
                    if (directory->starts_with(prefix)) {
                        directory = directories_.erase(directory);
                        removed = true;
                    } else {
                        ++directory;
                    }
                }
            }
            return removed
                ? finishMutation()
                : Domain::Result<void>::failure(Domain::makeError(
                      Domain::ErrorCodes::RecordNotFound,
                      "The scripted remove target does not exist."));
        } catch (...) {
            return internalFailure();
        }
    }

    [[nodiscard]] Domain::Result<void> move(
        const Contracts::AuthorizedPath& source,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return live;
            }
            auto mutation = beginMutation();
            if (!mutation) {
                return mutation;
            }
            ++moveCalls_;
            if (failMoveCall_ && moveCalls_ == failMoveCall_.value() &&
                failMovePhase_ == FailurePhase::BeforeMutation) {
                failMoveCall_.reset();
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Injected directory move failure."));
            }
            const auto sourceName = source.canonicalPath().value();
            const auto destinationName = destination.canonicalPath().value();
            const auto prefix = sourceName + "\\";
            bool exists = files_.contains(sourceName) || directories_.contains(sourceName) ||
                std::any_of(files_.begin(), files_.end(), [&](const auto& item) {
                    return item.first.starts_with(prefix);
                });
            if (!exists) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "The scripted move source does not exist."));
            }
            std::map<std::string, std::vector<std::byte>> movedFiles;
            for (auto file = files_.begin(); file != files_.end();) {
                if (file->first == sourceName || file->first.starts_with(prefix)) {
                    const auto suffix = file->first.substr(sourceName.size());
                    movedFiles.emplace(destinationName + suffix, std::move(file->second));
                    file = files_.erase(file);
                } else {
                    ++file;
                }
            }
            std::set<std::string> movedDirectories;
            for (auto directory = directories_.begin(); directory != directories_.end();) {
                if (*directory == sourceName || directory->starts_with(prefix)) {
                    const auto suffix = directory->substr(sourceName.size());
                    movedDirectories.emplace(destinationName + suffix);
                    directory = directories_.erase(directory);
                } else {
                    ++directory;
                }
            }
            files_.insert(
                std::make_move_iterator(movedFiles.begin()),
                std::make_move_iterator(movedFiles.end()));
            directories_.insert(movedDirectories.begin(), movedDirectories.end());
            successfulMoveDestinations_.push_back(destinationName);
            if (failMoveCall_ && moveCalls_ == failMoveCall_.value() &&
                failMovePhase_ == FailurePhase::AfterMutation) {
                failMoveCall_.reset();
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Injected directory move failure after mutation."));
            }
            return finishMutation();
        } catch (...) {
            return internalFailure();
        }
    }

private:
    [[nodiscard]] Domain::Result<void> beginMutation()
    {
        ++mutationCalls_;
        if (failMutationCall_ && mutationCalls_ == failMutationCall_.value() &&
            failMutationPhase_ == FailurePhase::BeforeMutation) {
            failMutationCall_.reset();
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Injected storage failure before mutation."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> finishMutation()
    {
        if (failMutationCall_ && mutationCalls_ == failMutationCall_.value() &&
            failMutationPhase_ == FailurePhase::AfterMutation) {
            failMutationCall_.reset();
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Injected storage failure after mutation."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] static Domain::Result<void> checkContext(
        const Domain::OperationContext& context)
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled, "Scripted storage operation was cancelled."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "Scripted storage operation deadline expired."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readImpl(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::scoped_lock lock{mutex_};
            auto live = checkContext(context);
            if (!live) {
                return Domain::Result<std::vector<std::byte>>::failure(
                    std::move(live).error());
            }
            const auto found = files_.find(file.canonicalPath().value());
            if (found == files_.end()) {
                return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "The scripted file does not exist."));
            }
            if (found->second.size() > maximumBytes) {
                return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The scripted file exceeds its read bound."));
            }
            return Domain::Result<std::vector<std::byte>>::success(found->second);
        } catch (...) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The scripted file could not be read."));
        }
    }

    [[nodiscard]] static Domain::Result<void> internalFailure()
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The scripted storage mutation failed."));
    }

    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::byte>> files_;
    std::set<std::string> directories_;
    std::vector<std::string> successfulMoveDestinations_;
    std::vector<bool> replaceRetainBackup_;
    std::size_t mutationCalls_{};
    std::size_t moveCalls_{};
    std::optional<std::size_t> failMoveCall_;
    FailurePhase failMovePhase_{FailurePhase::BeforeMutation};
    std::optional<std::size_t> failMutationCall_;
    FailurePhase failMutationPhase_{FailurePhase::BeforeMutation};
    bool failAtomicReplace_{};
};

class FaultingWorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    FaultingWorkspaceAuthority(
        Contracts::IWorkspaceAuthority& delegate,
        Domain::PathText faultPath,
        Domain::Error fault)
        : delegate_{delegate}, faultPath_{std::move(faultPath)}, fault_{std::move(fault)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.authorityFor(projectId, context);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        const bool shellEnabled,
        const std::uint64_t generation,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.narrow(
            authority, trustedRoots, grants, shellEnabled, generation, context);
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        if (request.requestedPath == faultPath_ &&
            request.access == Domain::FileAccess::Read) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(fault_);
        }
        return delegate_.authorize(authority, request, context);
    }

private:
    Contracts::IWorkspaceAuthority& delegate_;
    Domain::PathText faultPath_;
    Domain::Error fault_;
};

class ServeVerifierFake final : public Contracts::ILMStudioServeVerifier {
public:
    std::function<void(std::size_t)> onCall;

    void failAt(const std::size_t call, Domain::Error error)
    {
        std::scoped_lock lock{mutex_};
        failCall_ = call;
        failure_ = std::move(error);
    }

    void block() noexcept
    {
        std::scoped_lock lock{mutex_};
        blocking_ = true;
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectorHealth> verify(
        const Domain::PathText&,
        const Domain::PathText&,
        const Domain::LMStudioConnectorRole role,
        const std::optional<Domain::DeploymentId>& deploymentId,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++calls_;
            const auto call = calls_;
            roles_.push_back(role);
            revisions_.push_back(deploymentId ? deploymentId->value() : std::string{});
            lastAuthorityGeneration_ = authority.generation();
            lastAuthorityRoots_ = authority.trustedRoots();
            lastAuthorityGrants_ = authority.grants();
            lastAuthorityShellEnabled_ = authority.shellEnabled();
            if (blocking_) {
                activeOperation_ = context.operationId;
                changed_.notify_all();
                changed_.wait(lock, [&] { return cancelled_ || shutdown_; });
                activeOperation_.reset();
                return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                    Domain::makeError(Domain::ErrorCodes::Cancelled,
                                      "The scripted verifier was cancelled."));
            }
            const auto callback = onCall;
            const bool shouldFail = failCall_ && failCall_.value() == call;
            const auto failure = failure_;
            lock.unlock();
            if (callback) {
                callback(call);
            }
            if (shouldFail && failure) {
                return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                    failure.value());
            }
            return Domain::Result<Domain::LMStudioConnectorHealth>::success(
                Domain::LMStudioConnectorHealth{
                    role, true, std::string{"2025-11-25"}, 53U, "ready"});
        } catch (...) {
            return Domain::Result<Domain::LMStudioConnectorHealth>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "The scripted verifier failed."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        std::scoped_lock lock{mutex_};
        cancelledOperations_.push_back(operationId.value());
        if (activeOperation_ && activeOperation_.value() == operationId) {
            cancelled_ = true;
            changed_.notify_all();
        }
    }

    void shutdown() noexcept override
    {
        std::scoped_lock lock{mutex_};
        shutdown_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool waitUntilBlocked()
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 5s, [&] { return activeOperation_.has_value(); });
    }

    [[nodiscard]] std::size_t calls() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::vector<Domain::LMStudioConnectorRole> roles() const
    {
        std::scoped_lock lock{mutex_};
        return roles_;
    }

    [[nodiscard]] bool shutdownCalled() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return shutdown_;
    }

    [[nodiscard]] std::vector<std::string> cancelledOperations() const
    {
        std::scoped_lock lock{mutex_};
        return cancelledOperations_;
    }

    [[nodiscard]] std::uint64_t lastAuthorityGeneration() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return lastAuthorityGeneration_;
    }

    [[nodiscard]] std::vector<Domain::PathText> lastAuthorityRoots() const
    {
        std::scoped_lock lock{mutex_};
        return lastAuthorityRoots_;
    }

    [[nodiscard]] std::vector<Domain::FileAccess> lastAuthorityGrants() const
    {
        std::scoped_lock lock{mutex_};
        return lastAuthorityGrants_;
    }

    [[nodiscard]] bool lastAuthorityShellEnabled() const noexcept
    {
        std::scoped_lock lock{mutex_};
        return lastAuthorityShellEnabled_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::size_t> failCall_;
    std::optional<Domain::Error> failure_;
    std::optional<Domain::OperationId> activeOperation_;
    std::vector<Domain::LMStudioConnectorRole> roles_;
    std::vector<std::string> revisions_;
    std::vector<std::string> cancelledOperations_;
    std::vector<Domain::PathText> lastAuthorityRoots_;
    std::vector<Domain::FileAccess> lastAuthorityGrants_;
    std::size_t calls_{};
    std::uint64_t lastAuthorityGeneration_{};
    bool lastAuthorityShellEnabled_{};
    bool blocking_{};
    bool cancelled_{};
    bool shutdown_{};
};

class HostActivatorFake final : public Contracts::ILMStudioHostActivator {
public:
    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioEnvironmentStatus& environment,
        const Domain::LMStudioHostActivationRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext&) noexcept override
    {
        ++calls_;
        lastEnvironment_ = environment;
        lastAuthorityIntent_ = authority.intent();
        lastAuthorityGeneration_ = authority.generation();
        lastAuthorityRoots_ = authority.trustedRoots();
        lastAuthorityGrants_ = authority.grants();
        lastAuthorityShellEnabled_ = authority.shellEnabled();
        return Domain::Result<Domain::LMStudioHostActivationResult>::success(
            Domain::LMStudioHostActivationResult{
                request.deploymentId, true, false, false, true,
                {Domain::LMStudioConnectorRole::Primary,
                 Domain::LMStudioConnectorRole::Fallback},
                "host synchronized"});
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        cancelledOperations_.push_back(operationId.value());
    }

    void shutdown() noexcept override { shutdown_ = true; }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
    [[nodiscard]] bool shutdownCalled() const noexcept { return shutdown_; }
    [[nodiscard]] std::uint64_t lastAuthorityGeneration() const noexcept
    {
        return lastAuthorityGeneration_;
    }
    [[nodiscard]] const std::vector<Domain::PathText>& lastAuthorityRoots() const noexcept
    {
        return lastAuthorityRoots_;
    }
    [[nodiscard]] const std::vector<Domain::FileAccess>& lastAuthorityGrants() const noexcept
    {
        return lastAuthorityGrants_;
    }
    [[nodiscard]] bool lastAuthorityShellEnabled() const noexcept
    {
        return lastAuthorityShellEnabled_;
    }

private:
    std::optional<Domain::LMStudioEnvironmentStatus> lastEnvironment_;
    std::optional<Domain::FileAccess> lastAuthorityIntent_;
    std::vector<std::string> cancelledOperations_;
    std::vector<Domain::PathText> lastAuthorityRoots_;
    std::vector<Domain::FileAccess> lastAuthorityGrants_;
    std::size_t calls_{};
    std::uint64_t lastAuthorityGeneration_{};
    bool lastAuthorityShellEnabled_{};
    bool shutdown_{};
};

[[nodiscard]] Contracts::AuthorizedToolCall authorizedCall(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context,
    const Domain::ToolEffect effect,
    const std::string_view toolName = "install-lmstudio-plugin",
    const bool includeProject = true)
{
    Fakes::DeterministicToolAuthorizerFake authorizer{std::string{toolName}, effect};
    Domain::ToolCallRequest call{
        Domain::McpRequestMetadata{
            parse<Domain::RequestId>("p15-lmstudio-request"),
            context.correlationId,
            authority.callerId(),
            includeProject ? std::optional<Domain::ProjectId>{authority.projectId()}
                           : std::nullopt,
            "2025-11-25"},
        std::string{toolName},
        "{}"};
    return take(authorizer.authorize(
        Domain::ToolAuthorizationRequest{
            call,
            effect,
            Domain::AuthorityReference{authority.authorityId(), authority.generation()}},
        authority,
        context));
}

struct Fixture final {
    Fixture()
        : now{std::chrono::steady_clock::now()},
          clock{
              Domain::UtcTimePoint{std::chrono::milliseconds{123'456}},
              now},
          authorityProvider{
              parse<Domain::AuthorityId>("96000000-0000-4000-8000-000000000001"),
              parse<Domain::ClientId>("p15-lmstudio-maintenance"),
              {lmStudioRoot, forgeRoot, sourceRoot, binaryRoot},
              Domain::FileAccess::Write,
              {Domain::FileAccess::Read, Domain::FileAccess::Write,
               Domain::FileAccess::Create, Domain::FileAccess::Delete,
               Domain::FileAccess::Execute},
              {},
              true,
              15U},
          authority{take(authorityProvider.authorityFor(projectId, context()))},
          uuids{{
              parse<Domain::Uuid>("97000000-0000-4000-8000-000000000001"),
              parse<Domain::Uuid>("97000000-0000-4000-8000-000000000002"),
              parse<Domain::Uuid>("97000000-0000-4000-8000-000000000003"),
              parse<Domain::Uuid>("97000000-0000-4000-8000-000000000004")}},
          diagnostics{128U},
          service{
              environment,
              verifier,
              host,
              authorityProvider,
              storage,
              storage,
              paths,
              clock,
              uuids,
              diagnostics}
    {
        environment.inspectResult.set(
            Domain::Result<Domain::LMStudioEnvironmentStatus>::success(
                Domain::LMStudioEnvironmentStatus{
                    true,
                    lmStudioRoot,
                    configurationPath,
                    std::string{"0.4.21+2"},
                    path("C:\\fixture\\LM Studio.exe"),
                    {}}));
        paths.dataRootResult.set(
            Domain::Result<Domain::PathText>::success(forgeHome));
        storage.seedDirectory(lmStudioRoot.value());
        storage.seedDirectory(forgeRoot.value());
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::stop_token cancellation = {},
        const std::chrono::milliseconds lifetime = 5min) const
    {
        return operationContext(now, cancellation, lifetime);
    }

    [[nodiscard]] Domain::LMStudioDeploymentRequest request() const
    {
        return Domain::LMStudioDeploymentRequest{binary, true};
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::OperationContext& operation)
    {
        auto authorization = authorizedCall(
            authority, operation, Domain::ToolEffect::Write);
        return service.deploy(request(), authority, authorization, operation);
    }

    Domain::MonotonicTimePoint now;
    Domain::PathText lmStudioRoot{path("C:\\fixture\\.lmstudio")};
    Domain::PathText forgeRoot{path("C:\\fixture")};
    Domain::PathText configurationPath{path("C:\\fixture\\.lmstudio\\mcp.json")};
    Domain::PathText binary{currentTestExecutable()};
    Domain::PathText binaryRoot{parentOf(binary)};
    Domain::PathText forgeHome{path("C:\\fixture\\forge-home")};
    Domain::PathText sourceFile{path(__FILE__)};
    Domain::PathText sourceRoot{parentOf(sourceFile)};
    Domain::ProjectId projectId{
        parse<Domain::ProjectId>("98000000-0000-4000-8000-000000000001")};
    Fakes::FakeClock clock;
    Fakes::DeterministicWorkspaceAuthority authorityProvider;
    Contracts::WorkspaceAuthority authority;
    MemoryStorage storage;
    Fakes::RecordingLMStudioEnvironmentFake environment;
    ServeVerifierFake verifier;
    HostActivatorFake host;
    Fakes::RecordingApplicationPathsFake paths;
    Fakes::SequenceUuidGenerator uuids;
    Fakes::DiagnosticSinkFake diagnostics;
    WindowsLMStudioDeploymentService service;
};

constexpr std::string_view ForeignPluginSentinel{
    "C:\\fixture\\.lmstudio\\extensions\\plugins\\mcp\\continuity\\sentinel.txt"};

[[nodiscard]] std::array<Domain::PathText, 4U> legacyWrapperFiles(
    const Fixture& fixture)
{
    return {
        path(fixture.binaryRoot.value() + "\\forge-serve"),
        path(fixture.binaryRoot.value() + "\\forge-serve-fallback"),
        path(fixture.binaryRoot.value() + "\\forge-serve.cmd"),
        path(fixture.binaryRoot.value() + "\\forge-serve-fallback.cmd")};
}

[[nodiscard]] Domain::PathText adjacentWrapperSentinel(const Fixture& fixture)
{
    return path(fixture.binaryRoot.value() + "\\forge-serve-neighbor.cmd");
}

void seedForeignState(Fixture& fixture)
{
    fixture.storage.seedFile(
        fixture.configurationPath.value(),
        R"({
  "foreignRoot": {"keep": true},
  "mcpServers": {
    "continuity": {"command": "C:\\foreign.exe", "args": [], "unknown": 7},
    "legacy-wrapper": {"command": "C:\\old\\forge-serve-fallback.cmd", "args": []},
    "forge-serve-custom": {"command": "C:\\vendor\\forge-serve-helper.cmd", "args": []},
    "forge-serve": {"command": "C:\\fixture\\forge-home\\bin\\forge-serve.cmd", "args": []}
  }
    })");
    fixture.storage.seedFile(
        fixture.configurationPath.value() + ".bak", "preexisting-backup-sentinel");
    fixture.storage.seedFile(ForeignPluginSentinel, "foreign-sentinel");
    const auto wrappers = legacyWrapperFiles(fixture);
    fixture.storage.seedFile(
        wrappers[0].value(),
        "@echo off\r\npython -m forge_conductor.mcp_server %*\r\n");
    fixture.storage.seedFile(
        wrappers[1].value(), "@echo off\r\npython -m foreign_mcp %*\r\n");
    fixture.storage.seedFile(wrappers[2].value(), "arbitrary-foreign-content");
    fixture.storage.seedFile(wrappers[3].value(), "arbitrary-foreign-content");
    fixture.storage.seedFile(
        adjacentWrapperSentinel(fixture).value(), "foreign-neighbor");
}

void testTransactionalDeployPreservesForeignAndOrdersFallbackFirst()
{
    Fixture fixture;
    seedForeignState(fixture);
    const auto result = take(fixture.deploy(fixture.context()));
    require(result.ok && result.deploymentId.value() ==
                "97000000-0000-4000-8000-000000000001",
            "The deployment did not publish its fresh deterministic revision.");

    const auto configuration = Json::parse(
        fixture.storage.fileText(fixture.configurationPath.value()).value());
    require(configuration.at("foreignRoot").at("keep").get<bool>() &&
                configuration.at("mcpServers").at("continuity").at("unknown").get<int>() == 7,
            "The deployment changed foreign configuration content.");
    require(configuration.at("mcpServers").contains("legacy-wrapper") &&
                configuration.at("mcpServers").contains("forge-serve-custom"),
            "A foreign legacy-looking or prefix-collision registration was removed.");
    require(!configuration.at("mcpServers").contains("forge-serve"),
            "An exact Forge-owned legacy wrapper registration survived replacement.");
    require(fixture.storage.fileText(ForeignPluginSentinel) == "foreign-sentinel",
            "The deployment changed a foreign LM Studio plugin directory.");
    const auto wrappers = legacyWrapperFiles(fixture);
    require(!fixture.storage.fileText(wrappers[0].value()),
            "A signature-proven Forge-owned physical legacy wrapper survived replacement.");
    for (std::size_t index = 1U; index < wrappers.size(); ++index) {
        require(fixture.storage.fileText(wrappers[index].value()).has_value(),
                "A foreign file with a reserved-looking wrapper name was removed.");
    }
    require(fixture.storage.fileText(adjacentWrapperSentinel(fixture).value()) ==
                "foreign-neighbor",
            "Exact legacy-wrapper cleanup changed an adjacent foreign file.");
    require(fixture.storage.fileText(fixture.configurationPath.value() + ".bak") ==
                "preexisting-backup-sentinel",
            "Deployment changed the caller's preexisting mcp.json.bak state.");
    const auto backupRequests = fixture.storage.replaceRetainBackup();
    require(std::all_of(
                backupRequests.begin(), backupRequests.end(),
                [](const bool retain) { return !retain; }),
            "Deployment requested an atomic-store backup side effect.");

    const auto roles = fixture.verifier.roles();
    require(roles == std::vector<Domain::LMStudioConnectorRole>{
                         Domain::LMStudioConnectorRole::Primary,
                         Domain::LMStudioConnectorRole::Fallback,
                         Domain::LMStudioConnectorRole::Primary,
                         Domain::LMStudioConnectorRole::Fallback},
            "The service did not pre-smoke and post-smoke both exact roles.");
    const auto maintenanceRoots = fixture.verifier.lastAuthorityRoots();
    require(fixture.verifier.lastAuthorityGeneration() ==
                fixture.authority.generation() + 1U &&
                maintenanceRoots.size() == 3U &&
                std::find(maintenanceRoots.begin(), maintenanceRoots.end(),
                          fixture.binaryRoot) != maintenanceRoots.end() &&
                std::find(maintenanceRoots.begin(), maintenanceRoots.end(),
                          fixture.lmStudioRoot) != maintenanceRoots.end() &&
                std::find(maintenanceRoots.begin(), maintenanceRoots.end(),
                          fixture.forgeRoot) != maintenanceRoots.end() &&
                std::find(maintenanceRoots.begin(), maintenanceRoots.end(),
                          fixture.sourceRoot) == maintenanceRoots.end(),
            "Deployment did not bind a fresh project authority to only its three maintenance roots.");
    require(fixture.verifier.lastAuthorityGrants() ==
                std::vector<Domain::FileAccess>{
                    Domain::FileAccess::Read,
                    Domain::FileAccess::Write,
                    Domain::FileAccess::Create,
                    Domain::FileAccess::Delete,
                    Domain::FileAccess::Execute} &&
                fixture.verifier.lastAuthorityShellEnabled(),
            "Deployment smoke verification did not receive the exact narrow maintenance grants.");

    const auto moves = fixture.storage.successfulMoveDestinations();
    const auto fallbackTarget =
        "C:\\fixture\\.lmstudio\\extensions\\plugins\\mcp\\forge-conductor-fallback";
    const auto primaryTarget =
        "C:\\fixture\\.lmstudio\\extensions\\plugins\\mcp\\forge-conductor";
    const auto fallback = std::find(moves.begin(), moves.end(), fallbackTarget);
    const auto primary = std::find(moves.begin(), moves.end(), primaryTarget);
    require(fallback != moves.end() && primary != moves.end() && fallback < primary,
            "The active fallback plugin was not committed before primary.");

    const auto installState = Json::parse(
        fixture.storage.fileText(
            std::string{primaryTarget} + "\\install-state.json").value());
    require(installState.at("at").get<std::int64_t>() == 123'456,
            "The plugin install timestamp did not come from the injected clock.");
}

void testRepeatedDeployUsesFreshRevisionAndStatusDetectsDrift()
{
    Fixture fixture;
    seedForeignState(fixture);
    const auto first = take(fixture.deploy(fixture.context()));
    const auto second = take(fixture.deploy(fixture.context()));
    require(first.deploymentId != second.deploymentId,
            "Repeated deployment reused a stale revision.");
    auto configuration = Json::parse(
        fixture.storage.fileText(fixture.configurationPath.value()).value());
    configuration["mcpServers"][LMStudioPrimaryServerId]["env"]["FORGE_MCP_ROLE"] =
        "fallback";
    fixture.storage.seedFile(fixture.configurationPath.value(), configuration.dump());
    const auto status = take(fixture.service.status(
        fixture.request(), fixture.authority, fixture.context()));
    require(!status.mcpConfigurationRegistered && !status.primaryPluginInstalled,
            "Status accepted a wrong role or stale shared configuration revision.");
}

void testAtomicAndPostSmokeFaultsRestoreExactSnapshot()
{
    Fixture fixture;
    seedForeignState(fixture);
    static_cast<void>(take(fixture.deploy(fixture.context())));
    const auto baseline = fixture.storage.snapshot();

    fixture.storage.failMoveAt(fixture.storage.moveCalls() + 4U);
    require(!fixture.deploy(fixture.context()),
            "An injected primary staged-to-target move failure was accepted.");
    require(fixture.storage.snapshot() == baseline,
            "Directory commit failure after both targets were backed up did not restore exact state.");

    fixture.storage.failNextAtomicReplace();
    require(!fixture.deploy(fixture.context()),
            "An injected atomic configuration failure was accepted.");
    require(fixture.storage.snapshot() == baseline,
            "Atomic configuration failure did not restore exact plugin/configuration state.");

    fixture.verifier.failAt(
        fixture.verifier.calls() + 3U,
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                          "Injected post-smoke failure."));
    require(!fixture.deploy(fixture.context()),
            "An injected post-smoke failure was accepted.");
    require(fixture.storage.snapshot() == baseline,
            "Post-smoke failure did not restore exact plugin/configuration state.");
}

void testFreshInstallAndAmbiguousBackupFaultsRestoreExactState()
{
    {
        Fixture fixture;
        const auto baseline = fixture.storage.snapshot();
        fixture.storage.failMoveAt(4U);
        require(!fixture.deploy(fixture.context()),
                "A fresh install accepted failure after fallback commit but before primary commit.");
        require(fixture.storage.snapshot() == baseline,
                "Fresh-install rollback did not restore the exact no-file/no-plugin state.");
    }

    {
        Fixture fixture;
        seedForeignState(fixture);
        static_cast<void>(take(fixture.deploy(fixture.context())));
        const auto baseline = fixture.storage.snapshot();
        fixture.storage.resetMutationObservations();
        fixture.storage.failMoveAt(
            1U, MemoryStorage::FailurePhase::AfterMutation);
        require(!fixture.deploy(fixture.context()),
                "A failure after moving the fallback target to backup was accepted.");
        require(fixture.storage.snapshot() == baseline,
                "Ambiguous backup-move rollback did not restore the exact prior deployment.");
    }
}

void testEveryMutationBoundaryEitherRollsBackOrSurfacesCommittedCleanup()
{
    for (const auto phase : {
             MemoryStorage::FailurePhase::BeforeMutation,
             MemoryStorage::FailurePhase::AfterMutation}) {
        std::size_t exactRollbacks{};
        std::size_t committedCleanupFailures{};
        bool reachedSuccessfulCallBeyondMatrix{};

        for (std::size_t failureCall = 1U; failureCall <= 32U; ++failureCall) {
            Fixture fixture;
            seedForeignState(fixture);
            static_cast<void>(take(fixture.deploy(fixture.context())));
            const auto baseline = fixture.storage.snapshot();
            fixture.storage.resetMutationObservations();
            fixture.storage.failMutationAt(failureCall, phase);

            const auto result = fixture.deploy(fixture.context());
            if (result) {
                reachedSuccessfulCallBeyondMatrix = true;
                break;
            }

            const auto after = fixture.storage.snapshot();
            if (after == baseline) {
                ++exactRollbacks;
                continue;
            }

            requireError(
                result,
                Domain::ErrorCodes::IntegrityFailure,
                "A non-rollback mutation failure was not reported as a committed cleanup integrity failure.");
            const auto status = take(fixture.service.status(
                fixture.request(), fixture.authority, fixture.context()));
            require(status.primaryPluginInstalled && status.fallbackPluginInstalled &&
                        status.mcpConfigurationRegistered && status.deploymentId &&
                        status.deploymentId->value() ==
                            "97000000-0000-4000-8000-000000000002",
                    "A surfaced post-commit cleanup failure damaged the validated deployment.");
            require(fixture.storage.fileText(
                        fixture.configurationPath.value() + ".bak") ==
                        "preexisting-backup-sentinel",
                    "A faulted transaction changed the preexisting atomic backup state.");
            ++committedCleanupFailures;
        }

        require(reachedSuccessfulCallBeyondMatrix,
                "The storage mutation boundary matrix did not reach an unfaulted deployment.");
        require(exactRollbacks == 15U && committedCleanupFailures == 1U,
                "The storage mutation boundary matrix did not cover every rollback and cleanup boundary exactly.");
    }
}

void testRollbackAndPostCommitCleanupFailuresAreTypedAndDiagnosed()
{
    {
        Fixture fixture;
        seedForeignState(fixture);
        static_cast<void>(take(fixture.deploy(fixture.context())));
        const auto postPrimaryCall = fixture.verifier.calls() + 3U;
        fixture.verifier.failAt(
            postPrimaryCall,
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Injected post-smoke failure before rollback."));
        fixture.verifier.onCall = [&](const std::size_t call) {
            if (call == postPrimaryCall) {
                fixture.storage.failMutationAt(
                    fixture.storage.mutationCalls() + 1U);
            }
        };

        requireError(
            fixture.deploy(fixture.context()),
            Domain::ErrorCodes::IntegrityFailure,
            "A rollback restoration failure was suppressed.");
        const auto events = take(fixture.diagnostics.recent(
            128U, fixture.context()));
        require(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.event == "lmstudio_deploy_rollback_failed";
                }),
                "Rollback integrity failure was not recorded diagnostically.");
    }

    {
        Fixture fixture;
        seedForeignState(fixture);
        static_cast<void>(take(fixture.deploy(fixture.context())));
        const auto baseline = fixture.storage.snapshot();
        const auto postFallbackCall = fixture.verifier.calls() + 4U;
        fixture.verifier.onCall = [&](const std::size_t call) {
            if (call == postFallbackCall) {
                fixture.storage.failMutationAt(
                    fixture.storage.mutationCalls() + 1U);
            }
        };

        requireError(
            fixture.deploy(fixture.context()),
            Domain::ErrorCodes::IntegrityFailure,
            "A post-commit transaction-backup cleanup failure was suppressed.");
        require(fixture.storage.snapshot() != baseline,
                "Post-commit cleanup failure incorrectly attempted rollback after validation.");
        const auto status = take(fixture.service.status(
            fixture.request(), fixture.authority, fixture.context()));
        require(status.primaryPluginInstalled && status.fallbackPluginInstalled &&
                    status.mcpConfigurationRegistered && status.deploymentId &&
                    status.deploymentId->value() ==
                        "97000000-0000-4000-8000-000000000002",
                "Retained transaction backups coincided with a damaged live deployment.");
        const auto events = take(fixture.diagnostics.recent(
            128U, fixture.context()));
        require(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.event == "lmstudio_deploy_cleanup_failed";
                }),
                "Post-commit cleanup integrity failure was not recorded diagnostically.");
    }
}

void testStatusRejectsAuthorizedMissingAndNonBinaryFiles()
{
    Fixture fixture;

    auto missingRequest = fixture.request();
    missingRequest.preferredBinary = path(
        fixture.binaryRoot.value() + "\\missing-forge-conductor.exe");
    const auto missing = take(fixture.service.status(
        missingRequest, fixture.authority, fixture.context()));
    require(!missing.binaryExecutable,
            "Status reported an authorized missing executable as runnable.");

    const auto executable = take(fixture.service.status(
        fixture.request(), fixture.authority, fixture.context()));
    require(executable.binaryExecutable && executable.binaryPath == fixture.binary,
            "Status rejected the authorized canonical x64 PE test executable.");

    auto nonBinaryRequest = fixture.request();
    nonBinaryRequest.preferredBinary = fixture.sourceFile;
    const auto nonBinary = take(fixture.service.status(
        nonBinaryRequest, fixture.authority, fixture.context()));
    require(!nonBinary.binaryExecutable,
            "Status reported an authorized regular non-PE file as runnable.");

    auto outsideRequest = fixture.request();
    outsideRequest.preferredBinary = path("D:\\outside-authority\\forge-conductor.exe");
    const auto outside = take(fixture.service.status(
        outsideRequest, fixture.authority, fixture.context()));
    require(!outside.binaryExecutable,
            "Status reported an out-of-authority executable candidate as runnable.");
}

void testStatusPropagatesSystemicBinaryAuthorizationFault()
{
    Fixture fixture;
    FaultingWorkspaceAuthority faultingAuthority{
        fixture.authorityProvider,
        fixture.binary,
        Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Injected systemic binary authorization failure.")};
    WindowsLMStudioDeploymentService service{
        fixture.environment,
        fixture.verifier,
        fixture.host,
        faultingAuthority,
        fixture.storage,
        fixture.storage,
        fixture.paths,
        fixture.clock,
        fixture.uuids,
        fixture.diagnostics};

    requireError(
        service.status(fixture.request(), fixture.authority, fixture.context()),
        Domain::ErrorCodes::InternalFailure,
        "Status converted a systemic binary authorization failure to degraded success.");
}

void testCancelledAndExpiredPostCommitFaultsUseFreshRollbackContext()
{
    {
        Fixture fixture;
        seedForeignState(fixture);
        static_cast<void>(take(fixture.deploy(fixture.context())));
        const auto baseline = fixture.storage.snapshot();
        std::stop_source cancellation;
        fixture.verifier.failAt(
            fixture.verifier.calls() + 3U,
            Domain::makeError(Domain::ErrorCodes::Cancelled,
                              "Injected post-commit cancellation."));
        fixture.verifier.onCall = [&](const std::size_t call) {
            if (call == 7U) {
                cancellation.request_stop();
            }
        };
        requireError(
            fixture.deploy(fixture.context(cancellation.get_token())),
            Domain::ErrorCodes::Cancelled,
            "Post-commit cancellation was not propagated.");
        require(fixture.storage.snapshot() == baseline,
                "Cancelled caller context prevented exact rollback.");
    }

    {
        Fixture fixture;
        seedForeignState(fixture);
        static_cast<void>(take(fixture.deploy(fixture.context())));
        const auto baseline = fixture.storage.snapshot();
        fixture.verifier.failAt(
            fixture.verifier.calls() + 3U,
            Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                              "Injected post-commit deadline."));
        fixture.verifier.onCall = [&](const std::size_t call) {
            if (call == 7U) {
                fixture.clock.advance(6min);
            }
        };
        requireError(
            fixture.deploy(fixture.context({}, 5min)),
            Domain::ErrorCodes::DeadlineExceeded,
            "Post-commit deadline was not propagated.");
        require(fixture.storage.snapshot() == baseline,
                "Expired caller context prevented exact rollback.");
    }
}

void testMalformedConfigAndAuthorizationFailuresNeverMutate()
{
    Fixture fixture;
    fixture.storage.seedFile(fixture.configurationPath.value(), "{malformed");
    const auto baseline = fixture.storage.snapshot();
    requireError(
        fixture.deploy(fixture.context()),
        Domain::ErrorCodes::MalformedMessage,
        "Malformed foreign configuration was replaced.");
    require(fixture.storage.snapshot() == baseline,
            "Malformed configuration caused filesystem mutation.");

    auto operation = fixture.context();
    auto wrongName = authorizedCall(
        fixture.authority, operation, Domain::ToolEffect::Write, "wrong-tool");
    requireError(
        fixture.service.deploy(fixture.request(), fixture.authority, wrongName, operation),
        Domain::ErrorCodes::Unauthorized,
        "A capability for the wrong tool name was accepted.");
    auto projectless = authorizedCall(
        fixture.authority, operation, Domain::ToolEffect::Write,
        "install-lmstudio-plugin", false);
    requireError(
        fixture.service.deploy(fixture.request(), fixture.authority, projectless, operation),
        Domain::ErrorCodes::Unauthorized,
        "A projectless deployment capability was accepted.");
    auto noPreservation = fixture.request();
    noPreservation.preserveForeignEntries = false;
    auto correct = authorizedCall(
        fixture.authority, operation, Domain::ToolEffect::Write);
    requireError(
        fixture.service.deploy(noPreservation, fixture.authority, correct, operation),
        Domain::ErrorCodes::InvalidRequest,
        "Foreign-entry preservation was allowed to be disabled.");
    require(fixture.storage.snapshot() == baseline,
            "Rejected authorization or preservation policy mutated storage.");
}

void testActivationBindingAndShutdownDrainExactOperation()
{
    {
        const auto now = std::chrono::steady_clock::now();
        const auto root = path("C:\\fixture");
        const auto project = parse<Domain::ProjectId>(
            "99000000-0000-4000-8000-000000000001");
        Fakes::FakeClock clock{
            Domain::UtcTimePoint{std::chrono::milliseconds{1}}, now};
        Fakes::DeterministicWorkspaceAuthority provider{
            parse<Domain::AuthorityId>("99000000-0000-4000-8000-000000000002"),
            parse<Domain::ClientId>("p15-lmstudio-activation"),
            {root},
            Domain::FileAccess::Execute,
            {Domain::FileAccess::Read, Domain::FileAccess::Execute},
            {}, true, 15U};
        const auto operation = operationContext(now);
        const auto authority = take(provider.authorityFor(project, operation));
        MemoryStorage storage;
        Fakes::RecordingLMStudioEnvironmentFake environment;
        environment.inspectResult.set(
            Domain::Result<Domain::LMStudioEnvironmentStatus>::success(
                Domain::LMStudioEnvironmentStatus{
                    true, root, path("C:\\fixture\\mcp.json"),
                    std::string{"0.4.21+2"},
                    path("C:\\fixture\\LM Studio.exe"), {}}));
        ServeVerifierFake verifier;
        HostActivatorFake host;
        Fakes::RecordingApplicationPathsFake paths;
        paths.dataRootResult.set(Domain::Result<Domain::PathText>::success(root));
        Fakes::SequenceUuidGenerator uuids{{
            parse<Domain::Uuid>("99000000-0000-4000-8000-000000000003")}};
        Fakes::DiagnosticSinkFake diagnostics{16U};
        WindowsLMStudioDeploymentService service{
            environment, verifier, host, provider, storage, storage,
            paths, clock, uuids, diagnostics};
        auto request = Domain::LMStudioHostActivationRequest{
            parse<Domain::DeploymentId>("activation-revision"), 100ms};
        const auto capability = authorizedCall(
            authority, operation, Domain::ToolEffect::Execute);
        const auto activated = take(service.activate(
            request, authority, capability, operation));
        require(activated.configurationSynchronized && host.calls() == 1U,
                "Execute-bound activation was not delegated exactly once.");
        require(host.lastAuthorityGeneration() == authority.generation() + 1U &&
                    host.lastAuthorityRoots() ==
                        std::vector<Domain::PathText>{root} &&
                    host.lastAuthorityGrants() ==
                        std::vector<Domain::FileAccess>{
                            Domain::FileAccess::Read,
                            Domain::FileAccess::Execute} &&
                    !host.lastAuthorityShellEnabled(),
                "Activation did not delegate with one fresh, project-bound, non-shell maintenance authority.");
        const auto wrongName = authorizedCall(
            authority, operation, Domain::ToolEffect::Execute, "wrong-tool");
        requireError(
            service.activate(request, authority, wrongName, operation),
            Domain::ErrorCodes::Unauthorized,
            "Activation accepted a capability for the wrong tool name.");
    }

    {
        Fixture fixture;
        seedForeignState(fixture);
        fixture.verifier.block();
        const auto operation = fixture.context();
        std::optional<Domain::Result<Domain::LMStudioInstallResult>> outcome;
        std::jthread deployment{[&] { outcome.emplace(fixture.deploy(operation)); }};
        require(fixture.verifier.waitUntilBlocked(),
                "The deployment did not block in its owned verifier call.");
        fixture.service.shutdown();
        deployment.join();
        require(outcome.has_value(), "Shutdown returned before the active deployment drained.");
        requireError(outcome.value(), Domain::ErrorCodes::Cancelled,
                     "Shutdown did not cancel the exact active deployment.");
        require(fixture.verifier.shutdownCalled() && fixture.host.shutdownCalled(),
                "Service shutdown did not coordinate verifier and host-activator shutdown.");
        require(fixture.verifier.cancelledOperations() ==
                    std::vector<std::string>{operation.operationId.value()},
                "Shutdown did not target the exact active OperationId once.");
        require(fixture.storage.mutationCalls() == 0U,
                "Shutdown during pre-smoke allowed deployment mutation.");
        requireError(
            fixture.service.status(fixture.request(), fixture.authority, fixture.context()),
            Domain::ErrorCodes::Cancelled,
            "The service accepted work after shutdown.");
    }
}

} // namespace

void registerLMStudioDeploymentServiceTests(TestRegistry& tests)
{
    addTest(tests, "lmstudio.deploy.transaction-and-fallback-order",
            testTransactionalDeployPreservesForeignAndOrdersFallbackFirst);
    addTest(tests, "lmstudio.deploy.fresh-revision-and-drift",
            testRepeatedDeployUsesFreshRevisionAndStatusDetectsDrift);
    addTest(tests, "lmstudio.deploy.atomic-post-smoke-rollback",
            testAtomicAndPostSmokeFaultsRestoreExactSnapshot);
    addTest(tests, "lmstudio.deploy.fresh-and-ambiguous-move-rollback",
            testFreshInstallAndAmbiguousBackupFaultsRestoreExactState);
    addTest(tests, "lmstudio.deploy.complete-mutation-fault-matrix",
            testEveryMutationBoundaryEitherRollsBackOrSurfacesCommittedCleanup);
    addTest(tests, "lmstudio.deploy.rollback-cleanup-integrity",
            testRollbackAndPostCommitCleanupFailuresAreTypedAndDiagnosed);
    addTest(tests, "lmstudio.deploy.status-rejects-missing-nonbinary",
            testStatusRejectsAuthorizedMissingAndNonBinaryFiles);
    addTest(tests, "lmstudio.deploy.status-propagates-systemic-authority-fault",
            testStatusPropagatesSystemicBinaryAuthorizationFault);
    addTest(tests, "lmstudio.deploy.cancel-deadline-rollback",
            testCancelledAndExpiredPostCommitFaultsUseFreshRollbackContext);
    addTest(tests, "lmstudio.deploy.malformed-and-authorization",
            testMalformedConfigAndAuthorizationFailuresNeverMutate);
    addTest(tests, "lmstudio.deploy.activation-and-shutdown-drain",
            testActivationBindingAndShutdownDrainExactOperation);
}

} // namespace ForgeConductor::Tests
