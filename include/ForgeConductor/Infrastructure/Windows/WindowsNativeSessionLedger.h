#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

// Owns the bounded, app-data JSON ledger used by the native logical-session
// adapter. Injected services and capabilities must outlive this instance.
class WindowsNativeSessionLedger final : public Contracts::INativeSessionLedger {
public:
    static constexpr std::size_t MaximumDocumentBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t MaximumJsonDepth = 8U;
    static constexpr std::uint32_t SchemaVersion =
        Domain::NativeSessionLedgerSchemaVersion;

    WindowsNativeSessionLedger(
        Contracts::IAtomicFileStore& atomicFileStore,
        Contracts::IHasher& hasher,
        Contracts::AuthorizedPath readPath,
        Contracts::AuthorizedPath writePath,
        Contracts::AuthorizedPath createPath,
        Contracts::AuthorizedPath backupReadPath);
    ~WindowsNativeSessionLedger() noexcept override;

    WindowsNativeSessionLedger(const WindowsNativeSessionLedger&) = delete;
    WindowsNativeSessionLedger& operator=(const WindowsNativeSessionLedger&) = delete;
    WindowsNativeSessionLedger(WindowsNativeSessionLedger&&) = delete;
    WindowsNativeSessionLedger& operator=(WindowsNativeSessionLedger&&) = delete;

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> load(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::NativeSessionLedger> commit(
        const Domain::NativeSessionLedger& ledger,
        std::uint64_t expectedRevision,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
