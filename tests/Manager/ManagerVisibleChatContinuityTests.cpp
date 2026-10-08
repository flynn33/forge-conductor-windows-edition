#include "Fakes/ConfigurationStoreFake.h"
#include "Fakes/ProjectRepositoryFakes.h"
#include "Fakes/RecordingProjectMemoryService.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "Hosts/Manager/ManagerVisibleChatContinuity.h"
#include "Infrastructure/TestSupport.h"
#include "Infrastructure/Windows/LMStudioChatContinuityControl.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

namespace ForgeConductor::Hosts::Manager::Detail
{
class ManagerVisibleChatContinuityAccess final
{
public:
    template <typename Factory>
    static void setFactory(ManagerVisibleChatContinuity& owner, Factory&& factory)
    {
        owner.observerFactory_ = std::forward<Factory>(factory);
    }
    static Domain::Result<void> validate(ManagerVisibleChatContinuity& owner,
                                         const Domain::ProjectId& project,
                                         const Domain::PathText& root,
                                         const Domain::OperationContext& context)
    {
        return owner.validate(project, root, context);
    }
};
} // namespace ForgeConductor::Hosts::Manager::Detail

namespace ForgeConductor::Tests
{
namespace
{
using Json = nlohmann::json;
using Owner = Hosts::Manager::ManagerVisibleChatContinuity;
using OwnerAccess = Hosts::Manager::Detail::ManagerVisibleChatContinuityAccess;
using Observer = Infrastructure::Windows::WindowsLMStudioChatContinuity;
using Controls = Infrastructure::Windows::Detail::LMStudioChatControlActions;
using Effect = Infrastructure::Windows::LMStudioChatEffect;
using Stage = Infrastructure::Windows::LMStudioChatEffectStage;
using Codec = Infrastructure::Windows::LMStudioConfigurationCodec;
using namespace std::chrono_literals;

Domain::PathText pathText(const std::filesystem::path& path)
{
    const auto encoded = path.generic_u8string();
    return take(Domain::PathText::create(
        std::string{reinterpret_cast<const char*>(encoded.data()), encoded.size()}));
}

template <typename T> Domain::Result<T> unexpected()
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, "Unexpected service write in Manager owner fixture."));
}

class RegisteredProjects final : public Contracts::IProjectRegistryRepository
{
public:
    RegisteredProjects(Domain::ProjectMemoryDescriptor first,
                       Domain::ProjectMemoryDescriptor second)
        : descriptors_{std::move(first), std::move(second)}
    {
    }
    Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest&, const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::ProjectInitialization>();
    }
    Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& project, const Domain::OperationContext& context) noexcept override
    {
        if (context.cancellation.stop_requested())
        {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled, "Fixture registry operation cancelled."));
        }
        if (std::chrono::steady_clock::now() >= context.deadline)
        {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded, "Fixture registry operation expired."));
        }
        const auto found =
            std::find_if(descriptors_.begin(), descriptors_.end(),
                         [&](const auto& descriptor) { return descriptor.id == project; });
        if (found == descriptors_.end())
        {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(Domain::makeError(
                Domain::ErrorCodes::ProjectNotFound, "Unregistered fixture project."));
        }
        return Domain::Result<Domain::ProjectMemoryDescriptor>::success(*found);
    }
    Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        std::size_t, const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::success(descriptors_);
    }
    Domain::Result<void> detachAlias(const Domain::ProjectId&, const Domain::PathText&,
                                     const Domain::OperationContext&) noexcept override
    {
        return unexpected<void>();
    }

private:
    const std::vector<Domain::ProjectMemoryDescriptor> descriptors_;
};

class CurrentWorkspace final : public Contracts::IWorkspaceAuthority
{
public:
    explicit CurrentWorkspace(std::vector<Domain::PathText> roots) : roots_{std::move(roots)}
    {
    }
    Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& project, const Domain::OperationContext&) noexcept override
    {
        ++checks;
        if (revoked)
        {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "Fixture workspace authority revoked."));
        }
        return issueAuthority(parse<Domain::AuthorityId>("d89d9b94-9ed0-47a6-a641-551ee4457fd5"),
                              project, parse<Domain::ClientId>("manager-visible-chat-fixture"),
                              roots_, Domain::FileAccess::Read, {Domain::FileAccess::Read}, {},
                              false, 1U);
    }
    Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority, const std::vector<Domain::PathText>& roots,
        const std::vector<Domain::FileAccess>& grants, bool shell, std::uint64_t generation,
        const Domain::OperationContext&) noexcept override
    {
        return narrowAuthority(authority, roots, grants, shell, generation);
    }
    Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        if (revoked ||
            std::find(roots_.begin(), roots_.end(), request.requestedPath) == roots_.end())
        {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(Domain::ErrorCodes::Unauthorized,
                                  "Fixture path is outside current workspace authority."));
        }
        return issueAuthorizedPath(authority, request.requestedPath, request.requestedPath,
                                   request.access);
    }
    std::atomic<bool> revoked{};
    std::atomic<std::size_t> checks{};

private:
    const std::vector<Domain::PathText> roots_;
};

// The real worker and the transient bridge caller both read the retained pointer.
class LockedMemory final : public Contracts::ILegacyMemoryService
{
public:
    Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.set(request, context);
    }
    Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.get(request, context);
    }
    Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.list(request, context);
    }
    Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.remove(request, context);
    }
    Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.search(request, context);
    }
    Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& request,
        const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.purge(request, context);
    }
    Domain::Result<void> quickCheck(const Domain::OperationContext& context) noexcept override
    {
        std::lock_guard lock{mutex_};
        return fake_.quickCheck(context);
    }
    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        fake_.shutdown();
    }

private:
    std::mutex mutex_;
    Fakes::LegacyMemoryServiceFake fake_{8U,
                                         {"purge_legacy_memory", "all", "private-manager-test"}};
};

class RetainedPacket final : public Contracts::ILegacyContextContinuityService
{
public:
    void seed(Domain::LegacyContinuityRecord record)
    {
        std::lock_guard lock{mutex_};
        record_ = std::move(record);
    }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> checkpoint(
        const Domain::LegacyContinuityWriteRequest&, const Domain::ClientId&,
        Domain::LegacyHandoffSource, const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityPersistOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> handoff(
        const Domain::LegacyContinuityWriteRequest&, const Domain::ClientId&,
        Domain::LegacyHandoffSource, const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityPersistOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest&, const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityPersistOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityPersistOutcome> budgetHandoff(
        const Domain::ClientId&, std::string_view, const Domain::OperationContext&,
        const Domain::LegacyContinuityPatch&,
        std::optional<Domain::LegacyHandoffId>) noexcept override
    {
        return unexpected<Domain::LegacyContinuityPersistOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        if (record_ && request.handoffId && *request.handoffId == record_->packet.id)
        {
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success({record_, true});
        }
        return unexpected<Domain::LegacyContinuityGetOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityListOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome> repairProjections(
        const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityProjectionRepairOutcome>();
    }
    Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&, const Domain::OperationContext&) noexcept override
    {
        return unexpected<Domain::LegacyContinuityResetOutcome>();
    }
    void shutdown() noexcept override
    {
    }

private:
    std::mutex mutex_;
    std::optional<Domain::LegacyContinuityRecord> record_;
};

Json generation(std::uint64_t used)
{
    return Json{{"type", "contentBlock"},
                {"genInfo",
                 {{"stats", {{"totalTokensCount", used}, {"stopReason", "eosFound"}}},
                  {"loadModelConfig",
                   {{"fields", Json::array({Json{{"key", "llm.load.contextLength"},
                                                 {"value", 32768U}}})}}}}}};
}
Json assistant(Json steps)
{
    return Json{{"versions",
                 Json::array({Json{
                     {"type", "multiStep"}, {"role", "assistant"}, {"steps", std::move(steps)}}})},
                {"currentlySelected", 0U}};
}
Json conversation(std::uint64_t used)
{
    return Json{{"messages", Json::array({assistant(Json::array({generation(used)}))})},
                {"plugins", Json::array({"mcp/forge-conductor", "mcp/forge-conductor-fallback",
                                         "mcp/forge-conductor-clu"})}};
}
Json nativeTool(std::string_view name, const Json& payload, std::size_t call)
{
    const auto id = "manager-native-" + std::to_string(call);
    return Json{
        {"type", "contentBlock"},
        {"content",
         Json::array(
             {Json{{"type", "toolCallRequest"},
                   {"callId", call},
                   {"name", name},
                   {"toolCallRequestId", id},
                   {"pluginIdentifier", "mcp/forge-conductor"}},
              Json{{"type", "toolCallResult"},
                   {"callId", call},
                   {"toolCallRequestId", id},
                   {"content",
                    Json::array({Json{{"type", "text"}, {"text", payload.dump()}}}).dump()}}})}};
}

class OwnerFixture final
{
public:
    OwnerFixture()
        : files{std::filesystem::temp_directory_path() /
                ("manager-visible-chat-" + take(uuid.next()).value())},
          home{pathText(files / "home")}, studio{pathText(files / "studio")},
          root{pathText(files / "workspace")}, alternate{pathText(files / "alternate")},
          cli{trustedSiblingCli()}, lm{pathText(files / "LM Studio.exe")},
          registry{
              {project, "Private registered workspace", std::nullopt, {root, alternate}},
              {secondProject, "Second private registered workspace", std::nullopt, {alternate}}},
          authority{{root, alternate}}
    {
        for (const auto& directory :
             {files / "home", files / "workspace", files / "alternate",
              files / "studio" / ".internal", files / "studio" / "conversations" / "project"})
        {
            std::filesystem::create_directories(directory);
        }
        configuration.reloadResult.set(Domain::Result<Domain::AppConfig>::success({}));
        projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
            {project, {}, std::nullopt, false, 0U, 0U}));
        bindFiles(root, 100U);
        controls = std::make_shared<Controls>();
        controls->activate = [](const auto&, const auto&) {
            return Domain::Result<void>::success();
        };
        controls->pause = [this](const auto&, std::string_view,
                                 const Domain::OperationContext& operation) {
            ++controlCalls;
            if (blockPause)
            {
                std::unique_lock lock{gateMutex};
                entered = true;
                gate.notify_all();
                std::stop_callback cancelled{operation.cancellation, [this] { gate.notify_all(); }};
                gate.wait(lock, [&] { return operation.cancellation.stop_requested() || release; });
                cancellationSeen = operation.cancellation.stop_requested();
                gate.notify_all();
                gate.wait(lock, [&] { return release; });
                return Domain::Result<bool>::failure(
                    Domain::makeError(Domain::ErrorCodes::Cancelled, "Private control cancelled."));
            }
            return Domain::Result<bool>::success(true);
        };
        controls->idle = [this](const auto&, const Domain::OperationContext&) {
            ++controlCalls;
            return Domain::Result<bool>::success(true);
        };
        controls->send = [this](
                             const Domain::PathText& executable, std::string_view text,
                             bool newChat, const Domain::OperationContext& operation,
                             std::optional<std::string_view> expected,
                             const std::function<void(std::string_view)>& successor,
                             const Infrastructure::Windows::LMStudioChatEffectObserver& receipt) {
            require(executable == lm && receipt,
                    "Private observer lost its native control target or receipt.");
            std::lock_guard lock{filesMutex};
            require(expected == std::optional<std::string_view>{selected},
                    "Private observer addressed another selected chat.");
            if (newChat)
            {
                auto accepted = receipt({Effect::NewChat, Stage::BeforeDispatch, selected, 0U});
                if (!accepted)
                {
                    return accepted;
                }
                ++creations;
                selected = "project/successor.conversation.json";
                write(files / "studio" / "conversations" / selected,
                      Json{{"messages", Json::array()},
                           {"plugins",
                            Json::array({"mcp/forge-conductor", "mcp/forge-conductor-fallback",
                                         "mcp/forge-conductor-clu"})}});
                write(files / "studio" / ".internal" / "conversation-config.json",
                      Json{{"selectedConversation", selected}});
                accepted = receipt({Effect::NewChat, Stage::Confirmed, selected, 0U});
                if (!accepted)
                {
                    return accepted;
                }
                if (successor)
                {
                    successor(selected);
                }
            }
            const auto observed =
                take(Infrastructure::Windows::WindowsLMStudioConversationReader::read(studio,
                                                                                      operation));
            require(observed && observed->conversationId == selected,
                    "Private Send did not retain selected native identity.");
            const auto boundary = observed->userMessages.size();
            auto accepted = receipt({Effect::Send, Stage::BeforeDispatch, selected, boundary});
            if (!accepted)
            {
                return accepted;
            }
            ++sends;
            if (newChat)
            {
                ++deliveries;
            }
            if (uncertainSend)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::AcknowledgementTimeout,
                                      "Private uncertain native Send.", true));
            }
            appendUnlocked(Json{
                {"versions",
                 Json::array(
                     {Json{{"type", "singleStep"},
                           {"role", "user"},
                           {"content", Json::array({Json{{"type", "text"}, {"text", text}}})}}})},
                {"currentlySelected", 0U}});
            return receipt({Effect::Send, Stage::Confirmed, selected, boundary});
        };
        owner = std::make_unique<Owner>(home, studio, lm, cli, registry, authority, memory,
                                        continuity, projects, clock, uuid, configuration);
        OwnerAccess::setFactory(*owner, [this](const auto& projectId, const auto& projectRoot,
                                               const auto& config) {
            ++factoryCalls;
            projects.listRecentResult.set(Domain::Result<Domain::MemoryPage>::success(
                {projectId, {}, std::nullopt, false, 0U, 0U}));
            auto observer =
                Infrastructure::Windows::Detail::LMStudioChatContinuityAccess::createScoped(
                    controls, cli,
                    [this](const auto& boundProject, const auto& boundRoot, const auto& operation) {
                        return OwnerAccess::validate(*owner, boundProject, boundRoot, operation);
                    },
                    projectId, projectRoot, home, studio, lm, config, memory, continuity, projects,
                    clock, uuid, configuration, true);
            return std::shared_ptr<Observer>{std::move(observer)};
        });
    }
    ~OwnerFixture()
    {
        releaseControl();
        if (owner)
        {
            owner->shutdown();
        }
        owner.reset();
        std::error_code ignored;
        std::filesystem::remove_all(files, ignored);
    }
    static Domain::PathText trustedSiblingCli()
    {
        std::wstring module(32768U, L'\0');
        const auto count =
            ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        require(count > 0U && count < module.size(), "Private test module path is unavailable.");
        module.resize(count);
        return pathText(std::filesystem::path{module}.parent_path() / "forge-conductor.exe");
    }
    static void write(const std::filesystem::path& target, const Json& value)
    {
        const auto temporary = target.wstring() + L".private-publish";
        {
            std::ofstream output{std::filesystem::path{temporary},
                                 std::ios::binary | std::ios::trunc};
            output << value.dump();
            require(static_cast<bool>(output), "Private LM fixture write failed.");
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!::MoveFileExW(temporary.c_str(), target.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            require(std::chrono::steady_clock::now() < deadline,
                    "Private LM fixture atomic publication failed.");
            std::this_thread::sleep_for(5ms);
        }
    }
    Json routes(const Domain::PathText& workspace, const Domain::ProjectId& boundProject) const
    {
        const auto bytes = take(Codec::mergeForgeServers(
            Codec::empty(), cli, home,
            parse<Domain::DeploymentId>("0b6eadbe-c35a-4d41-8c6e-6d8c40b68dc2"), boundProject,
            workspace));
        return Json::parse(reinterpret_cast<const char*>(bytes.data()),
                           reinterpret_cast<const char*>(bytes.data()) + bytes.size());
    }
    Json routes(const Domain::PathText& workspace) const
    {
        return routes(workspace, project);
    }
    void bindFiles(const Domain::PathText& workspace, std::uint64_t used,
                   const Domain::ProjectId& boundProject)
    {
        std::lock_guard lock{filesMutex};
        selected = "project/chat.conversation.json";
        write(files / "studio" / "mcp.json", routes(workspace, boundProject));
        write(files / "studio" / ".internal" / "projects-registry.json",
              Json{{"json",
                    {{"projects", Json::array({Json::array(
                                      {"test-project", Json{{"path", studio.value()}}})})}}}});
        write(files / "studio" / ".internal" / "conversation-config.json",
              Json{{"selectedConversation", selected}});
        write(files / "studio" / "conversations" / selected, conversation(used));
    }
    void bindFiles(const Domain::PathText& workspace, std::uint64_t used)
    {
        bindFiles(workspace, used, project);
    }
    void pressure()
    {
        std::lock_guard lock{filesMutex};
        write(files / "studio" / "conversations" / selected, conversation(31000U));
    }
    void append(Json value)
    {
        std::lock_guard lock{filesMutex};
        appendUnlocked(std::move(value));
    }
    void appendUnlocked(Json value)
    {
        const auto file = files / "studio" / "conversations" / selected;
        Json saved;
        {
            std::ifstream input{file, std::ios::binary};
            saved = Json::parse(input);
        }
        saved["messages"].push_back(std::move(value));
        write(file, saved);
    }
    Json retainModelPacket()
    {
        Domain::LegacyHandoffPacket packet{parse<Domain::LegacyHandoffId>("manager-model-packet")};
        packet.resumeReady = true;
        packet.goal = "Continue the original private Manager continuity task after its transient "
                      "caller exits.";
        packet.narrative =
            "The Manager owns the observer after the temporary authenticated caller exits. The "
            "task retains a complete model packet and exact registered workspace identity. Native "
            "PRIMARY conversation evidence must acknowledge the handoff before creating one "
            "successor, and recovery must read the retained packet and complete a following Forge "
            "tool without repeating any uncertain native control effect.";
        packet.resumeSeed = packet.narrative + " Call context_get for this packet and run "
                                               "agent_list before continuing the pending task.";
        packet.keyFiles = {root.value()};
        packet.decisions = {"Keep Manager ownership across temporary caller lifetimes",
                            "Do not replay uncertain New chat or Send"};
        packet.nextActions = {"Call context_get for the retained packet",
                              "Call agent_list after recovery"};
        Json body{
            {"meta", {{"id", packet.id.value()}, {"source", "model"}, {"resume_ready", true}}},
            {"task",
             {{"goal", packet.goal},
              {"status", packet.status},
              {"next_actions", packet.nextActions},
              {"blockers", packet.blockers}}},
            {"working_set", {{"key_files", packet.keyFiles}, {"decisions", packet.decisions}}},
            {"resume", {{"seed", packet.resumeSeed}}},
            {"narrative", packet.narrative},
            {"agents", Json::array()}};
        continuity.seed({packet, 1U, {}});
        TestContext caller;
        const auto saved = take(memory.set(
            {"continuity/project/" + project.value(), packet.id.value(), {}}, caller.active()));
        require(saved.stored, "Private retained packet pointer was not saved.");
        return Json{{"ok", true},
                    {"handoff_id", packet.id.value()},
                    {"resume_seed", packet.resumeSeed},
                    {"packet", body}};
    }
    Json status(const Domain::ProjectId& boundProject) const
    {
        TestContext caller;
        const auto result = take(owner->status(boundProject, caller.active()));
        require(result.projectId == boundProject, "Manager status changed the project identity.");
        return Json::parse(result.canonicalStatus);
    }
    Json status() const
    {
        return status(project);
    }
    Json await(const std::function<bool(const Json&)>& predicate,
               const Domain::ProjectId& boundProject) const
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        do
        {
            auto value = status(boundProject);
            if (predicate(value))
            {
                return value;
            }
            std::this_thread::sleep_for(20ms);
        } while (std::chrono::steady_clock::now() < deadline);
        throw TestFailure{"Manager owner status did not reach expected state: " +
                          status(boundProject).dump()};
    }
    Json await(const std::function<bool(const Json&)>& predicate) const
    {
        return await(predicate, project);
    }
    void observing(const Domain::ProjectId& boundProject) const
    {
        await(
            [](const Json& status) {
                return status.value("state", std::string{}) == "observing" &&
                       status.contains("context_telemetry") && !status.contains("error");
            },
            boundProject);
        // Telemetry is published before checkpoint loading in tick(); allow that
        // first tick to finish before asking the owner to release an idle scope.
        std::this_thread::sleep_for(600ms);
    }
    void observing() const
    {
        observing(project);
    }
    void start()
    {
        require(static_cast<bool>(owner->start()), "Private Manager owner did not start.");
    }
    Domain::Result<Manager::ManagerVisibleChatSnapshot> observe(
        const Domain::PathText& workspace, std::string_view tool = "get_forge_status",
        Json result = Json{{"ok", true}}) const
    {
        TestContext caller;
        return owner->observe({project, workspace, std::string{tool}, true, result.dump()},
                              caller.active());
    }
    Domain::Result<Manager::ManagerVisibleChatSnapshot> observeProject(
        const Domain::ProjectId& boundProject, const Domain::PathText& workspace) const
    {
        TestContext caller;
        return owner->observe({boundProject, workspace, "get_forge_status", true, "{\"ok\":true}"},
                              caller.active());
    }
    std::string checkpointBytes(const Domain::ProjectId& boundProject) const
    {
        std::ifstream input{files / "home" / "continuity" /
                                ("lmstudio-visible-" + boundProject.value() + ".checkpoint"),
                            std::ios::binary};
        require(static_cast<bool>(input), "Private retained checkpoint is missing.");
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    }
    void releaseControl()
    {
        std::lock_guard lock{gateMutex};
        release = true;
        gate.notify_all();
    }

    Infrastructure::Windows::SystemClock clock;
    Infrastructure::Windows::WindowsUuidGenerator uuid;
    Domain::ProjectId project{parse<Domain::ProjectId>("5c06c108-26c9-4008-8123-2720fc8c9d97")};
    Domain::ProjectId secondProject{
        parse<Domain::ProjectId>("aebbe194-23c9-4407-bd7a-228e26f31caa")};
    std::filesystem::path files;
    Domain::PathText home, studio, root, alternate, cli, lm;
    RegisteredProjects registry;
    CurrentWorkspace authority;
    LockedMemory memory;
    RetainedPacket continuity;
    Fakes::RecordingConfigurationStoreFake configuration;
    Fakes::RecordingProjectMemoryService projects;
    std::shared_ptr<Controls> controls;
    std::unique_ptr<Owner> owner;
    std::atomic<std::size_t> factoryCalls{}, sends{}, creations{}, deliveries{}, controlCalls{};
    bool uncertainSend{}, blockPause{};
    std::mutex gateMutex;
    std::condition_variable gate;
    bool entered{}, cancellationSeen{}, release{};

private:
    std::mutex filesMutex;
    std::string selected{"project/chat.conversation.json"};
};

void currentRoutesAndAuthorityAreRequired()
{
    OwnerFixture fixture;
    fixture.start();
    fixture.observing();
    require(fixture.factoryCalls == 1U && fixture.sends == 0U,
            "Current explicit routes did not activate one idle observer.");
    TestContext caller;
    const auto unknown = parse<Domain::ProjectId>("083f1a53-cda8-438e-9e04-5aaefb275a44");
    requireError(fixture.owner->observe({unknown, fixture.root, "get_forge_status", true, "{}"},
                                        caller.active()),
                 Domain::ErrorCodes::ProjectNotFound, "Manager admitted an unknown project.");
    requireError(fixture.owner->status(unknown, caller.active()),
                 Domain::ErrorCodes::ProjectNotFound,
                 "Manager status admitted an unknown project.");
    requireError(fixture.observe(pathText(fixture.files / "unregistered")),
                 Domain::ErrorCodes::Unauthorized, "Manager admitted an unregistered root.");
    fixture.authority.revoked = true;
    requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                 "Manager reused revoked current authority.");
    fixture.authority.revoked = false;
    requireError(
        fixture.owner->observe({fixture.project, fixture.root, "get_forge_status", true, "{}"},
                               caller.expired()),
        Domain::ErrorCodes::DeadlineExceeded, "Manager admitted an expired observation.");
    caller.cancellation.request_stop();
    requireError(
        fixture.owner->observe({fixture.project, fixture.root, "get_forge_status", true, "{}"},
                               caller.active()),
        Domain::ErrorCodes::Cancelled, "Manager admitted a cancelled observation.");
    for (const std::string_view change :
         {"role", "home", "binary", "deployment", "missing_route", "cwd"})
    {
        auto routes = fixture.routes(fixture.root);
        auto& secondary = routes["mcpServers"]["forge-conductor-fallback"];
        if (change == "role")
        {
            secondary["env"]["FORGE_MCP_ROLE"] = "primary";
        }
        else if (change == "home")
        {
            secondary["env"]["FORGE_CONDUCTOR_HOME"] = fixture.alternate.value();
        }
        else if (change == "binary")
        {
            secondary["command"] = pathText(fixture.files / "ForgeConductor.Manager.exe").value();
        }
        else if (change == "deployment")
        {
            secondary["env"]["FORGE_DEPLOYMENT_ID"] = "018cf94c-a96c-47a2-8ddc-596ae1f39353";
        }
        else if (change == "missing_route")
        {
            routes["mcpServers"].erase("forge-conductor-clu");
        }
        else
        {
            secondary["cwd"] = fixture.alternate.value();
        }
        OwnerFixture::write(fixture.files / "studio" / "mcp.json", routes);
        requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                     "Manager admitted changed native routes.");
    }
    OwnerFixture::write(fixture.files / "studio" / "mcp.json", fixture.routes(fixture.root));
    require(static_cast<bool>(fixture.observe(fixture.root)),
            "Restored current routes could not reconnect.");
    require(fixture.factoryCalls == 1U && fixture.sends == 0U,
            "Rejected observations replaced the observer or dispatched effects.");
}

void trustedSiblingCliMismatchDoesNotActivate()
{
    OwnerFixture fixture;
    auto routes = fixture.routes(fixture.root);
    const auto manager =
        pathText(std::filesystem::path{
                     std::u8string{reinterpret_cast<const char8_t*>(fixture.cli.value().data()),
                                   fixture.cli.value().size()}}
                     .parent_path() /
                 "ForgeConductor.Manager.exe");
    for (const char* key : {"forge-conductor", "forge-conductor-fallback", "forge-conductor-clu"})
    {
        routes["mcpServers"][key]["command"] = manager.value();
    }
    OwnerFixture::write(fixture.files / "studio" / "mcp.json", routes);
    fixture.start();
    require(fixture.factoryCalls == 0U && fixture.status().contains("error"),
            "Manager activated routes aimed at its own binary instead of the trusted sibling CLI.");
    requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                 "Manager admitted same-role routes with the wrong trusted binary.");
    OwnerFixture::write(fixture.files / "studio" / "mcp.json", fixture.routes(fixture.root));
    require(static_cast<bool>(fixture.observe(fixture.root)),
            "Current sibling CLI routes did not activate after repair.");
    fixture.observing();
    require(fixture.factoryCalls == 1U,
            "Trusted sibling CLI repair activated more than one observer.");
}

void freshAuthorityIsCheckedAtNativeDispatch()
{
    OwnerFixture fixture;
    fixture.controls->pause = [&](const auto&, std::string_view, const auto&) {
        fixture.authority.revoked = true;
        return Domain::Result<bool>::success(true);
    };
    fixture.pressure();
    fixture.start();
    fixture.await([](const Json& status) {
        return status.value("error", std::string{}).find("workspace authority revoked") !=
               std::string::npos;
    });
    require(fixture.factoryCalls == 1U && fixture.authority.checks > 1U && fixture.sends == 0U &&
                fixture.creations == 0U,
            "Manager activation authority was reused after revocation at the native dispatch "
            "boundary.");
}

void rejectedUnboundObservationRetainsItsStatusDiagnostic()
{
    OwnerFixture fixture;
    auto routes = fixture.routes(fixture.root);
    for (auto& route : routes["mcpServers"])
    {
        route["args"] = Json::array({"serve"});
        route.erase("cwd");
        route["env"]["FORGE_CONDUCTOR_HOME"] = fixture.alternate.value();
    }
    OwnerFixture::write(fixture.files / "studio" / "mcp.json", routes);
    fixture.start();
    require(fixture.factoryCalls == 0U && !fixture.status().contains("error"),
            "Global routes activated an observer before a bound observation.");
    requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                 "Manager admitted a different native route home.");
    const auto refused = fixture.status();
    require(fixture.factoryCalls == 0U && refused.value("error", std::string{}).find(
                "all three current CLI, home, role and shared deployment routes") != std::string::npos,
            "Rejected unbound observation lost its inspectable admission diagnostic.");
    routes["mcpServers"].erase("forge-conductor-fallback");
    for (const auto& incomplete : {routes, Json::object()})
    {
        OwnerFixture::write(fixture.files / "studio" / "mcp.json", incomplete);
        requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                     "Missing native registrations bypassed admission refusal.");
        require(fixture.factoryCalls == 0U && fixture.status().value("error", std::string{}).find(
                    "all three current native connector registrations") != std::string::npos,
                "Missing unbound registrations lost their inspectable refusal diagnostic.");
    }
    OwnerFixture::write(fixture.files / "studio" / "mcp.json", fixture.routes(fixture.root));
    require(static_cast<bool>(fixture.observe(fixture.root)),
            "Repaired current routes did not activate an observer.");
    fixture.observing();
    require(fixture.factoryCalls == 1U && !fixture.status().contains("error"),
            "Successful activation retained its old admission diagnostic.");
}

void completeNativeRecovery(OwnerFixture& fixture)
{
    fixture.start();
    fixture.observing();
    fixture.pressure();
    fixture.await([](const Json& status) {
        return status.value("state", std::string{}) == "waiting_for_model_packet";
    });
    require(fixture.sends == 1U && fixture.creations == 0U,
            "Manager did not request one model packet in its predecessor.");
    const auto handoff = fixture.retainModelPacket();
    {
        TestContext temporaryCaller;
        require(static_cast<bool>(fixture.owner->observe(
                    {fixture.project, fixture.root, "session_handoff", true, handoff.dump()},
                    temporaryCaller.active())),
                "The registered scoped handoff observation was refused.");
        temporaryCaller.cancellation.request_stop();
    }
    const auto before = fixture.status();
    std::this_thread::sleep_for(600ms);
    require(fixture.creations == 0U && fixture.deliveries == 0U,
            "A bridge result without native PRIMARY handoff evidence created a successor.");
    fixture.append(assistant(Json::array({nativeTool("session_handoff", handoff, 1U)})));
    const auto resuming = fixture.await(
        [](const Json& status) { return status.value("state", std::string{}) == "resuming"; });
    require(resuming["owner"] == "manager" &&
                resuming["owner_process_id"] == ::GetCurrentProcessId() &&
                resuming["owner_process_id"] == before["owner_process_id"] &&
                fixture.factoryCalls == 1U,
            "A new scoped caller did not reconnect to the same Manager observer.");
    require(fixture.creations == 1U && fixture.deliveries == 1U && fixture.sends == 2U,
            "Native handoff did not create and deliver exactly one successor.");
    require(static_cast<bool>(fixture.observe(
                fixture.root, "context_get",
                Json{{"ok", true}, {"found", true}, {"handoff_id", "manager-model-packet"}})),
            "Scoped context recovery observation was refused.");
    require(static_cast<bool>(fixture.observe(fixture.root, "agent_list")),
            "Scoped following tool observation was refused.");
    std::this_thread::sleep_for(600ms);
    require(fixture.status()["state"] == "resuming",
            "Bridge-only context_get and following tool evidence completed native recovery.");
    fixture.append(assistant(Json::array(
        {nativeTool("context_get",
                    Json{{"ok", true}, {"found", true}, {"handoff_id", "manager-model-packet"}},
                    2U),
         generation(3000U)})));
    std::this_thread::sleep_for(600ms);
    require(
        fixture.status()["state"] == "resuming",
        "Native context_get completed recovery without a following successful native Forge tool.");
    fixture.append(assistant(
        Json::array({nativeTool("agent_list", Json{{"ok", true}}, 3U), generation(3000U)})));
    fixture.await(
        [](const Json& status) { return status.value("state", std::string{}) == "resumed"; });
    require(fixture.creations == 1U && fixture.deliveries == 1U && fixture.sends == 2U,
            "Completed native recovery repeated a native effect.");
}

void callerExitKeepsManagerObserverAndNativeRecovery()
{
    OwnerFixture fixture;
    completeNativeRecovery(fixture);
    const auto checkpoint = fixture.checkpointBytes(fixture.project);
    fixture.bindFiles(fixture.alternate, 100U, fixture.secondProject);
    require(static_cast<bool>(fixture.observeProject(fixture.secondProject, fixture.alternate)),
            "Verified complete observer could not release its previous workspace.");
    fixture.observing(fixture.secondProject);
    require(fixture.factoryCalls == 2U && fixture.creations == 1U && fixture.deliveries == 1U &&
                fixture.checkpointBytes(fixture.project) == checkpoint,
            "Complete workspace switch did not release and replace one observer without replay.");
}

void sameProjectAliasRetainsOriginalCheckpointScope()
{
    OwnerFixture fixture;
    completeNativeRecovery(fixture);
    const auto checkpoint = fixture.checkpointBytes(fixture.project);
    fixture.bindFiles(fixture.alternate, 100U);
    requireError(fixture.observe(fixture.alternate), Domain::ErrorCodes::Unauthorized,
                 "A routing-only observation changed the retained project's checkpoint root.");
    require(fixture.factoryCalls == 1U && fixture.checkpointBytes(fixture.project) == checkpoint &&
                fixture.sends == 2U && fixture.creations == 1U,
            "Refused alias scope released the original observer, changed its checkpoint or "
            "replayed native control.");
}

void idleWorkspaceSwitchAndPendingRefusal()
{
    OwnerFixture fixture;
    fixture.start();
    fixture.observing();
    requireError(fixture.observeProject(fixture.secondProject, fixture.alternate),
                 Domain::ErrorCodes::Unauthorized,
                 "Manager accepted a new workspace before all native routes selected it.");
    fixture.bindFiles(fixture.alternate, 100U, fixture.secondProject);
    require(static_cast<bool>(fixture.observeProject(fixture.secondProject, fixture.alternate)),
            "Idle registered workspace could not switch.");
    fixture.observing(fixture.secondProject);
    require(fixture.factoryCalls == 2U && fixture.sends == 0U,
            "Idle switch did not replace exactly one observer.");
    fixture.pressure();
    fixture.await(
        [](const Json& status) {
            return status.value("state", std::string{}) == "waiting_for_model_packet";
        },
        fixture.secondProject);
    const auto checkpoint = fixture.checkpointBytes(fixture.secondProject);
    fixture.bindFiles(fixture.root, 100U);
    requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                 "Pending model packet let Manager rebind its observer.");
    require(fixture.checkpointBytes(fixture.secondProject) == checkpoint &&
                fixture.factoryCalls == 2U && fixture.sends == 1U && fixture.creations == 0U,
            "Denied pending switch changed its retained checkpoint or replayed native control.");
}

void uncertainNativeEffectRefusesWorkspaceSwitch()
{
    OwnerFixture fixture;
    fixture.uncertainSend = true;
    fixture.pressure();
    fixture.start();
    fixture.await([&](const Json& status) {
        return status.value("state", std::string{}) == "recovery_pending" && fixture.sends == 1U &&
               status.value("error", std::string{}).find("Private uncertain native Send") !=
                   std::string::npos;
    });
    require(fixture.sends == 1U, "Private uncertain request did not dispatch once.");
    fixture.bindFiles(fixture.alternate, 100U);
    requireError(fixture.observe(fixture.alternate), Domain::ErrorCodes::Unauthorized,
                 "Uncertain native Send let Manager rebind its observer.");
    require(fixture.factoryCalls == 1U && fixture.sends == 1U && fixture.creations == 0U,
            "Denied uncertain switch discarded ownership or repeated a native effect.");
}

void beginShutdownCancelsAndShutdownJoinsBeforeDependencies()
{
    OwnerFixture fixture;
    fixture.blockPause = true;
    fixture.pressure();
    fixture.start();
    bool entered{};
    {
        std::unique_lock lock{fixture.gateMutex};
        entered = fixture.gate.wait_for(lock, 10s, [&] { return fixture.entered; });
    }
    fixture.owner->beginShutdown();
    requireError(fixture.observe(fixture.root), Domain::ErrorCodes::Unauthorized,
                 "beginShutdown left observations open.");
    TestContext caller;
    requireError(fixture.owner->status(fixture.project, caller.active()),
                 Domain::ErrorCodes::Unauthorized,
                 "beginShutdown left reconnect status callbacks open.");
    bool cancelled{};
    {
        std::unique_lock lock{fixture.gateMutex};
        cancelled = fixture.gate.wait_for(lock, 2s, [&] { return fixture.cancellationSeen; });
    }
    auto joined = std::async(std::launch::async, [&] { fixture.owner->shutdown(); });
    const bool returnedBeforeControl = joined.wait_for(50ms) == std::future_status::ready;
    fixture.releaseControl();
    const bool completed = joined.wait_for(5s) == std::future_status::ready;
    joined.get();
    require(entered && cancelled,
            "Manager beginShutdown did not propagate cancellation into its live observer control.");
    require(!returnedBeforeControl && completed,
            "Manager shutdown did not join the exact live observer before returning.");
    const auto calls = fixture.controlCalls.load();
    fixture.memory.shutdown();
    fixture.continuity.shutdown();
    fixture.configuration.shutdown();
    fixture.projects.shutdown();
    std::this_thread::sleep_for(600ms);
    require(fixture.controlCalls == calls && fixture.sends == 0U && fixture.creations == 0U,
            "A native worker continued after joined shutdown and dependency close.");
    fixture.owner->shutdown();
}
} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    TestRegistry tests;
    addTest(tests, "ManagerVisibleChatContinuity.current_routes_and_authority",
            currentRoutesAndAuthorityAreRequired);
    addTest(tests, "ManagerVisibleChatContinuity.trusted_sibling_cli",
            trustedSiblingCliMismatchDoesNotActivate);
    addTest(tests, "ManagerVisibleChatContinuity.fresh_native_dispatch_authority",
            freshAuthorityIsCheckedAtNativeDispatch);
    addTest(tests, "ManagerVisibleChatContinuity.rejected_unbound_observation_diagnostic",
            rejectedUnboundObservationRetainsItsStatusDiagnostic);
    addTest(tests, "ManagerVisibleChatContinuity.caller_exit_native_recovery",
            callerExitKeepsManagerObserverAndNativeRecovery);
    addTest(tests, "ManagerVisibleChatContinuity.same_project_alias_scope",
            sameProjectAliasRetainsOriginalCheckpointScope);
    addTest(tests, "ManagerVisibleChatContinuity.idle_switch_pending_refusal",
            idleWorkspaceSwitchAndPendingRefusal);
    addTest(tests, "ManagerVisibleChatContinuity.uncertain_effect_refusal",
            uncertainNativeEffectRefusesWorkspaceSwitch);
    addTest(tests, "ManagerVisibleChatContinuity.shutdown_cancellation_exact_join",
            beginShutdownCancelsAndShutdownJoinsBeforeDependencies);
    std::size_t failures{};
    for (const auto& [name, run] : tests)
    {
        try
        {
            run();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "Manager visible-chat owner tests: " << tests.size() - failures << " passed, "
              << failures << " failed\n";
    return failures == 0U ? 0 : 1;
}
