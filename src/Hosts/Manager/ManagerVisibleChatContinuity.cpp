#include "ManagerVisibleChatContinuity.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>

namespace ForgeConductor::Hosts::Manager
{
namespace
{
using Json = nlohmann::json;
using Snapshot = ForgeConductor::Manager::ManagerVisibleChatSnapshot;
using Observation = ForgeConductor::Manager::ManagerVisibleChatObserveRequest;
template <typename T> Domain::Result<T> rejected(std::string message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::Unauthorized, std::move(message)));
}
std::filesystem::path path(const Domain::PathText& value)
{
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(value.value().data()), value.value().size()}};
}
Domain::Result<Infrastructure::Windows::LMStudioConfigurationDocument> routes(
    const Domain::PathText& studio)
{
    using Codec = Infrastructure::Windows::LMStudioConfigurationCodec;
    std::ifstream input{path(studio) / "mcp.json", std::ios::binary};
    std::string bytes(Codec::MaximumDocumentBytes + 1U, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (!input.eof() || bytes.size() > Codec::MaximumDocumentBytes)
    {
        return rejected<Infrastructure::Windows::LMStudioConfigurationDocument>(
            "Current native routes are unavailable or oversized.");
    }
    return Codec::parse(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}
} // namespace
ManagerVisibleChatContinuity::ManagerVisibleChatContinuity(
    Domain::PathText home, Domain::PathText studio, Domain::PathText lmExecutable,
    Domain::PathText forgeExecutable, Contracts::IProjectRegistryRepository& registry,
    Contracts::IWorkspaceAuthority& authority, Contracts::ILegacyMemoryService& memory,
    Contracts::ILegacyContextContinuityService& continuity,
    Contracts::IProjectMemoryService& projects, Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuid, Contracts::IConfigurationStore& configuration)
    : home_{std::move(home)}, studio_{std::move(studio)}, lmExecutable_{std::move(lmExecutable)},
      forgeExecutable_{std::move(forgeExecutable)}, registry_{registry}, authority_{authority},
      memory_{memory}, continuity_{continuity}, projects_{projects}, clock_{clock}, uuid_{uuid},
      configuration_{configuration}
{
}
ManagerVisibleChatContinuity::~ManagerVisibleChatContinuity() noexcept
{
    shutdown();
}
Domain::OperationContext ManagerVisibleChatContinuity::context()
{
    auto id = uuid_.next();
    if (!id)
    {
        throw std::runtime_error{id.error().message};
    }
    auto operation = Domain::OperationId::parse(id.value().value());
    if (!operation)
    {
        throw std::runtime_error{operation.error().message};
    }
    auto correlation = Domain::CorrelationId::parse(id.value().value());
    if (!correlation)
    {
        throw std::runtime_error{correlation.error().message};
    }
    return {std::move(operation).value(),
            clock_.monotonicNow() + std::chrono::seconds{20},
            {},
            std::move(correlation).value()};
}
Domain::Result<void> ManagerVisibleChatContinuity::validate(
    const Domain::ProjectId& project, const Domain::PathText& root,
    const Domain::OperationContext& operation)
{
    if (stopping_ || operation.cancellation.stop_requested())
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled, "Visible chat admission was cancelled."));
    }
    if (clock_.monotonicNow() >= operation.deadline)
    {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                                               "Visible chat admission expired."));
    }
    auto descriptor = registry_.descriptor(project, operation);
    if (!descriptor)
    {
        return Domain::Result<void>::failure(descriptor.error());
    }
    if (std::find(descriptor.value().aliases.begin(), descriptor.value().aliases.end(), root) ==
        descriptor.value().aliases.end())
    {
        return rejected<void>("Visible chat workspace is not an exact registered project alias.");
    }
    auto authority = authority_.authorityFor(project, operation);
    if (!authority)
    {
        return Domain::Result<void>::failure(authority.error());
    }
    auto authorized = authority_.authorize(
        authority.value(), {root, std::nullopt, Domain::FileAccess::Read, false, std::nullopt},
        operation);
    if (!authorized)
    {
        return Domain::Result<void>::failure(authorized.error());
    }
    if (authorized.value().canonicalPath() != root)
    {
        return rejected<void>("Visible chat workspace is not canonical current authority.");
    }
    auto document = routes(studio_);
    if (!document)
    {
        return Domain::Result<void>::failure(document.error());
    }
    const auto servers = Json::parse(document.value().sourceUtf8()).at("mcpServers");
    bool explicitProject = false;
    for (const char* key : {"forge-conductor", "forge-conductor-fallback", "forge-conductor-clu"})
    {
        const auto& route = servers.at(key);
        explicitProject =
            explicitProject || (route.contains("args") && route.at("args").is_array() &&
                                route.at("args").size() == 3U);
        if (route.contains("cwd") &&
            (!route.at("cwd").is_string() || route.at("cwd").get<std::string>() != root.value()))
        {
            return rejected<void>(
                "A native connector's working directory differs from the authorized project root.");
        }
    }
    auto inspected = Infrastructure::Windows::LMStudioConfigurationCodec::inspect(
        document.value(), forgeExecutable_, home_,
        explicitProject ? std::optional<Domain::ProjectId>{project} : std::nullopt,
        explicitProject ? std::optional<Domain::PathText>{root} : std::nullopt);
    if (!inspected)
    {
        return Domain::Result<void>::failure(inspected.error());
    }
    if (!inspected.value().registered || !inspected.value().deploymentId)
    {
        return rejected<void>("Visible chat requires all three current CLI, home, role and shared "
                              "deployment routes.");
    }
    return Domain::Result<void>::success();
}
Domain::Result<void> ManagerVisibleChatContinuity::activate(
    const Domain::ProjectId& project, const Domain::PathText& root,
    const Domain::OperationContext& operation)
{
    auto admitted = validate(project, root, operation);
    if (!admitted)
    {
        return admitted;
    }
    if (observer_)
    {
        if (project_ == project && root_ == root)
        {
            return Domain::Result<void>::success();
        }
        if (project_ == project && root_ != root)
        {
            return rejected<void>("The retained project's checkpoint root cannot be changed by a "
                                  "routing-only workspace observation.");
        }
        if (!observer_->releaseForWorkspaceChange())
        {
            if (stopping_)
            {
                observer_->beginShutdown();
            }
            return rejected<void>("The current project's pending or uncertain native handoff must "
                                  "be reconciled before rebinding.");
        }
        publishedObserver_.store({});
        observer_.reset();
        project_.reset();
        root_.reset();
        admitted = validate(project, root, operation);
        if (!admitted)
        {
            return admitted;
        }
    }
    if (stopping_)
    {
        return rejected<void>("Visible chat owner stopped before workspace activation.");
    }
    auto config = configuration_.reload(operation);
    if (!config)
    {
        return Domain::Result<void>::failure(config.error());
    }
    observer_ =
        observerFactory_
            ? observerFactory_(project, root, config.value().localModel)
            : std::make_shared<Infrastructure::Windows::WindowsLMStudioChatContinuity>(
                  project, root, home_, studio_, lmExecutable_, config.value().localModel, memory_,
                  continuity_, projects_, clock_, uuid_, configuration_, true, forgeExecutable_,
                  [this](const Domain::ProjectId& boundProject, const Domain::PathText& boundRoot,
                         const Domain::OperationContext& current) {
                      return validate(boundProject, boundRoot, current);
                  });
    project_ = project;
    root_ = root;
    observer_->start();
    publishedObserver_.store(observer_);
    if (stopping_)
    {
        observer_->beginShutdown();
    }
    startupError_.clear();
    return Domain::Result<void>::success();
}
Domain::Result<void> ManagerVisibleChatContinuity::start() noexcept
{
    try
    {
        std::lock_guard lock{mutex_};
        if (stopping_)
        {
            return rejected<void>("Visible chat owner is stopping.");
        }
        if (running_)
        {
            return Domain::Result<void>::success();
        }
        running_ = true;
        auto document = routes(studio_);
        if (!document)
        {
            startupError_ = document.error().message;
            return Domain::Result<void>::success();
        }
        const auto route =
            Json::parse(document.value().sourceUtf8()).at("mcpServers").at("forge-conductor");
        if (!route.contains("args") || !route.at("args").is_array() ||
            route.at("args").size() != 3U || !route.contains("cwd") || !route.at("cwd").is_string())
        {
            return Domain::Result<void>::success();
        }
        auto project = Domain::ProjectId::parse(route.at("args").at(2).get<std::string>());
        auto root = Domain::PathText::create(route.at("cwd").get<std::string>());
        if (!project || !root)
        {
            startupError_ = "Explicit native route workspace is invalid.";
            return Domain::Result<void>::success();
        }
        auto activated = activate(project.value(), root.value(), context());
        if (!activated)
        {
            startupError_ = activated.error().message;
        }
        return Domain::Result<void>::success();
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock{mutex_};
        startupError_ = error.what();
        return Domain::Result<void>::success();
    }
}
void ManagerVisibleChatContinuity::beginShutdown() noexcept
{
    stopping_ = true;
    auto observer = publishedObserver_.load();
    if (observer)
    {
        observer->beginShutdown();
    }
}
void ManagerVisibleChatContinuity::shutdown() noexcept
{
    beginShutdown();
    std::lock_guard lock{mutex_};
    if (observer_)
    {
        observer_->shutdown();
        publishedObserver_.store({});
        observer_.reset();
    }
    running_ = false;
}
Snapshot ManagerVisibleChatContinuity::snapshot(const Domain::ProjectId& project) const
{
    auto value = observer_ && project_ == project
                     ? Json::parse(observer_->status())
                     : Json{{"state", "awaiting_bound_workspace"}, {"available", false}};
    if (!startupError_.empty())
    {
        value["error"] = startupError_;
    }
    value["owner"] = "manager";
    value["owner_process_id"] = ::GetCurrentProcessId();
    return {project, value.dump()};
}
Domain::Result<Snapshot> ManagerVisibleChatContinuity::observe(
    const Observation& request, const Domain::OperationContext& operation) noexcept
{
    try
    {
        std::lock_guard lock{mutex_};
        if (!running_ || stopping_)
        {
            return rejected<Snapshot>("Visible chat owner is not accepting observations.");
        }
        auto admitted = activate(request.projectId, request.projectRoot, operation);
        if (!admitted)
        {
            return Domain::Result<Snapshot>::failure(admitted.error());
        }
        observer_->recordTool(request.toolName, request.succeeded, request.canonicalResult);
        return Domain::Result<Snapshot>::success(snapshot(request.projectId));
    }
    catch (const std::exception& error)
    {
        return rejected<Snapshot>(error.what());
    }
}
Domain::Result<Snapshot> ManagerVisibleChatContinuity::status(
    const Domain::ProjectId& project, const Domain::OperationContext& operation) noexcept
{
    try
    {
        std::lock_guard lock{mutex_};
        if (stopping_)
        {
            return rejected<Snapshot>("Visible chat owner is stopping.");
        }
        auto registered = registry_.descriptor(project, operation);
        if (!registered)
        {
            return Domain::Result<Snapshot>::failure(registered.error());
        }
        return Domain::Result<Snapshot>::success(snapshot(project));
    }
    catch (const std::exception& error)
    {
        return rejected<Snapshot>(error.what());
    }
}
} // namespace ForgeConductor::Hosts::Manager
