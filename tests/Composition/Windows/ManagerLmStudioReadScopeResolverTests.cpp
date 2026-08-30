#include "ManagerLmStudioReadScopeResolver.h"

#include "Fakes/DeterministicWorkspaceAuthority.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;

static_assert(std::is_final_v<Composition::ManagerLmStudioReadScope>);
static_assert(std::is_final_v<Composition::ManagerLmStudioReadScopeResolver>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerLmStudioReadScope>);
static_assert(std::is_move_constructible_v<
              Composition::ManagerLmStudioReadScope>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerLmStudioReadScopeResolver>);
static_assert(
    Composition::ManagerLmStudioReadScopeResolver::
        MaximumCandidateEvaluations == 64U);
static_assert(
    Composition::ManagerLmStudioReadScopeResolver::MaximumAuthorityRoots ==
    4U);

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

[[nodiscard]] Domain::OperationContext activeContext(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(
            "d0000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        cancellation,
        parse<Domain::CorrelationId>("manager-lmstudio-read-scope")};
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
        throw std::runtime_error{"A test path could not be encoded as UTF-8."};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required, nullptr,
            nullptr) != required) {
        throw std::runtime_error{"A test path could not be encoded as UTF-8."};
    }
    return result;
}

[[nodiscard]] Domain::PathText path(const std::filesystem::path& value)
{
    return take(Domain::PathText::create(utf8(value.native())));
}

class TempTree final {
public:
    TempTree()
    {
        const auto base = std::filesystem::temp_directory_path();
        root_ = base /
            (L"ForgeConductor-ManagerLmScope-" +
             std::to_wstring(::GetCurrentProcessId()) + L'-' +
             std::to_wstring(
                 std::chrono::steady_clock::now()
                     .time_since_epoch()
                     .count()));
        std::filesystem::create_directories(root_ / L"lm-config");
        std::filesystem::create_directories(root_ / L"lm-app");
        std::filesystem::create_directories(root_ / L"forge-bin");
        std::filesystem::create_directories(root_ / L"forge-data");
        std::filesystem::create_directories(root_ / L"shared-lm");
    }

    ~TempTree() noexcept
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    [[nodiscard]] Domain::PathText root() const { return path(root_); }
    [[nodiscard]] Domain::PathText child(
        const std::wstring_view relative) const
    {
        return path(root_ / std::filesystem::path{relative});
    }

private:
    std::filesystem::path root_;
};

class MapFileSystem final : public Contracts::IFileSystem {
public:
    void seed(
        const Domain::PathText& file,
        const std::string_view content)
    {
        std::vector<std::byte> bytes(content.size());
        if (!content.empty()) {
            std::memcpy(bytes.data(), content.data(), content.size());
        }
        files_.insert_or_assign(file.value(), std::move(bytes));
    }

    [[nodiscard]] Domain::Result<std::vector<std::byte>> readFile(
        const Contracts::AuthorizedPath& file,
        const std::size_t maximumBytes,
        const Domain::OperationContext&) noexcept override
    {
        const auto match = files_.find(file.canonicalPath().value());
        if (match == files_.end()) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::RecordNotFound,
                    "The test file is missing."));
        }
        if (match->second.size() > maximumBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The test file exceeds its read bound."));
        }
        return Domain::Result<std::vector<std::byte>>::success(match->second);
    }

    [[nodiscard]] Domain::Result<void> writeFile(
        const Contracts::AuthorizedPath&,
        std::span<const std::byte>,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<Domain::DirectoryListing> list(
        const Contracts::AuthorizedPath&,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DirectoryListing>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Directory listing is unsupported."));
    }

    [[nodiscard]] Domain::Result<void> createDirectory(
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<void> remove(
        const Contracts::AuthorizedPath&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

    [[nodiscard]] Domain::Result<void> move(
        const Contracts::AuthorizedPath&,
        const Contracts::AuthorizedPath&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported();
    }

private:
    [[nodiscard]] static Domain::Result<void> unsupported()
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Mutation is unsupported by the test filesystem."));
    }

    std::map<std::string, std::vector<std::byte>> files_;
};

[[nodiscard]] Infrastructure::WindowsLMStudioDiscoveryCandidate configuration(
    const Domain::PathText& file,
    const Domain::LMStudioDiscoverySource source =
        Domain::LMStudioDiscoverySource::KnownUserLocation)
{
    return Infrastructure::WindowsLMStudioDiscoveryCandidate{
        source,
        file,
        std::nullopt,
        file,
        std::nullopt,
        std::nullopt,
        true,
        "Configuration candidate."};
}

[[nodiscard]] Infrastructure::WindowsLMStudioDiscoveryCandidate application(
    const Domain::PathText& file,
    const Domain::LMStudioDiscoverySource source =
        Domain::LMStudioDiscoverySource::InstalledApplication)
{
    return Infrastructure::WindowsLMStudioDiscoveryCandidate{
        source,
        file,
        file,
        std::nullopt,
        std::nullopt,
        std::string{"0.4.21"},
        true,
        "Application candidate."};
}

struct Identities final {
    Domain::AuthorityId selectionAuthorityId{parse<Domain::AuthorityId>(
        "d1000000-0000-4000-8000-000000000001")};
    Domain::AuthorityId readScopeAuthorityId{parse<Domain::AuthorityId>(
        "d1000000-0000-4000-8000-000000000002")};
    Domain::ProjectId projectId{parse<Domain::ProjectId>(
        "d2000000-0000-4000-8000-000000000001")};
    Domain::ClientId callerId{
        parse<Domain::ClientId>("manager-lmstudio-maintenance")};
    std::uint64_t selectionGeneration{7U};
    std::uint64_t readScopeGeneration{1U};
};

[[nodiscard]] Composition::ManagerLmStudioReadScopeConfiguration configurationFor(
    const Identities& ids,
    const TempTree& tree)
{
    return Composition::ManagerLmStudioReadScopeConfiguration{
        Composition::ManagerLmStudioReadScopeIdentity{
            ids.selectionAuthorityId,
            ids.projectId,
            ids.callerId,
            ids.selectionGeneration},
        Composition::ManagerLmStudioReadScopeIdentity{
            ids.readScopeAuthorityId,
            ids.projectId,
            ids.callerId,
            ids.readScopeGeneration},
        tree.child(L"forge-data"),
        tree.child(L"forge-bin\\forge-conductor.exe")};
}

[[nodiscard]] Contracts::WorkspaceAuthority selectionAuthority(
    Fakes::DeterministicWorkspaceAuthority& issuer,
    const Identities& ids)
{
    return take(issuer.authorityFor(ids.projectId, activeContext()));
}

[[nodiscard]] Infrastructure::WindowsLMStudioCandidateSelection select(
    Infrastructure::WindowsLMStudioCandidateSelector& selector,
    const Contracts::WorkspaceAuthority& authority,
    std::vector<Infrastructure::WindowsLMStudioDiscoveryCandidate> candidates)
{
    return take(selector.select(
        std::move(candidates), authority, activeContext()));
}

void resolvesExactReadOnlyScopeFromSelectedEvidence()
{
    TempTree tree;
    Identities ids;
    Fakes::DeterministicWorkspaceAuthority selectionIssuer{
        ids.selectionAuthorityId,
        ids.callerId,
        {tree.root()},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {Domain::FileAccess::Execute},
        false,
        ids.selectionGeneration};
    auto broadSelectionAuthority = selectionAuthority(selectionIssuer, ids);
    MapFileSystem files;
    const auto configurationPath = tree.child(L"lm-config\\mcp.json");
    const auto applicationPath = tree.child(L"lm-app\\LM Studio.exe");
    files.seed(configurationPath, R"({"mcpServers":{}})");
    Infrastructure::WindowsLMStudioCandidateSelector selector{
        selectionIssuer, files};
    auto selected = select(
        selector,
        broadSelectionAuthority,
        {application(applicationPath), configuration(configurationPath)});

    Composition::ManagerLmStudioReadScopeResolver resolver{
        configurationFor(ids, tree)};
    auto scope = take(resolver.resolve(selected, activeContext()));
    const auto& authority = scope.authority();
    require(
        authority.authorityId() == ids.readScopeAuthorityId &&
            authority.projectId() == ids.projectId &&
            authority.callerId() == ids.callerId &&
            authority.generation() == ids.readScopeGeneration,
        "The resolved scope changed its injected maintenance identity.");
    require(
        authority.intent() == Domain::FileAccess::Read &&
            authority.grants() ==
                std::vector<Domain::FileAccess>{Domain::FileAccess::Read} &&
            authority.denials() ==
                std::vector<Domain::FileAccess>{
                    Domain::FileAccess::Write,
                    Domain::FileAccess::Create,
                    Domain::FileAccess::Delete,
                    Domain::FileAccess::Execute} &&
            !authority.shellEnabled(),
        "The resolved scope was not exactly read-only and shell-disabled.");
    require(
        authority.trustedRoots().size() == 4U &&
            std::find(
                authority.trustedRoots().begin(),
                authority.trustedRoots().end(),
                tree.root()) == authority.trustedRoots().end(),
        "The resolved scope retained a broad discovery root.");

    const auto readable = scope.issuer().authorize(
        authority,
        Domain::PathAuthorizationRequest{
            configurationPath,
            std::nullopt,
            Domain::FileAccess::Read,
            false},
        activeContext());
    require(readable.hasValue(), "The selected configuration was not readable.");
    requireError(
        scope.issuer().authorize(
            authority,
            Domain::PathAuthorizationRequest{
                applicationPath,
                std::nullopt,
                Domain::FileAccess::Execute,
                false},
            activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "The read scope authorized executable access.");
}

void coalescesCorroboratingEvidenceAndRejectsAmbiguousResources()
{
    TempTree tree;
    Identities ids;
    Fakes::DeterministicWorkspaceAuthority selectionIssuer{
        ids.selectionAuthorityId,
        ids.callerId,
        {tree.root()},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {},
        false,
        ids.selectionGeneration};
    auto authority = selectionAuthority(selectionIssuer, ids);
    MapFileSystem files;
    const auto configurationPath = tree.child(L"lm-config\\mcp.json");
    const auto applicationPath = tree.child(L"lm-app\\LM Studio.exe");
    files.seed(configurationPath, R"({"mcpServers":{}})");
    Infrastructure::WindowsLMStudioCandidateSelector selector{
        selectionIssuer, files};
    Composition::ManagerLmStudioReadScopeResolver resolver{
        configurationFor(ids, tree)};

    auto duplicate = select(
        selector,
        authority,
        {application(applicationPath),
         configuration(configurationPath),
         configuration(
             configurationPath,
             Domain::LMStudioDiscoverySource::RunningProcess)});
    auto duplicateScope = take(resolver.resolve(duplicate, activeContext()));
    require(
        duplicateScope.authority().trustedRoots().size() == 4U,
        "Corroborating resource evidence broadened the read authority.");

    auto crossClassified = select(
        selector,
        authority,
        {application(applicationPath),
         configuration(configurationPath),
         configuration(
             applicationPath,
             Domain::LMStudioDiscoverySource::RunningProcess)});
    requireError(
        resolver.resolve(crossClassified, activeContext()),
        Domain::ErrorCodes::Conflict,
        "One resource path was accepted as both application and configuration.");

    auto ambiguousCandidate = application(
        tree.child(L"shared-lm\\LM Studio.exe"));
    ambiguousCandidate.configurationPath = configurationPath;
    auto ambiguous = select(
        selector,
        authority,
        {std::move(ambiguousCandidate),
         application(applicationPath),
         configuration(configurationPath)});
    requireError(
        resolver.resolve(ambiguous, activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "An ambiguous candidate was authorized.");

    auto missingConfiguration = select(
        selector, authority, {application(applicationPath)});
    requireError(
        resolver.resolve(missingConfiguration, activeContext()),
        Domain::ErrorCodes::RecordNotFound,
        "A scope was issued without a selected configuration.");
}

void rejectsForeignStaleAndOversizedSelectionEvidence()
{
    TempTree tree;
    Identities ids;
    Fakes::DeterministicWorkspaceAuthority selectionIssuer{
        ids.selectionAuthorityId,
        ids.callerId,
        {tree.root()},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {},
        false,
        ids.selectionGeneration};
    auto authority = selectionAuthority(selectionIssuer, ids);
    MapFileSystem files;
    const auto configurationPath = tree.child(L"lm-config\\mcp.json");
    const auto applicationPath = tree.child(L"lm-app\\LM Studio.exe");
    files.seed(configurationPath, R"({"mcpServers":{}})");
    Infrastructure::WindowsLMStudioCandidateSelector selector{
        selectionIssuer, files,
        Infrastructure::WindowsLMStudioCandidateSelectorOptions{
            65U,
            Infrastructure::WindowsLMStudioCandidateSelectorOptions::
                DefaultMaximumConfigurationBytes,
            Infrastructure::WindowsLMStudioCandidateSelectorOptions::
                DefaultMaximumJsonDepth}};
    auto selected = select(
        selector,
        authority,
        {application(applicationPath), configuration(configurationPath)});

    auto foreignProject = configurationFor(ids, tree);
    foreignProject.selectionIdentity.projectId = parse<Domain::ProjectId>(
        "d2000000-0000-4000-8000-000000000002");
    foreignProject.readScopeIdentity.projectId =
        foreignProject.selectionIdentity.projectId;
    Composition::ManagerLmStudioReadScopeResolver foreignProjectResolver{
        std::move(foreignProject)};
    requireError(
        foreignProjectResolver.resolve(selected, activeContext()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "Foreign project selection evidence was accepted.");

    auto stale = configurationFor(ids, tree);
    ++stale.selectionIdentity.generation;
    Composition::ManagerLmStudioReadScopeResolver staleResolver{
        std::move(stale)};
    requireError(
        staleResolver.resolve(selected, activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "Stale authority selection evidence was accepted.");

    auto colliding = configurationFor(ids, tree);
    colliding.readScopeIdentity.authorityId =
        colliding.selectionIdentity.authorityId;
    Composition::ManagerLmStudioReadScopeResolver collidingResolver{
        std::move(colliding)};
    requireError(
        collidingResolver.resolve(selected, activeContext()),
        Domain::ErrorCodes::Conflict,
        "The broad selection and narrow read scope reused one capability "
        "identifier.");

    std::vector<Infrastructure::WindowsLMStudioDiscoveryCandidate> candidates;
    candidates.reserve(65U);
    candidates.push_back(application(applicationPath));
    candidates.push_back(configuration(configurationPath));
    for (std::size_t index = candidates.size(); index < 65U; ++index) {
        candidates.push_back(Infrastructure::WindowsLMStudioDiscoveryCandidate{
            Domain::LMStudioDiscoverySource::RunningProcess,
            take(Domain::PathText::create(
                "unavailable-candidate-" + std::to_string(index))),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            false,
            "Unavailable candidate."});
    }
    auto oversized = select(
        selector, authority, std::move(candidates));
    Composition::ManagerLmStudioReadScopeResolver resolver{
        configurationFor(ids, tree)};
    requireError(
        resolver.resolve(oversized, activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "More than 64 candidate evaluations entered Manager maintenance.");
}

void deduplicatesEqualRootsAndRejectsOverlapOrBroadening()
{
    TempTree tree;
    Identities ids;
    Fakes::DeterministicWorkspaceAuthority selectionIssuer{
        ids.selectionAuthorityId,
        ids.callerId,
        {tree.root()},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {},
        false,
        ids.selectionGeneration};
    auto authority = selectionAuthority(selectionIssuer, ids);
    MapFileSystem files;
    const auto sharedConfiguration = tree.child(L"shared-lm\\mcp.json");
    const auto sharedApplication = tree.child(L"shared-lm\\LM Studio.exe");
    files.seed(sharedConfiguration, R"({"mcpServers":{}})");
    Infrastructure::WindowsLMStudioCandidateSelector selector{
        selectionIssuer, files};
    auto shared = select(
        selector,
        authority,
        {application(sharedApplication), configuration(sharedConfiguration)});
    Composition::ManagerLmStudioReadScopeResolver resolver{
        configurationFor(ids, tree)};
    auto deduplicated = take(resolver.resolve(shared, activeContext()));
    require(
        deduplicated.authority().trustedRoots().size() == 3U,
        "Equal selected resource parents were not deduplicated.");

    auto overlappingConfiguration = configurationFor(ids, tree);
    overlappingConfiguration.forgeDataRoot = tree.root();
    Composition::ManagerLmStudioReadScopeResolver overlappingResolver{
        std::move(overlappingConfiguration)};
    requireError(
        overlappingResolver.resolve(shared, activeContext()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "An ancestor root broadened the maintenance read scope.");

    auto broadConfiguration = configurationFor(ids, tree);
    broadConfiguration.forgeDataRoot =
        take(Domain::PathText::create("C:\\"));
    Composition::ManagerLmStudioReadScopeResolver broadResolver{
        std::move(broadConfiguration)};
    requireError(
        broadResolver.resolve(shared, activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "A drive root was accepted as Manager maintenance authority.");
}

void cancellationStopsBeforeResolution()
{
    TempTree tree;
    Identities ids;
    Fakes::DeterministicWorkspaceAuthority selectionIssuer{
        ids.selectionAuthorityId,
        ids.callerId,
        {tree.root()},
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read},
        {},
        false,
        ids.selectionGeneration};
    auto authority = selectionAuthority(selectionIssuer, ids);
    MapFileSystem files;
    const auto configurationPath = tree.child(L"lm-config\\mcp.json");
    files.seed(configurationPath, R"({"mcpServers":{}})");
    Infrastructure::WindowsLMStudioCandidateSelector selector{
        selectionIssuer, files};
    auto selected = select(
        selector,
        authority,
        {application(tree.child(L"lm-app\\LM Studio.exe")),
         configuration(configurationPath)});
    Composition::ManagerLmStudioReadScopeResolver resolver{
        configurationFor(ids, tree)};
    std::stop_source stop;
    stop.request_stop();
    requireError(
        resolver.resolve(selected, activeContext(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "A cancelled scope resolution reached authority construction.");
}

} // namespace

int main()
{
    try {
        resolvesExactReadOnlyScopeFromSelectedEvidence();
        coalescesCorroboratingEvidenceAndRejectsAmbiguousResources();
        rejectsForeignStaleAndOversizedSelectionEvidence();
        deduplicatesEqualRootsAndRejectsOverlapOrBroadening();
        cancellationStopsBeforeResolution();
        std::cout << "Manager LM Studio read-scope resolver tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager LM Studio read-scope resolver tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager LM Studio read-scope resolver tests failed with "
                     "an unknown error.\n";
        return 1;
    }
}
