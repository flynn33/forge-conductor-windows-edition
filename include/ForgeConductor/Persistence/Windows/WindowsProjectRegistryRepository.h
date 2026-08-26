#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ForgeConductor::Persistence::Windows {

struct WindowsProjectRegistryStoragePaths final {
    Contracts::AuthorizedPath readPath;
    Contracts::AuthorizedPath writePath;
    Contracts::AuthorizedPath createPath;
    Contracts::AuthorizedPath backupReadPath;
};

class WindowsProjectRegistryRepository final
    : public Contracts::IProjectRegistryRepository {
public:
    static constexpr std::uint32_t SchemaVersion = 1U;
    static constexpr std::size_t MaximumDocumentBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumProjectCount = 1'024U;
    static constexpr std::size_t MaximumAliasCount = 32U;
    static constexpr std::size_t MaximumJsonDepth = 32U;
    static constexpr std::size_t MaximumGitConfigBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumGitRemoteCount = 256U;
    static constexpr std::size_t MaximumRepositoryIdentityBytes = 4U * 1024U;

    inline static constexpr std::wstring_view RegistryRelativePath{
        L"projects\\registry.json"};
    inline static constexpr std::wstring_view RegistryBackupRelativePath{
        L"projects\\registry.json.bak"};
    inline static constexpr std::wstring_view RegistryLockRelativePath{
        L"projects\\.registry.lock"};

    WindowsProjectRegistryRepository(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
        WindowsProjectRegistryStoragePaths storagePaths,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IClock> clock,
        Domain::ProjectMemoryLimits limits);

    ~WindowsProjectRegistryRepository() noexcept override;

    WindowsProjectRegistryRepository(const WindowsProjectRegistryRepository&) = delete;
    WindowsProjectRegistryRepository& operator=(
        const WindowsProjectRegistryRepository&) = delete;
    WindowsProjectRegistryRepository(WindowsProjectRegistryRepository&&) = delete;
    WindowsProjectRegistryRepository& operator=(WindowsProjectRegistryRepository&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId& projectId,
        const Domain::PathText& alias,
        const Domain::OperationContext& context) noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
