#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace WindowsDetail = ForgeConductor::Infrastructure::Windows::Detail;
using namespace std::chrono_literals;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, message);
}

[[nodiscard]] Domain::AuthorityId authorityId()
{
    return parse<Domain::AuthorityId>("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
}

[[nodiscard]] Domain::ProjectId projectId()
{
    return parse<Domain::ProjectId>("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
}

[[nodiscard]] Domain::ClientId callerId()
{
    return parse<Domain::ClientId>("p13-workspace-client");
}

[[nodiscard]] Domain::OperationContext context(
    const std::stop_token cancellation = {},
    const bool expired = false)
{
    const auto now = std::chrono::steady_clock::now();
    return Domain::OperationContext{
        parse<Domain::OperationId>("cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
        expired ? now : now + 5min,
        cancellation,
        parse<Domain::CorrelationId>("p13-workspace-authority")};
}

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path& path)
{
    return take(Domain::PathText::create(
        take(WindowsDetail::strictUtf16ToUtf8(path.native()))));
}

class ScopedTestTree final {
public:
    ScopedTestTree()
    {
        std::wstring temporary(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(),
                "GetTempPathW failed for workspace-authority tests");
        temporary.resize(length);
        base_ = std::filesystem::path{temporary} /
                (L"ForgeConductor.P13.Authority." +
                 std::to_wstring(::GetCurrentProcessId()) + L"." +
                 std::to_wstring(::GetTickCount64()));
        root_ = base_ / L"root";
        secondRoot_ = base_ / L"second";
        prefixPeer_ = base_ / L"root-sibling";
        outside_ = base_ / L"outside";
        caseSensitive_ = base_ / L"case-sensitive";
        require(std::filesystem::create_directories(root_ / L"nested") &&
                    std::filesystem::create_directories(secondRoot_) &&
                    std::filesystem::create_directories(prefixPeer_) &&
                    std::filesystem::create_directories(outside_) &&
                    std::filesystem::create_directories(caseSensitive_),
                "workspace-authority test directories could not be created");
    }

    ~ScopedTestTree() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(base_, ignored));
    }

    ScopedTestTree(const ScopedTestTree&) = delete;
    ScopedTestTree& operator=(const ScopedTestTree&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const std::filesystem::path& secondRoot() const noexcept
    {
        return secondRoot_;
    }
    [[nodiscard]] const std::filesystem::path& prefixPeer() const noexcept
    {
        return prefixPeer_;
    }
    [[nodiscard]] const std::filesystem::path& outside() const noexcept { return outside_; }
    [[nodiscard]] const std::filesystem::path& caseSensitive() const noexcept
    {
        return caseSensitive_;
    }

private:
    std::filesystem::path base_;
    std::filesystem::path root_;
    std::filesystem::path secondRoot_;
    std::filesystem::path prefixPeer_;
    std::filesystem::path outside_;
    std::filesystem::path caseSensitive_;
};

[[nodiscard]] Infrastructure::WindowsWorkspaceAuthorityPolicy policy(
    std::vector<Domain::PathText> roots,
    const std::uint64_t generation = 7U)
{
    return Infrastructure::WindowsWorkspaceAuthorityPolicy{
        authorityId(), projectId(), callerId(), std::move(roots),
        Domain::FileAccess::Read,
        {Domain::FileAccess::Read, Domain::FileAccess::Write,
         Domain::FileAccess::Create, Domain::FileAccess::Delete},
        {Domain::FileAccess::Execute}, false, generation};
}

struct Fixture final {
    Fixture()
        : root{pathText(tree.root())},
          secondRoot{pathText(tree.secondRoot())},
          authority{{policy({root, secondRoot})}}
    {
    }

    ScopedTestTree tree;
    Domain::PathText root;
    Domain::PathText secondRoot;
    Infrastructure::WindowsWorkspaceAuthority authority;
};

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::WorkspaceAuthority issue(
        Domain::AuthorityId id,
        Domain::ProjectId project,
        Domain::ClientId caller,
        std::vector<Domain::PathText> roots,
        const std::uint64_t generation)
    {
        return take(issueAuthority(
            std::move(id), std::move(project), std::move(caller),
            std::move(roots), Domain::FileAccess::Read,
            {Domain::FileAccess::Read, Domain::FileAccess::Write,
             Domain::FileAccess::Create, Domain::FileAccess::Delete},
            {Domain::FileAccess::Execute}, false, generation));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::WorkspaceAuthority>();
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::WorkspaceAuthority>();
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Contracts::AuthorizedPath>();
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unsupported()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The test issuer only exposes its capability factory."));
    }
};

void issuesCanonicalAuthorityAndPaths()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    require(authority.authorityId() == authorityId() &&
                authority.projectId() == projectId() &&
                authority.callerId() == callerId() && authority.generation() == 7U,
            "authorityFor changed the configured identity binding");
    require(authority.trustedRoots() ==
                std::vector<Domain::PathText>{fixture.root, fixture.secondRoot},
            "authorityFor changed the canonical roots");

    const auto target = pathText(fixture.tree.root() / L"nested" / L"new.txt");
    const auto authorized = take(fixture.authority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            target, fixture.root, Domain::FileAccess::Write, false},
        context()));
    require(authorized.authorityId() == authority.authorityId() &&
                authorized.authorityRoot() == fixture.root &&
                authorized.canonicalPath() == target &&
                authorized.access() == Domain::FileAccess::Write,
            "authorize did not issue the exact canonical path capability");

    const auto inferred = take(fixture.authority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            target, std::nullopt, Domain::FileAccess::Read, false},
        context()));
    require(inferred.authorityRoot() == fixture.root,
            "authorize did not infer the sole containing root");

    requireError(
        fixture.authority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                target, pathText(fixture.tree.root() / L"nested"),
                Domain::FileAccess::Read, false},
            context()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "authorize accepted a base path that was not an exact trusted root");
}

void rejectsMissingProjectAndMismatchedCapabilities()
{
    Fixture fixture;
    const auto request = Domain::PathAuthorizationRequest{
        pathText(fixture.tree.root() / L"nested" / L"file.txt"),
        fixture.root, Domain::FileAccess::Read, false};
    const auto otherProject = parse<Domain::ProjectId>(
        "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    requireError(
        fixture.authority.authorityFor(otherProject, context()),
        Domain::ErrorCodes::ProjectNotFound,
        "authorityFor accepted an unconfigured project");

    const auto otherId = parse<Domain::AuthorityId>(
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const auto wrongId = CapabilityIssuer::issue(
        otherId, projectId(), callerId(), {fixture.root, fixture.secondRoot}, 7U);
    requireError(
        fixture.authority.authorize(wrongId, request, context()),
        Domain::ErrorCodes::Unauthorized,
        "authorize accepted an unconfigured authority identifier");

    const auto wrongProject = CapabilityIssuer::issue(
        authorityId(), otherProject, callerId(),
        {fixture.root, fixture.secondRoot}, 7U);
    requireError(
        fixture.authority.authorize(wrongProject, request, context()),
        Domain::ErrorCodes::ProjectScopeMismatch,
        "authorize accepted a capability bound to another project");

    const auto wrongCaller = CapabilityIssuer::issue(
        authorityId(), projectId(), parse<Domain::ClientId>("wrong-caller"),
        {fixture.root, fixture.secondRoot}, 7U);
    requireError(
        fixture.authority.authorize(wrongCaller, request, context()),
        Domain::ErrorCodes::Unauthorized,
        "authorize accepted a capability bound to another caller");

    const auto stale = CapabilityIssuer::issue(
        authorityId(), projectId(), callerId(),
        {fixture.root, fixture.secondRoot}, 6U);
    requireError(
        fixture.authority.authorize(stale, request, context()),
        Domain::ErrorCodes::Unauthorized,
        "authorize accepted a stale authority generation");
}

void narrowsOnlyWithinTheConfiguredBaseline()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    const auto narrowed = take(fixture.authority.narrow(
        authority, {fixture.root}, {Domain::FileAccess::Read}, false, 8U, context()));
    require(narrowed.authorityId() == authority.authorityId() &&
                narrowed.projectId() == authority.projectId() &&
                narrowed.callerId() == authority.callerId() &&
                narrowed.generation() == 8U &&
                narrowed.trustedRoots() == std::vector<Domain::PathText>{fixture.root},
            "narrow changed identity or failed to reduce roots");
    const auto narrowedRead = take(fixture.authority.authorize(
        narrowed,
        Domain::PathAuthorizationRequest{
            pathText(fixture.tree.root() / L"nested" / L"read.txt"),
            fixture.root, Domain::FileAccess::Read, false},
        context()));
    require(narrowedRead.authorityRoot() == fixture.root,
            "the narrowed authority did not issue its retained root");

    requireError(
        fixture.authority.authorize(
            narrowed,
            Domain::PathAuthorizationRequest{
                pathText(fixture.tree.secondRoot() / L"outside.txt"),
                std::nullopt, Domain::FileAccess::Read, false},
            context()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "a narrowed capability retained a removed root");
    requireError(
        fixture.authority.narrow(
            authority, {pathText(fixture.tree.outside())},
            {Domain::FileAccess::Read}, false, 8U, context()),
        Domain::ErrorCodes::Unauthorized,
        "narrow accepted a root outside the capability");
    requireError(
        fixture.authority.narrow(
            authority, {fixture.root}, {Domain::FileAccess::Read}, true, 8U,
            context()),
        Domain::ErrorCodes::Unauthorized,
        "narrow enabled shell access");
    requireError(
        fixture.authority.narrow(
            authority, {fixture.root}, {Domain::FileAccess::Read}, false, 7U,
            context()),
        Domain::ErrorCodes::Unauthorized,
        "narrow reused the current generation");
}

void rejectsOutsideTraversalAndWindowsNamespaceForms()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    const auto expectRejected = [&](const Domain::PathText& requested) {
        const auto result = fixture.authority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                requested, std::nullopt, Domain::FileAccess::Read, false},
            context());
        require(!result, "authorize accepted an unsafe Windows path");
    };

    expectRejected(pathText(fixture.tree.outside() / L"file.txt"));
    expectRejected(pathText(fixture.tree.prefixPeer() / L"file.txt"));
    expectRejected(take(Domain::PathText::create(
        fixture.root.value() + "\\..\\outside\\file.txt")));
    expectRejected(take(Domain::PathText::create("\\\\server\\share\\file.txt")));
    expectRejected(take(Domain::PathText::create("\\\\?\\C:\\file.txt")));
    expectRejected(take(Domain::PathText::create(
        fixture.root.value() + "\\file.txt:stream")));
    expectRejected(take(Domain::PathText::create("relative\\file.txt")));
}

void protectsTheAuthorityRootFromDestructiveRequests()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    requireError(
        fixture.authority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                fixture.root, fixture.root, Domain::FileAccess::Delete, true},
            context()),
        Domain::ErrorCodes::Unauthorized,
        "root protection accepted deletion of the authority root");
    const auto protectedRead = take(fixture.authority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            fixture.root, fixture.root, Domain::FileAccess::Read, true},
        context()));
    require(protectedRead.canonicalPath() == fixture.root,
            "root protection blocked a non-destructive read");
    const auto childDelete = take(fixture.authority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            pathText(fixture.tree.root() / L"nested"), fixture.root,
            Domain::FileAccess::Delete, true},
        context()));
    require(childDelete.access() == Domain::FileAccess::Delete,
            "root protection blocked a destructive child request");
}

struct MountPointData final {
    DWORD tag{};
    WORD dataLength{};
    WORD reserved{};
    WORD substituteOffset{};
    WORD substituteLength{};
    WORD printOffset{};
    WORD printLength{};
    wchar_t pathBuffer[1]{};
};

void createJunction(
    const std::filesystem::path& junction,
    const std::filesystem::path& target)
{
    require(std::filesystem::create_directory(junction),
            "junction placeholder could not be created");
    WindowsDetail::UniqueHandle handle{::CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(handle), "junction placeholder could not be opened");

    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring printName = target.native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                  printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointData, pathBuffer) + pathBytes;
    require(totalBytes <= static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) &&
                pathBytes + 8U <=
                    static_cast<std::size_t>((std::numeric_limits<WORD>::max)()),
            "junction payload exceeded native bounds");

    std::vector<std::uint64_t> storage(
        (totalBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
    auto* const data = reinterpret_cast<MountPointData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(pathBytes + 8U);
    data->substituteLength = static_cast<WORD>(substituteBytes);
    data->printOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    data->printLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    data->pathBuffer[substitute.size()] = L'\0';
    std::memcpy(
        reinterpret_cast<std::byte*>(data->pathBuffer) + data->printOffset,
        printName.data(), printBytes);
    data->pathBuffer[(data->printOffset / sizeof(wchar_t)) + printName.size()] = L'\0';

    DWORD returned{};
    require(::DeviceIoControl(
                handle.get(), FSCTL_SET_REPARSE_POINT, data,
                static_cast<DWORD>(totalBytes), nullptr, 0U, &returned, nullptr) != FALSE,
            "junction reparse metadata could not be installed");
}

class CaseSensitiveDirectoryGuard final {
public:
    explicit CaseSensitiveDirectoryGuard(const std::filesystem::path& path)
        : handle_{::CreateFileW(
              path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
              OPEN_EXISTING,
              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)}
    {
        require(static_cast<bool>(handle_),
                "case-sensitive test directory could not be opened");
        FILE_CASE_SENSITIVE_INFO information{};
        information.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
        require(::SetFileInformationByHandle(
                    handle_.get(), FileCaseSensitiveInfo, &information,
                    sizeof(information)) != FALSE,
                "test volume could not enable per-directory case sensitivity");
        enabled_ = true;
    }

    ~CaseSensitiveDirectoryGuard() noexcept
    {
        if (enabled_) {
            FILE_CASE_SENSITIVE_INFO information{};
            static_cast<void>(::SetFileInformationByHandle(
                handle_.get(), FileCaseSensitiveInfo, &information,
                sizeof(information)));
        }
    }

    CaseSensitiveDirectoryGuard(const CaseSensitiveDirectoryGuard&) = delete;
    CaseSensitiveDirectoryGuard& operator=(const CaseSensitiveDirectoryGuard&) = delete;

private:
    WindowsDetail::UniqueHandle handle_;
    bool enabled_{};
};

void rejectsReparseEscapesCaseSensitiveAndOverlappingRoots()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    const auto junction = fixture.tree.root() / L"redirect";
    createJunction(junction, fixture.tree.outside());
    requireError(
        fixture.authority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                pathText(junction / L"escaped.txt"), fixture.root,
                Domain::FileAccess::Read, false},
            context()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "authorize followed a junction outside the trusted root");
    require(::RemoveDirectoryW(junction.c_str()) != FALSE,
            "junction fixture could not be removed safely");

    {
        CaseSensitiveDirectoryGuard guard{fixture.tree.caseSensitive()};
        Infrastructure::WindowsWorkspaceAuthority caseSensitiveAuthority{{
            policy({pathText(fixture.tree.caseSensitive())})}};
        requireError(
            caseSensitiveAuthority.authorityFor(projectId(), context()),
            Domain::ErrorCodes::PathOutsideAuthority,
            "a case-sensitive workspace root was admitted");
    }

    Infrastructure::WindowsWorkspaceAuthority overlapping{{policy({
        fixture.root, pathText(fixture.tree.root() / L"nested")})}};
    requireError(
        overlapping.authorityFor(projectId(), context()),
        Domain::ErrorCodes::InvalidRequest,
        "overlapping workspace roots were admitted");
}

void honorsCancellationAndDeadlineOnEveryCall()
{
    Fixture fixture;
    const auto authority = take(fixture.authority.authorityFor(projectId(), context()));
    const auto request = Domain::PathAuthorizationRequest{
        pathText(fixture.tree.root() / L"nested" / L"file.txt"),
        fixture.root, Domain::FileAccess::Read, false};
    std::stop_source stop;
    stop.request_stop();

    requireError(
        fixture.authority.authorityFor(projectId(), context(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "authorityFor ignored cancellation");
    requireError(
        fixture.authority.narrow(
            authority, {fixture.root}, {Domain::FileAccess::Read}, false, 8U,
            context(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "narrow ignored cancellation");
    requireError(
        fixture.authority.authorize(authority, request, context(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "authorize ignored cancellation");
    requireError(
        fixture.authority.authorityFor(projectId(), context({}, true)),
        Domain::ErrorCodes::DeadlineExceeded,
        "authorityFor ignored its deadline");
    requireError(
        fixture.authority.narrow(
            authority, {fixture.root}, {Domain::FileAccess::Read}, false, 8U,
            context({}, true)),
        Domain::ErrorCodes::DeadlineExceeded,
        "narrow ignored its deadline");
    requireError(
        fixture.authority.authorize(authority, request, context({}, true)),
        Domain::ErrorCodes::DeadlineExceeded,
        "authorize ignored its deadline");
}

} // namespace

int main()
{
    try {
        issuesCanonicalAuthorityAndPaths();
        std::cout << "PASS workspace_authority.canonical_success\n";
        rejectsMissingProjectAndMismatchedCapabilities();
        std::cout << "PASS workspace_authority.identity_scope_generation\n";
        narrowsOnlyWithinTheConfiguredBaseline();
        std::cout << "PASS workspace_authority.narrowing\n";
        rejectsOutsideTraversalAndWindowsNamespaceForms();
        std::cout << "PASS workspace_authority.hostile_paths\n";
        protectsTheAuthorityRootFromDestructiveRequests();
        std::cout << "PASS workspace_authority.root_protection\n";
        rejectsReparseEscapesCaseSensitiveAndOverlappingRoots();
        std::cout << "PASS workspace_authority.reparse_case_overlap\n";
        honorsCancellationAndDeadlineOnEveryCall();
        std::cout << "PASS workspace_authority.context\n";
        std::cout << "SUMMARY passed=7 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
