#pragma once
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"
#include <functional>
#include <memory>

namespace ForgeConductor::Infrastructure::Windows {
enum class ManagedReceiptPurpose { ReadOnlyReviewer, IndependentWorker };
struct ReviewerRunStoragePaths final {
    Contracts::AuthorizedPath read;
    Contracts::AuthorizedPath write;
    Contracts::AuthorizedPath create;
};
using ReviewerRunStorageResolver = std::function<Domain::Result<ReviewerRunStoragePaths>(
    const Domain::SessionId&, const Domain::OperationContext&)>;
class WindowsReviewerRunStore final : public Contracts::IManagedRunStore {
public:
    static constexpr std::size_t MaximumRecordBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t MaximumRetainedRecords = 16U;
    // The owner creates the private directory before construction; atomic writes
    // may create a receipt leaf but cannot grant or create its parent directories.
    WindowsReviewerRunStore(Contracts::IAtomicFileStore&, Contracts::IHasher&, Contracts::IClock&,
        Contracts::AuthorizedPath storageDirectoryRead, ReviewerRunStorageResolver,
        ManagedReceiptPurpose purpose = ManagedReceiptPurpose::ReadOnlyReviewer);
    ~WindowsReviewerRunStore() override;
    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId&, const Domain::OperationContext&) noexcept override;
    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord&, const Domain::OperationContext&) noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};
} // namespace ForgeConductor::Infrastructure::Windows
