#include "TestSupport.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsProjectWorkspaceAuthority;
using Infrastructure::Windows::Detail::UniqueHandle;
using Infrastructure::Windows::Detail::strictUtf16ToUtf8;
using namespace std::chrono_literals;

static_assert(WindowsProjectWorkspaceAuthority::MaximumProjects == 1'024U);

[[nodiscard]] Domain::ProjectId projectId(const std::uint64_t value = 1U)
{
    std::array<char, 37U> text{};
    const int written = std::snprintf(
        text.data(), text.size(), "10000000-0000-4000-8000-%012llx",
        static_cast<unsigned long long>(value));
    require(written == 36, "test project UUID formatting failed");
    return parse<Domain::ProjectId>(text.data());
}

[[nodiscard]] Domain::Uuid authorityUuid(const std::uint64_t value)
{
    std::array<char, 37U> text{};
    const int written = std::snprintf(
        text.data(), text.size(), "20000000-0000-4000-8000-%012llx",
        static_cast<unsigned long long>(value));
    require(written == 36, "test authority UUID formatting failed");
    return parse<Domain::Uuid>(text.data());
}

[[nodiscard]] Domain::ClientId serveClient()
{
    return parse<Domain::ClientId>("p14-dynamic-workspace-client");
}

[[nodiscard]] Domain::OperationContext activeContext(
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("30000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + 5min,
        cancellation,
        parse<Domain::CorrelationId>("p14-dynamic-workspace-authority")};
}

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path& path)
{
    return take(Domain::PathText::create(
        take(strictUtf16ToUtf8(path.native()))));
}

class ScopedTestTree final {
public:
    ScopedTestTree()
    {
        std::wstring temporary(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(),
                "GetTempPathW failed for dynamic authority tests");
        temporary.resize(length);
        base_ = std::filesystem::path{temporary} /
            (L"ForgeConductor.P14.DynamicAuthority." +
             std::to_wstring(::GetCurrentProcessId()) + L"." +
             std::to_wstring(::GetTickCount64()));
        first_ = base_ / L"first";
        second_ = base_ / L"second";
        outside_ = base_ / L"outside";
        require(std::filesystem::create_directories(first_ / L"child") &&
                    std::filesystem::create_directories(second_) &&
                    std::filesystem::create_directories(outside_),
                "dynamic authority test directories could not be created");
    }

    ~ScopedTestTree() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(base_, ignored));
    }

    ScopedTestTree(const ScopedTestTree&) = delete;
    ScopedTestTree& operator=(const ScopedTestTree&) = delete;

    [[nodiscard]] const std::filesystem::path& base() const noexcept { return base_; }
    [[nodiscard]] const std::filesystem::path& first() const noexcept { return first_; }
    [[nodiscard]] const std::filesystem::path& second() const noexcept { return second_; }
    [[nodiscard]] const std::filesystem::path& outside() const noexcept { return outside_; }

private:
    std::filesystem::path base_;
    std::filesystem::path first_;
    std::filesystem::path second_;
    std::filesystem::path outside_;
};

class RegistryFake final : public Contracts::IProjectRegistryRepository {
public:
    void seed(
        const Domain::ProjectId& id,
        std::vector<Domain::PathText> aliases)
    {
        std::lock_guard lock{mutex_};
        descriptors_.insert_or_assign(
            id, Domain::ProjectMemoryDescriptor{id, "Project", std::nullopt,
                                                 std::move(aliases)});
    }

    void remove(const Domain::ProjectId& id)
    {
        std::lock_guard lock{mutex_};
        descriptors_.erase(id);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ProjectInitialization>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The dynamic authority registry fake does not initialize projects."));
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& id,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto match = descriptors_.find(id);
            if (match == descriptors_.end()) {
                return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The dynamic authority project was not found."));
            }
            return Domain::Result<Domain::ProjectMemoryDescriptor>::success(
                match->second);
        } catch (...) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dynamic authority registry fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            std::vector<Domain::ProjectMemoryDescriptor> descriptors;
            descriptors.reserve((std::min)(maximumCount, descriptors_.size()));
            for (const auto& [id, descriptor] : descriptors_) {
                static_cast<void>(id);
                if (descriptors.size() >= maximumCount) {
                    break;
                }
                descriptors.push_back(descriptor);
            }
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::success(
                std::move(descriptors));
        } catch (...) {
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dynamic authority registry fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId& id,
        const Domain::PathText& alias,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto match = descriptors_.find(id);
            if (match == descriptors_.end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::ProjectNotFound,
                    "The dynamic authority project was not found."));
            }
            auto& aliases = match->second.aliases;
            aliases.erase(std::remove(aliases.begin(), aliases.end(), alias), aliases.end());
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dynamic authority registry fake failed."));
        }
    }

private:
    std::mutex mutex_;
    std::map<Domain::ProjectId, Domain::ProjectMemoryDescriptor> descriptors_;
};

class CountingUuidGenerator final : public Contracts::IUuidGenerator {
public:
    explicit CountingUuidGenerator(const std::uint64_t first = 1U) noexcept
        : first_{first}, next_{first}
    {
    }

    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto value = next_.fetch_add(1U, std::memory_order_relaxed);
            return Domain::Result<Domain::Uuid>::success(authorityUuid(value));
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dynamic authority UUID fake failed."));
        }
    }

    [[nodiscard]] std::uint64_t consumed() const noexcept
    {
        return next_.load(std::memory_order_relaxed) - first_;
    }

private:
    const std::uint64_t first_;
    std::atomic_uint64_t next_;
};

class RacingUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            const auto sequence = ++calls_;
            ready_.notify_all();
            if (!ready_.wait_for(lock, 30s, [&] { return calls_ >= 2U; })) {
                return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The concurrent UUID test did not reach its publication race."));
            }
            lock.unlock();
            return Domain::Result<Domain::Uuid>::success(authorityUuid(sequence));
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The concurrent UUID fake failed."));
        }
    }

    [[nodiscard]] std::size_t consumed() const noexcept
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::size_t calls_{};
};

class CapabilityIssuer final : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Contracts::WorkspaceAuthority issue(
        Domain::AuthorityId authorityId,
        Domain::ProjectId project,
        Domain::ClientId caller,
        std::vector<Domain::PathText> roots,
        const std::uint64_t generation)
    {
        return take(issueAuthority(
            std::move(authorityId), std::move(project), std::move(caller),
            std::move(roots), Domain::FileAccess::Write,
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
            "The test capability issuer only exposes its factory."));
    }
};

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess access)
{
    return std::find(values.begin(), values.end(), access) != values.end();
}

void discoversProjectsAndRefreshesAliases()
{
    ScopedTestTree tree;
    RegistryFake registry;
    CountingUuidGenerator uuids;
    WindowsProjectWorkspaceAuthority authority{
        registry, uuids, serveClient(), false};

    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::ProjectNotFound,
        "an unknown project was issued a workspace authority");
    require(uuids.consumed() == 0U,
            "an unknown project consumed an authority identifier");

    const auto firstRoot = pathText(tree.first());
    const auto secondRoot = pathText(tree.second());
    registry.seed(projectId(), {firstRoot});
    const auto first = take(authority.authorityFor(projectId(), activeContext()));
    require(first.intent() == Domain::FileAccess::Write &&
                first.trustedRoots() == std::vector<Domain::PathText>{firstRoot} &&
                first.callerId() == serveClient() && !first.shellEnabled() &&
                !containsAccess(first.grants(), Domain::FileAccess::Execute) &&
                containsAccess(first.denials(), Domain::FileAccess::Execute),
            "the initial registry-backed policy was not issued exactly");

    registry.seed(projectId(), {firstRoot, secondRoot});
    const auto attached = take(authority.authorityFor(projectId(), activeContext()));
    require(attached.authorityId() == first.authorityId() &&
                attached.trustedRoots() ==
                    std::vector<Domain::PathText>{firstRoot, secondRoot},
            "an attached alias did not refresh under the stable authority ID");
    const auto narrowed = take(authority.narrow(
        attached, {secondRoot},
        {Domain::FileAccess::Read, Domain::FileAccess::Write}, false, 2U,
        activeContext()));
    require(narrowed.authorityId() == first.authorityId() &&
                narrowed.generation() == 2U &&
                narrowed.trustedRoots() ==
                    std::vector<Domain::PathText>{secondRoot},
            "the refreshed descriptor was not used while narrowing authority");
    const auto secondPath = pathText(tree.second() / L"new.txt");
    const auto authorized = take(authority.authorize(
        attached,
        Domain::PathAuthorizationRequest{
            secondPath, secondRoot, Domain::FileAccess::Create, false},
        activeContext()));
    require(authorized.authorityRoot() == secondRoot,
            "the refreshed alias was not usable immediately");

    registry.seed(projectId(), {secondRoot});
    requireError(
        authority.authorize(
            attached,
            Domain::PathAuthorizationRequest{
                pathText(tree.first() / L"child"), firstRoot,
                Domain::FileAccess::Read, false},
            activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "a detached alias remained usable through a previously issued capability");
    const auto detached = take(authority.authorityFor(projectId(), activeContext()));
    require(detached.authorityId() == first.authorityId() &&
                detached.trustedRoots() == std::vector<Domain::PathText>{secondRoot} &&
                uuids.consumed() == 1U,
            "alias detachment changed or regenerated the project authority ID");
    const auto retained = take(authority.authorize(
        narrowed,
        Domain::PathAuthorizationRequest{
            secondPath, secondRoot, Domain::FileAccess::Read, false},
        activeContext()));
    require(retained.authorityRoot() == secondRoot,
            "a narrowed capability stopped tracking its retained registry alias");
}

void publishesOneStableIdDuringConcurrentFirstIssuance()
{
    ScopedTestTree tree;
    RegistryFake registry;
    registry.seed(projectId(), {pathText(tree.first())});
    RacingUuidGenerator uuids;
    WindowsProjectWorkspaceAuthority authority{
        registry, uuids, serveClient(), false};

    std::array<std::string, 2U> ids;
    std::array<std::string, 2U> errors;
    std::jthread first{[&] {
        auto issued = authority.authorityFor(projectId(), activeContext());
        if (issued) {
            ids[0] = issued.value().authorityId().value();
        } else {
            errors[0] = issued.error().code;
        }
    }};
    std::jthread second{[&] {
        auto issued = authority.authorityFor(projectId(), activeContext());
        if (issued) {
            ids[1] = issued.value().authorityId().value();
        } else {
            errors[1] = issued.error().code;
        }
    }};
    first.join();
    second.join();

    require(errors[0].empty() && errors[1].empty(),
            "concurrent first issuance failed");
    require(!ids[0].empty() && ids[0] == ids[1],
            "concurrent first issuance published more than one authority ID");
    require(uuids.consumed() == 2U,
            "the concurrent test did not exercise double-checked publication");
}

void rejectsForeignStaleAndNoLongerRegisteredCapabilities()
{
    ScopedTestTree tree;
    RegistryFake registry;
    CountingUuidGenerator uuids;
    const auto root = pathText(tree.first());
    registry.seed(projectId(), {root});
    WindowsProjectWorkspaceAuthority authority{
        registry, uuids, serveClient(), false};
    const auto issued = take(authority.authorityFor(projectId(), activeContext()));
    const auto request = Domain::PathAuthorizationRequest{
        pathText(tree.first() / L"child"), root, Domain::FileAccess::Read, false};

    const auto foreignId = CapabilityIssuer::issue(
        Domain::AuthorityId{authorityUuid(999U)}, projectId(), serveClient(),
        {root}, 1U);
    requireError(
        authority.authorize(foreignId, request, activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "a foreign authority ID was accepted");
    const auto foreignCaller = CapabilityIssuer::issue(
        issued.authorityId(), projectId(),
        parse<Domain::ClientId>("foreign-client"), {root}, 1U);
    requireError(
        authority.authorize(foreignCaller, request, activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "a foreign caller binding was accepted");
    const auto stale = CapabilityIssuer::issue(
        issued.authorityId(), projectId(), serveClient(), {root}, 0U);
    requireError(
        authority.authorize(stale, request, activeContext()),
        Domain::ErrorCodes::Unauthorized,
        "a stale generation was accepted");

    registry.remove(projectId());
    requireError(
        authority.authorize(issued, request, activeContext()),
        Domain::ErrorCodes::ProjectNotFound,
        "a removed project retained workspace authority");
    registry.seed(projectId(), {root});
    const auto restored = take(authority.authorityFor(projectId(), activeContext()));
    require(restored.authorityId() == issued.authorityId() &&
                uuids.consumed() == 1U,
            "a re-registered project lost its process-lifetime authority binding");
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
    UniqueHandle handle{::CreateFileW(
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

void rejectsOverlapCaseDuplicatesAndReparseRootsWithoutPublishing()
{
    ScopedTestTree tree;
    RegistryFake registry;
    CountingUuidGenerator uuids;
    WindowsProjectWorkspaceAuthority authority{
        registry, uuids, serveClient(), false};
    const auto root = pathText(tree.first());
    const auto child = pathText(tree.first() / L"child");

    registry.seed(projectId(), {});
    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "an empty registry alias set was admitted");
    registry.seed(
        projectId(),
        std::vector<Domain::PathText>(
            Infrastructure::Windows::WindowsWorkspaceAuthority::
                    MaximumTrustedRootsPerPolicy +
                1U,
            root));
    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "an over-bound registry alias set was admitted");
    require(uuids.consumed() == 0U,
            "structurally invalid descriptors consumed authority identifiers");

    registry.seed(projectId(), {root, child});
    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "overlapping registry aliases were admitted");

    std::string differentCase = root.value();
    std::transform(
        differentCase.begin(), differentCase.end(), differentCase.begin(),
        [](const unsigned char value) {
            if (value >= static_cast<unsigned char>('a') &&
                value <= static_cast<unsigned char>('z')) {
                return static_cast<char>(value - ('a' - 'A'));
            }
            return static_cast<char>(value);
        });
    registry.seed(projectId(), {root, take(Domain::PathText::create(differentCase))});
    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "case-only duplicate registry aliases were admitted");

    const auto junction = tree.base() / L"redirect";
    createJunction(junction, tree.outside());
    registry.seed(projectId(), {pathText(junction)});
    requireError(
        authority.authorityFor(projectId(), activeContext()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "a reparse-point registry alias was admitted");
    require(::RemoveDirectoryW(junction.c_str()) != FALSE,
            "junction fixture could not be removed safely");

    registry.seed(projectId(), {root});
    const auto recovered = take(authority.authorityFor(projectId(), activeContext()));
    require(recovered.trustedRoots() == std::vector<Domain::PathText>{root} &&
                uuids.consumed() == 4U,
            "invalid descriptors published a cached authority binding");
}

void enforcesContextAndProjectBound()
{
    ScopedTestTree tree;
    RegistryFake registry;
    CountingUuidGenerator uuids;
    const auto root = pathText(tree.first());
    registry.seed(projectId(), {root});
    WindowsProjectWorkspaceAuthority authority{
        registry, uuids, serveClient(), true};

    std::stop_source stop;
    stop.request_stop();
    requireError(
        authority.authorityFor(projectId(), activeContext(stop.get_token())),
        Domain::ErrorCodes::Cancelled,
        "authority issuance ignored cancellation");
    const auto now = std::chrono::steady_clock::now();
    const Domain::OperationContext expired{
        parse<Domain::OperationId>("30000000-0000-4000-8000-000000000002"),
        now - 1ms,
        {},
        parse<Domain::CorrelationId>("p14-expired-dynamic-authority")};
    requireError(
        authority.authorityFor(projectId(), expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "authority issuance ignored its deadline");
    require(uuids.consumed() == 0U,
            "cancelled or expired issuance consumed an authority identifier");

    for (std::size_t index = 1U;
         index <= WindowsProjectWorkspaceAuthority::MaximumProjects + 1U;
         ++index) {
        registry.seed(projectId(index), {root});
    }
    for (std::size_t index = 1U;
         index <= WindowsProjectWorkspaceAuthority::MaximumProjects;
         ++index) {
        const auto issued = take(authority.authorityFor(projectId(index), activeContext()));
        require(issued.shellEnabled() &&
                    containsAccess(issued.grants(), Domain::FileAccess::Execute) &&
                    !containsAccess(issued.denials(), Domain::FileAccess::Execute),
                "the shell-enabled project policy omitted execute authority");
    }
    requireError(
        authority.authorityFor(
            projectId(WindowsProjectWorkspaceAuthority::MaximumProjects + 1U),
            activeContext()),
        Domain::ErrorCodes::LimitExceeded,
        "the process-lifetime project authority bound was exceeded");
}

} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    TestRegistry tests;
    addTest(tests, "dynamic_authority.registry_refresh", discoversProjectsAndRefreshesAliases);
    addTest(tests, "dynamic_authority.concurrent_first_issue", publishesOneStableIdDuringConcurrentFirstIssuance);
    addTest(tests, "dynamic_authority.foreign_stale_removed", rejectsForeignStaleAndNoLongerRegisteredCapabilities);
    addTest(tests, "dynamic_authority.overlap_case_reparse", rejectsOverlapCaseDuplicatesAndReparseRootsWithoutPublishing);
    addTest(tests, "dynamic_authority.context_and_bound", enforcesContextAndProjectBound);

    std::size_t passed{};
    for (const auto& [name, run] : tests) {
        try {
            run();
            ++passed;
            std::printf("PASS %s\n", name.c_str());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
            return EXIT_FAILURE;
        }
    }
    std::printf("SUMMARY passed=%zu failed=0\n", passed);
    return EXIT_SUCCESS;
}
