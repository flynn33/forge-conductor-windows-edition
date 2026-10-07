#pragma once

#include "ForgeConductor/Contracts/IArtifactDocumentService.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"

#include <cstddef>

namespace ForgeConductor::NativeTools::Windows {

class WindowsArtifactDocumentService final : public Contracts::IArtifactDocumentService {
public:
    static constexpr std::size_t MaximumInputBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumOutputBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t MaximumTextBytes = 64U * 1024U;
    static constexpr std::size_t MaximumParagraphs = 4096U;
    static constexpr std::size_t MaximumSheets = 32U;
    static constexpr std::size_t MaximumRowsPerSheet = 10'000U;
    static constexpr std::size_t MaximumColumns = 256U;
    static constexpr std::size_t MaximumCells = 100'000U;
    static constexpr std::size_t MaximumSlides = 128U;
    static constexpr std::size_t MaximumSlideParagraphs = 128U;

    WindowsArtifactDocumentService(Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IAtomicFileStore& atomicFileStore) noexcept;

    [[nodiscard]] Domain::Result<std::string> execute(
        std::string_view name, std::string_view arguments,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;

private:
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    Contracts::IAtomicFileStore& atomicFileStore_;
};

} // namespace ForgeConductor::NativeTools::Windows
