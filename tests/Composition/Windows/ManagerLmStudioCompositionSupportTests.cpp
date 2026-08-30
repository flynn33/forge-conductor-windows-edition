#include "ManagerLmStudioAuthorityRouter.h"
#include "UnavailableLmStudioDeploymentService.h"

#include "ForgeConductor/Contracts/IToolServices.h"
#include "ForgeConductor/Domain/Error.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Composition::ManagerLmStudioAuthorityRouter>);
static_assert(std::is_base_of_v<
              Contracts::IWorkspaceAuthority,
              Composition::ManagerLmStudioAuthorityRouter>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerLmStudioAuthorityRouter>);
static_assert(!std::is_move_constructible_v<
              Composition::ManagerLmStudioAuthorityRouter>);
static_assert(std::is_final_v<
              Composition::UnavailableLmStudioDeploymentService>);
static_assert(std::is_base_of_v<
              Contracts::ILMStudioDeploymentService,
              Composition::UnavailableLmStudioDeploymentService>);
static_assert(!std::is_copy_constructible_v<
              Composition::UnavailableLmStudioDeploymentService>);
static_assert(!std::is_move_constructible_v<
              Composition::UnavailableLmStudioDeploymentService>);
static_assert(
    Composition::UnavailableLmStudioDeploymentService::MaximumReasonBytes ==
    512U);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::ProjectId projectId()
{
    return parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
}

[[nodiscard]] Domain::ProjectId otherProjectId()
{
    return parse<Domain::ProjectId>(
        "22222222-2222-4222-8222-222222222222");
}

[[nodiscard]] Domain::ClientId callerId()
{
    return parse<Domain::ClientId>("manager-lmstudio-maintenance");
}

[[nodiscard]] Domain::AuthorityId readAuthorityId()
{
    return parse<Domain::AuthorityId>(
        "33333333-3333-4333-8333-333333333333");
}

[[nodiscard]] Domain::AuthorityId writeAuthorityId()
{
    return parse<Domain::AuthorityId>(
        "44444444-4444-4444-8444-444444444444");
}

[[nodiscard]] Domain::OperationContext routerContext(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "55555555-5555-4555-8555-555555555555"),
        std::chrono::steady_clock::now() + 2min,
        cancellation,
        parse<Domain::CorrelationId>("manager-lmstudio-router")};
}

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error{"The test path UTF-8 size could not be resolved."};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required, nullptr,
            nullptr) != required) {
        throw std::runtime_error{"The test path could not be converted to UTF-8."};
    }
    return result;
}

class ScopedTestDirectory final {
public:
    ScopedTestDirectory()
        : path_{std::filesystem::temp_directory_path() /
                (L"forge-manager-lmstudio-support-" +
                 std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()))},
          root_{take(Domain::PathText::create(utf8(path_.native())))}
    {
        std::error_code error;
        if (!std::filesystem::create_directories(path_, error) || error) {
            throw std::runtime_error{
                "The Manager LM Studio support test root could not be created."};
        }
    }

    ~ScopedTestDirectory() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

    [[nodiscard]] const Domain::PathText& root() const noexcept
    {
        return root_;
    }

private:
    std::filesystem::path path_;
    Domain::PathText root_;
};

[[nodiscard]] Infrastructure::WindowsWorkspaceAuthorityPolicy readPolicy(
    const Domain::PathText& root,
    const Domain::AuthorityId& authorityId = readAuthorityId(),
    const Domain::ProjectId& project = projectId(),
    const Domain::ClientId& caller = callerId())
{
    return Infrastructure::WindowsWorkspaceAuthorityPolicy{
        authorityId,
        project,
        caller,
        {root},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        false,
        7U};
}

[[nodiscard]] Infrastructure::WindowsWorkspaceAuthorityPolicy writePolicy(
    const Domain::PathText& root,
    const Domain::AuthorityId& authorityId = writeAuthorityId(),
    const Domain::ProjectId& project = projectId(),
    const Domain::ClientId& caller = callerId())
{
    return Infrastructure::WindowsWorkspaceAuthorityPolicy{
        authorityId,
        project,
        caller,
        {root},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        {},
        true,
        11U};
}

class AuthorityFixture final {
public:
    AuthorityFixture()
        : readIssuer_{std::vector<
              Infrastructure::WindowsWorkspaceAuthorityPolicy>{
              readPolicy(directory_.root())}},
          writeIssuer_{std::vector<
              Infrastructure::WindowsWorkspaceAuthorityPolicy>{
              writePolicy(directory_.root())}},
          read_{take(readIssuer_.authorityFor(projectId(), routerContext()))},
          write_{take(writeIssuer_.authorityFor(projectId(), routerContext()))},
          router_{readIssuer_, read_, writeIssuer_, write_}
    {
    }

    [[nodiscard]] const Domain::PathText& root() const noexcept
    {
        return directory_.root();
    }

    ScopedTestDirectory directory_;
    Infrastructure::WindowsWorkspaceAuthority readIssuer_;
    Infrastructure::WindowsWorkspaceAuthority writeIssuer_;
    Contracts::WorkspaceAuthority read_;
    Contracts::WorkspaceAuthority write_;
    Composition::ManagerLmStudioAuthorityRouter router_;
};

void routesOnlyByDistinctCapabilityIdentity()
{
    AuthorityFixture fixture;

    require(
        fixture.router_.readAuthority().authorityId() ==
            fixture.read_.authorityId(),
        "router changed the read capability identity");
    require(
        fixture.router_.writeAuthority().authorityId() ==
            fixture.write_.authorityId(),
        "router changed the deployment capability identity");

    requireError(
        fixture.router_.authorityFor(projectId(), routerContext()),
        Domain::ErrorCodes::Conflict,
        "router issued an ambiguous aggregate project capability");
    requireError(
        fixture.router_.authorityFor(otherProjectId(), routerContext()),
        Domain::ErrorCodes::ProjectNotFound,
        "router changed an unknown project into an ambiguous capability");

    std::stop_source cancelled;
    cancelled.request_stop();
    requireError(
        fixture.router_.authorityFor(
            projectId(), routerContext(cancelled.get_token())),
        Domain::ErrorCodes::Cancelled,
        "router ambiguity hid operation cancellation");

    const auto readPath = take(fixture.router_.authorize(
        fixture.read_,
        Domain::PathAuthorizationRequest{
            fixture.root(), std::nullopt, Domain::FileAccess::Read, false},
        routerContext()));
    require(
        readPath.authorityId() == fixture.read_.authorityId() &&
            readPath.access() == Domain::FileAccess::Read,
        "read authorization was not delegated to its issuer");

    const auto writePath = take(fixture.router_.authorize(
        fixture.write_,
        Domain::PathAuthorizationRequest{
            fixture.root(), std::nullopt, Domain::FileAccess::Write, false},
        routerContext()));
    require(
        writePath.authorityId() == fixture.write_.authorityId() &&
            writePath.access() == Domain::FileAccess::Write,
        "deployment authorization was not delegated to its issuer");

    requireError(
        fixture.router_.authorize(
            fixture.read_,
            Domain::PathAuthorizationRequest{
                fixture.root(), std::nullopt, Domain::FileAccess::Write,
                false},
            routerContext()),
        Domain::ErrorCodes::Unauthorized,
        "router widened the exact read capability");

    const auto narrowedRead = take(fixture.router_.narrow(
        fixture.read_, {fixture.read_.trustedRoots().front()},
        {Domain::FileAccess::Read}, false,
        fixture.read_.generation() + 1U, routerContext()));
    require(
        narrowedRead.authorityId() == fixture.read_.authorityId() &&
            narrowedRead.generation() == fixture.read_.generation() + 1U,
        "read narrowing was not delegated by capability identity");

    const auto narrowedWrite = take(fixture.router_.narrow(
        fixture.write_, {fixture.write_.trustedRoots().front()},
        {Domain::FileAccess::Read,
         Domain::FileAccess::Write,
         Domain::FileAccess::Create,
         Domain::FileAccess::Delete,
         Domain::FileAccess::Execute},
        true, fixture.write_.generation() + 1U, routerContext()));
    require(
        narrowedWrite.authorityId() == fixture.write_.authorityId() &&
            narrowedWrite.generation() == fixture.write_.generation() + 1U,
        "deployment narrowing was not delegated by capability identity");

    Infrastructure::WindowsWorkspaceAuthority foreignIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            readPolicy(
                fixture.root(),
                parse<Domain::AuthorityId>(
                    "66666666-6666-4666-8666-666666666666"))}};
    const auto foreign = take(
        foreignIssuer.authorityFor(projectId(), routerContext()));
    requireError(
        fixture.router_.authorize(
            foreign,
            Domain::PathAuthorizationRequest{
                fixture.root(), std::nullopt, Domain::FileAccess::Read,
                false},
            routerContext()),
        Domain::ErrorCodes::Unauthorized,
        "router accepted a foreign capability identity");
    requireError(
        fixture.router_.narrow(
            foreign, {foreign.trustedRoots().front()},
            {Domain::FileAccess::Read}, false,
            foreign.generation() + 1U, routerContext()),
        Domain::ErrorCodes::Unauthorized,
        "router narrowed a foreign capability identity");
}

void rejectsNoncanonicalOrAmbiguousRouterConstruction()
{
    AuthorityFixture fixture;

    const auto expectInvalid = [&](Infrastructure::WindowsWorkspaceAuthority& readIssuer,
                                   const Contracts::WorkspaceAuthority& read,
                                   Infrastructure::WindowsWorkspaceAuthority& writeIssuer,
                                   const Contracts::WorkspaceAuthority& write,
                                   const std::string_view message) {
        bool rejected{};
        try {
            Composition::ManagerLmStudioAuthorityRouter invalid{
                readIssuer, read, writeIssuer, write};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, message);
    };

    expectInvalid(
        fixture.readIssuer_, fixture.read_, fixture.readIssuer_, fixture.write_,
        "router accepted one concrete issuer twice");

    Infrastructure::WindowsWorkspaceAuthority duplicateIdIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            writePolicy(fixture.root(), readAuthorityId())}};
    const auto duplicateId = take(
        duplicateIdIssuer.authorityFor(projectId(), routerContext()));
    expectInvalid(
        fixture.readIssuer_, fixture.read_, duplicateIdIssuer, duplicateId,
        "router accepted one identifier for two capabilities");

    Infrastructure::WindowsWorkspaceAuthority otherProjectIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            writePolicy(
                fixture.root(), writeAuthorityId(), otherProjectId())}};
    const auto otherProject = take(otherProjectIssuer.authorityFor(
        otherProjectId(), routerContext()));
    expectInvalid(
        fixture.readIssuer_, fixture.read_, otherProjectIssuer, otherProject,
        "router accepted mismatched maintenance projects");

    Infrastructure::WindowsWorkspaceAuthority otherCallerIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            writePolicy(
                fixture.root(), writeAuthorityId(), projectId(),
                parse<Domain::ClientId>("foreign-manager-caller"))}};
    const auto otherCaller = take(
        otherCallerIssuer.authorityFor(projectId(), routerContext()));
    expectInvalid(
        fixture.readIssuer_, fixture.read_, otherCallerIssuer, otherCaller,
        "router accepted mismatched maintenance callers");

    auto broadReadPolicy = readPolicy(fixture.root());
    broadReadPolicy.grants = {
        Domain::FileAccess::Read, Domain::FileAccess::Write};
    broadReadPolicy.denials = {
        Domain::FileAccess::Create,
        Domain::FileAccess::Delete,
        Domain::FileAccess::Execute};
    Infrastructure::WindowsWorkspaceAuthority broadReadIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            std::move(broadReadPolicy)}};
    const auto broadRead = take(
        broadReadIssuer.authorityFor(projectId(), routerContext()));
    expectInvalid(
        broadReadIssuer, broadRead, fixture.writeIssuer_, fixture.write_,
        "router accepted a broadened read capability");

    auto narrowWritePolicy = writePolicy(fixture.root());
    narrowWritePolicy.grants = {
        Domain::FileAccess::Read,
        Domain::FileAccess::Write,
        Domain::FileAccess::Create,
        Domain::FileAccess::Delete};
    narrowWritePolicy.denials = {Domain::FileAccess::Execute};
    Infrastructure::WindowsWorkspaceAuthority narrowWriteIssuer{
        std::vector<Infrastructure::WindowsWorkspaceAuthorityPolicy>{
            std::move(narrowWritePolicy)}};
    const auto narrowWrite = take(
        narrowWriteIssuer.authorityFor(projectId(), routerContext()));
    expectInvalid(
        fixture.readIssuer_, fixture.read_, narrowWriteIssuer, narrowWrite,
        "router accepted an incomplete deployment capability");
}

class Clock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utcValue{Domain::UtcTimePoint{} + 10s};
    Domain::MonotonicTimePoint monotonicValue{
        Domain::MonotonicTimePoint{} + 20s};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utcValue;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept
        override
    {
        return monotonicValue;
    }
};

[[nodiscard]] Domain::OperationContext serviceContext(
    const Domain::MonotonicTimePoint deadline,
    const std::string_view operation =
        "77777777-7777-4777-8777-777777777777",
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operation),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>("manager-lmstudio-unavailable")};
}

class ToolAuthorizer final : public Contracts::IToolAuthorizer {
public:
    [[nodiscard]] Domain::Result<Contracts::AuthorizedToolCall> authorize(
        const Domain::ToolAuthorizationRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override
    {
        return issueAuthorizedToolCall(request, authority, context);
    }
};

[[nodiscard]] Contracts::AuthorizedToolCall authorizedCall(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context)
{
    ToolAuthorizer authorizer;
    return take(authorizer.authorize(
        Domain::ToolAuthorizationRequest{
            Domain::ToolCallRequest{
                Domain::McpRequestMetadata{
                    parse<Domain::RequestId>("lmstudio-unavailable-request"),
                    context.correlationId,
                    authority.callerId(),
                    authority.projectId(),
                    "2025-06-18"},
                "install-lmstudio-plugin",
                "{\"preserveForeignEntries\":true}"},
            Domain::ToolEffect::Write,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}},
        authority,
        context));
}

template <typename Value>
void requireUnavailable(
    const Domain::Result<Value>& result,
    const std::string_view reason,
    const std::string_view message)
{
    requireError(
        result, Domain::ErrorCodes::HostCapabilityUnavailable, message);
    require(result.error().message == reason, message);
    require(result.error().retryable, message);
    require(!result.error().evidenceId.has_value(), message);
}

void reportsOptionalHostAbsenceWithoutFabricatedModels()
{
    AuthorityFixture fixture;
    Clock clock;
    const std::string reason{
        "LM Studio is not installed or no supported native installation was discovered."};
    Composition::UnavailableLmStudioDeploymentService service{clock, reason};
    const auto context = serviceContext(clock.monotonicValue + 1min);
    const auto authorization = authorizedCall(fixture.write_, context);
    const Domain::LMStudioDeploymentRequest deploymentRequest{
        std::nullopt, true};

    require(service.reason() == reason, "unavailable reason changed");
    requireUnavailable(
        service.status(deploymentRequest, fixture.read_, context),
        reason,
        "unavailable status fabricated an LM Studio model");
    requireUnavailable(
        service.deploy(
            deploymentRequest, fixture.write_, authorization, context),
        reason,
        "unavailable deployment fabricated an install result");
    requireUnavailable(
        service.activate(
            Domain::LMStudioHostActivationRequest{
                parse<Domain::DeploymentId>("unavailable-deployment"), 10s},
            fixture.write_, authorization, context),
        reason,
        "unavailable activation fabricated a host result");
}

void cancellationDeadlineAndShutdownPrecedeCapabilityFailure()
{
    AuthorityFixture fixture;
    Clock clock;
    Composition::UnavailableLmStudioDeploymentService service{
        clock, "LM Studio native deployment is unavailable."};
    const Domain::LMStudioDeploymentRequest request{std::nullopt, true};

    std::stop_source cancelled;
    cancelled.request_stop();
    requireError(
        service.status(
            request,
            fixture.read_,
            serviceContext(
                clock.monotonicValue + 1min,
                "88888888-8888-4888-8888-888888888888",
                cancelled.get_token())),
        Domain::ErrorCodes::Cancelled,
        "context cancellation was hidden by capability unavailability");

    requireError(
        service.status(
            request,
            fixture.read_,
            serviceContext(
                clock.monotonicValue,
                "99999999-9999-4999-8999-999999999999")),
        Domain::ErrorCodes::DeadlineExceeded,
        "expired deadline was hidden by capability unavailability");

    const auto exactCancelled = serviceContext(
        clock.monotonicValue + 1min,
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    service.cancel(exactCancelled.operationId);
    service.cancel(exactCancelled.operationId);
    requireError(
        service.status(request, fixture.read_, exactCancelled),
        Domain::ErrorCodes::Cancelled,
        "exact operation cancellation was not retained");

    const auto replacement = serviceContext(
        clock.monotonicValue + 1min,
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    service.cancel(replacement.operationId);
    requireUnavailable(
        service.status(request, fixture.read_, exactCancelled),
        service.reason(),
        "bounded cancellation retained more than one operation");
    requireError(
        service.status(request, fixture.read_, replacement),
        Domain::ErrorCodes::Cancelled,
        "replacement cancellation was not retained");

    service.shutdown();
    service.shutdown();
    service.cancel(exactCancelled.operationId);
    requireError(
        service.status(
            request,
            fixture.read_,
            serviceContext(
                clock.monotonicValue + 1min,
                "cccccccc-cccc-4ccc-8ccc-cccccccccccc")),
        Domain::ErrorCodes::Cancelled,
        "shutdown did not close unavailable service admission");
}

void requiresOneBoundedUnavailableReason()
{
    Clock clock;
    const auto expectInvalid = [&](std::string reason,
                                   const std::string_view message) {
        bool rejected{};
        try {
            Composition::UnavailableLmStudioDeploymentService invalid{
                clock, std::move(reason)};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, message);
    };

    expectInvalid({}, "unavailable service accepted an empty reason");
    expectInvalid(
        std::string(
            Composition::UnavailableLmStudioDeploymentService::
                    MaximumReasonBytes +
                1U,
            'x'),
        "unavailable service accepted an unbounded reason");

    Composition::UnavailableLmStudioDeploymentService exactBound{
        clock,
        std::string(
            Composition::UnavailableLmStudioDeploymentService::
                MaximumReasonBytes,
            'x')};
    require(
        exactBound.reason().size() ==
            Composition::UnavailableLmStudioDeploymentService::
                MaximumReasonBytes,
        "unavailable service changed an exactly bounded reason");
}

} // namespace

int main()
{
    try {
        routesOnlyByDistinctCapabilityIdentity();
        rejectsNoncanonicalOrAmbiguousRouterConstruction();
        reportsOptionalHostAbsenceWithoutFabricatedModels();
        cancellationDeadlineAndShutdownPrecedeCapabilityFailure();
        requiresOneBoundedUnavailableReason();
        std::cout << "Manager LM Studio composition support tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager LM Studio composition support tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager LM Studio composition support tests failed with "
                     "an unknown error.\n";
        return 1;
    }
}
