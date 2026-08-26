#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"

#include <cstddef>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsAtomicFileStore final : public Contracts::IAtomicFileStore {
public:
    static constexpr std::size_t MaximumContentBytes = 32U * 1024U * 1024U;

    WindowsAtomicFileStore();
    ~WindowsAtomicFileStore() override;

    WindowsAtomicFileStore(const WindowsAtomicFileStore&) = delete;
    WindowsAtomicFileStore& operator=(const WindowsAtomicFileStore&) = delete;

    [[nodiscard]] Domain::Result<std::vector<std::byte>> read(
        const Contracts::AuthorizedPath& path,
        std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> replace(
        const Contracts::AuthorizedPath& path,
        std::span<const std::byte> content,
        bool retainBackup,
        const Domain::OperationContext& context) noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
