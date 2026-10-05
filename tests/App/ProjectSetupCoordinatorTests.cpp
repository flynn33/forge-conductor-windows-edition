#include "ForgeConductor/Application/ProjectSetupCoordinator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ProjectSetupWorkspace.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <iostream>
#include <stdexcept>

using namespace ForgeConductor::Application;

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error{message};
}

namespace Domain = ForgeConductor::Domain;
namespace App = ForgeConductor::Hosts::App;
namespace Windows = ForgeConductor::Infrastructure::Windows;

template<class T> T take(Domain::Result<T> result)
{
    if (!result) throw std::runtime_error{result.error().code + ": " + result.error().message};
    return std::move(result).value();
}

Domain::PathText pathText(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return take(Domain::PathText::create(std::string{
        reinterpret_cast<const char*>(utf8.data()), utf8.size()}));
}

struct TestTree final {
    std::filesystem::path base = std::filesystem::temp_directory_path() /
        (L"ForgeConductor.Prepare." + std::to_wstring(::GetCurrentProcessId()) +
         L"." + std::to_wstring(::GetTickCount64()));
    TestTree()
    {
        std::filesystem::create_directories(base / L"project" / L"child");
        std::filesystem::create_directories(base / L"project-peer");
    }
    ~TestTree()
    {
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }
};

void registeredWorkspaceReuse()
{
    TestTree tree;
    const auto folder = pathText(tree.base / L"project");
    const auto projectId = take(Domain::ProjectId::parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
    const Domain::ProjectMemoryDescriptor descriptor{
        projectId, "Existing workspace", "git:saved-identity", {folder}};
    const std::vector<Domain::ProjectMemoryDescriptor> projects{descriptor};
    auto selected = take(App::registeredProjectForSetupFolder(folder, projects));
    require(selected && selected->project.id == projectId &&
            selected->project.repositoryIdentity == descriptor.repositoryIdentity,
        "Prepare must reuse the existing registry identity for an exact alias");
    auto normalized = folder.value() + "/./";
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char character) {
        return character >= 'a' && character <= 'z'
            ? static_cast<char>(character - 'a' + 'A') : character;
    });
    const auto normalizedFolder = take(Domain::PathText::create(normalized));
    const auto normalizedSelection = take(App::registeredProjectForSetupFolder(normalizedFolder, projects));
    require(normalizedSelection && normalizedSelection->project.id == projectId,
        "Windows case, separators and trailing dot segments must select the same registered alias");
    require(!take(App::registeredProjectForSetupFolder(pathText(tree.base / L"new-project"), projects)),
        "A new workspace must retain the initialization path");
    require(!take(App::registeredProjectForSetupFolder(pathText(tree.base / L"project" / L"child"), projects)) &&
            !take(App::registeredProjectForSetupFolder(pathText(tree.base / L"project-peer"), projects)),
        "Child and prefix sibling folders must not inherit an existing project registration");
    auto conflicting = projects;
    conflicting.push_back({take(Domain::ProjectId::parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd")),
        "Ambiguous workspace", std::nullopt, {folder}});
    const auto ambiguous = App::registeredProjectForSetupFolder(folder, conflicting);
    require(!ambiguous && ambiguous.error().code == Domain::ErrorCodes::IntegrityFailure,
        "An alias registered to multiple project identities must fail explicitly");
    const auto relative = App::registeredProjectForSetupFolder(
        take(Domain::PathText::create("project")), projects);
    require(!relative && relative.error().code == Domain::ErrorCodes::InvalidRequest,
        "Prepare must reject relative workspace folders");

    const Domain::OperationContext context{
        take(Domain::OperationId::parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc")),
        std::chrono::steady_clock::now() + std::chrono::seconds{30}, {},
        take(Domain::CorrelationId::parse("registered-project-prepare"))};
    auto policy = Windows::WindowsWorkspaceAuthorityPolicy{
        take(Domain::AuthorityId::parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        projectId, take(Domain::ClientId::parse("desktop-project-prepare")),
        {folder}, Domain::FileAccess::Read, {Domain::FileAccess::Read}, {}, false, 1U};
    Windows::WindowsWorkspaceAuthority authority{{policy}};
    const auto authorized = take(App::authorizeRegisteredSetupFolder(*selected,
        folder, authority, context));
    require(App::ProjectSetupDetail::samePath(authorized, folder),
        "Reusing a registered project must authorize and read back its exact canonical root");
    const auto noncanonical = App::authorizeRegisteredSetupFolder(*selected,
        normalizedFolder, authority, context);
    require(!noncanonical && noncanonical.error().code == Domain::ErrorCodes::InvalidRequest,
        "Lexical matching must retain native rejection of noncanonical path components");
    const auto child = App::authorizeRegisteredSetupFolder(*selected,
        pathText(tree.base / L"project" / L"child"), authority, context);
    require(!child && child.error().code == Domain::ErrorCodes::ProjectScopeMismatch,
        "Read authority over children must not permit registered project reuse for a child");
    policy.intent = Domain::FileAccess::Write;
    policy.grants = {Domain::FileAccess::Write};
    Windows::WindowsWorkspaceAuthority writeOnly{{policy}};
    const auto denied = App::authorizeRegisteredSetupFolder(*selected, folder, writeOnly, context);
    require(!denied && denied.error().code == Domain::ErrorCodes::Unauthorized,
        "Registered project reuse must propagate a missing Read grant");
    auto spoofed = *selected;
    spoofed.alias = pathText(tree.base / L"project-peer");
    const auto wrongRoot = App::authorizeRegisteredSetupFolder(spoofed, spoofed.alias, authority, context);
    require(!wrongRoot && wrongRoot.error().code == Domain::ErrorCodes::ProjectScopeMismatch,
        "Caller-supplied roots absent from the registered descriptor must fail before reuse");
    auto expired = context;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    const auto timedOut = App::authorizeRegisteredSetupFolder(*selected, folder, authority, expired);
    require(!timedOut && timedOut.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "Prepare must respect native authorization deadlines");
}

class Backend final : public IProjectSetupOperations {
public:
    int calls{};
    int failAt{-1};
    bool omitIdentity{};
    bool throwFailure{};
    SetupOperationResult next()
    {
        ++calls;
        if (throwFailure) throw std::runtime_error{"offline"};
        return {calls != failAt, calls == failAt ? "Needs repair" : "Verified"};
    }
    SetupOperationResult ensureManager(std::stop_token) override { return next(); }
    SetupOperationResult ensurePlugins(ProjectSetupSnapshot& result, std::stop_token) override
    {
        require(result.projectId == "existing-project", "Plugin preparation must receive the registered project identity");
        return next();
    }
    SetupOperationResult ensureProject(ProjectSetupSnapshot& result, std::stop_token) override
    {
        if (!omitIdentity) result.projectId = "existing-project";
        return next();
    }
    SetupOperationResult ensureProvider(ProjectSetupSnapshot& result, std::stop_token) override
    {
        result.model = "local-model";
        return next();
    }
    SetupOperationResult verifyProvider(ProjectSetupSnapshot&, std::stop_token) override
    { return next(); }
};
}

int main()
{
    try {
        registeredWorkspaceReuse();
        for (int failedStage = 1; failedStage <= 5; ++failedStage) {
            Backend backend;
            backend.failAt = failedStage;
            ProjectSetupCoordinator coordinator{backend};
            auto result = coordinator.prepare("C:/project", {});
            require(!result.ready, "A failed stage must not advertise readiness");
            require(backend.calls == failedStage, "No dependent step may run after failure");
            require(result.checks[static_cast<std::size_t>(failedStage - 1)].state ==
                SetupState::NeedsAction, "Failure must provide an actionable state");
            backend.calls = 0;
            backend.failAt = -1;
            auto retry = coordinator.prepare("C:/project", {});
            require(retry.checks.size() == 5 && retry.checks[2].stage == SetupStage::Plugins,
                "Default setup must install plugins before preparing the model");
            require(retry.ready && retry.projectId == "existing-project",
                "Retry must revalidate all stages and retain authoritative project identity");
        }
        Backend backend;
        ProjectSetupCoordinator coordinator{backend};
        std::stop_source stop;
        auto cancelled = coordinator.prepare("C:/project", stop.get_token(),
            [&stop](const ProjectSetupSnapshot& snapshot) {
                if (snapshot.checks[0].state == SetupState::Ready) stop.request_stop();
            });
        require(!cancelled.ready && backend.calls == 1,
            "Cancellation must prevent further setup mutations");
        backend.calls = 0;
        backend.omitIdentity = true;
        auto missing = coordinator.prepare("C:/project", {});
        require(!missing.ready && backend.calls == 2,
            "A success flag without project readback must fail closed");
        backend.throwFailure = true;
        auto exception = coordinator.prepare("C:/project", {});
        require(!exception.ready && exception.checks[0].detail == "offline",
            "Backend exceptions must become visible recovery states");
        std::cout << "Project setup failure, cancellation, identity and retry checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
