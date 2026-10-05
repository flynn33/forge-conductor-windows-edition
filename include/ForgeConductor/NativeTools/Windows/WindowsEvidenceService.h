#pragma once
#include "ForgeConductor/Contracts/IEvidenceService.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include <memory>

namespace ForgeConductor::NativeTools::Windows {
class WindowsEvidenceService final : public Contracts::IEvidenceService {
public:
    static constexpr std::size_t MaximumPaths = 64U;
    static constexpr std::size_t MaximumPathBytes = 32U * 1024U;
    static constexpr std::uint64_t MaximumFileBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t MaximumCaptureBytes = 128ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t MaximumLogBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t MaximumPageBytes = 256U * 1024U;
    WindowsEvidenceService(Contracts::IWorkspaceAuthority& authority,
        Contracts::IAtomicFileStore& files, Contracts::IHasher& hasher,
        Contracts::IClock& clock, Contracts::IUuidGenerator& uuids,
        Contracts::EvidenceStorageResolver storage, Domain::PathText dataRoot);
    ~WindowsEvidenceService() override;
    [[nodiscard]] Domain::Result<std::string> digest(
        const std::vector<Domain::PathText>& paths, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::string> readLog(
        const Contracts::WorkspaceAuthority& authority, std::size_t offset, std::size_t limit,
        bool verify, const Domain::OperationContext& context) noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
} // namespace ForgeConductor::NativeTools::Windows
