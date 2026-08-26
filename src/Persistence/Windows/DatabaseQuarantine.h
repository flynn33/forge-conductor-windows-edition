#pragma once

#include "Detail/DatabaseNamespaceLease.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/FileSystemModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <memory>
#include <cstdint>
#include <string>

namespace ForgeConductor::Persistence::Windows {

struct DatabaseQuarantineReport final {
    Domain::PathText cohortMainPath;
    Domain::PathText manifestPath;
    std::string evidenceId;
    std::size_t preservedFileCount{};
};

class IDatabaseQuarantineObserver {
public:
    virtual ~IDatabaseQuarantineObserver() noexcept = default;

    virtual void onSourceCohortCaptured() noexcept = 0;
    virtual void onEvidenceCohortCopied() noexcept = 0;
    virtual void onManifestCommitted() noexcept = 0;
};

class DatabaseQuarantine final {
public:
    static constexpr std::uint64_t MaximumFileBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t MaximumCohortBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;

    [[nodiscard]] static Domain::Result<DatabaseQuarantineReport> preserve(
        const std::shared_ptr<Detail::DatabaseNamespaceLease>& sourceNamespace,
        const Domain::Error& reason,
        const Domain::OperationContext& context,
        IDatabaseQuarantineObserver* observer = nullptr) noexcept;
};

} // namespace ForgeConductor::Persistence::Windows
