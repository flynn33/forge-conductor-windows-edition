#pragma once

#include "ForgeConductor/Contracts/IConfigurationStore.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsConfigurationStore final : public Contracts::IConfigurationStore {
public:
    static constexpr std::size_t MaximumDocumentBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonDepth = 32U;
    static constexpr std::uint32_t SchemaVersion = 1U;

    WindowsConfigurationStore(Contracts::IAtomicFileStore& atomicFileStore,
                              Contracts::AuthorizedPath readPath,
                              Contracts::AuthorizedPath writePath,
                              Contracts::AuthorizedPath createPath,
                              Contracts::AuthorizedPath backupReadPath);
    ~WindowsConfigurationStore() override;

    WindowsConfigurationStore(const WindowsConfigurationStore&) = delete;
    WindowsConfigurationStore& operator=(const WindowsConfigurationStore&) = delete;

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    load(const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    update(const Domain::AppConfigPatch& patch,
           const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    reload(const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
