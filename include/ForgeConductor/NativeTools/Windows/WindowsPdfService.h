#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/INativeToolServices.h"

namespace ForgeConductor::NativeTools::Windows {

class WindowsPdfService final : public Contracts::IPdfService {
public:
    explicit WindowsPdfService(
        Contracts::IAtomicFileStore& atomicFileStore) noexcept;

    WindowsPdfService(const WindowsPdfService&) = delete;
    WindowsPdfService& operator=(const WindowsPdfService&) = delete;
    WindowsPdfService(WindowsPdfService&&) = delete;
    WindowsPdfService& operator=(WindowsPdfService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::PdfWriteReceipt> write(
        std::string_view title,
        std::string_view body,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::PdfWriteReceipt> fromTextFile(
        std::string_view title,
        const Contracts::AuthorizedPath& source,
        const Contracts::AuthorizedPath& destination,
        const Domain::OperationContext& context) noexcept override;

private:
    Contracts::IAtomicFileStore& atomicFileStore_;
};

} // namespace ForgeConductor::NativeTools::Windows
