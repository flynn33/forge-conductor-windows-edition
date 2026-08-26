#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"

#include "Detail/AtomicReplaceEngine.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsAtomicFileStore::Impl final {
public:
    Detail::AtomicReplaceEngine engine;
};

WindowsAtomicFileStore::WindowsAtomicFileStore()
    : implementation_{std::make_unique<Impl>()}
{
}

WindowsAtomicFileStore::~WindowsAtomicFileStore() = default;

Domain::Result<std::vector<std::byte>> WindowsAtomicFileStore::read(
    const Contracts::AuthorizedPath& path,
    const std::size_t maximumBytes,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->engine.read(path, maximumBytes, context);
}

Domain::Result<void> WindowsAtomicFileStore::replace(
    const Contracts::AuthorizedPath& path,
    const std::span<const std::byte> content,
    const bool retainBackup,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->engine.replace(
        path,
        content,
        retainBackup,
        context);
}

} // namespace ForgeConductor::Infrastructure::Windows
