#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "../../../Infrastructure/Windows/Detail/UniqueHandle.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
class DatabaseQuarantine;
}

namespace ForgeConductor::Persistence::Windows::Detail {

struct DatabaseNamespaceState;

enum class DatabaseLeafRole : unsigned char {
    Main,
    Wal,
    SharedMemory,
    Journal,
    MigrationLock,
};

enum class DatabaseLeafDisposition : unsigned char {
    OpenExisting,
    CreateNew,
    OpenOrCreate,
};

enum class DatabaseLeafAccess : unsigned char {
    Exists,
    Read,
    ReadWrite,
};

struct DatabaseFileIdentity final {
    std::uint64_t volumeSerialNumber{};
    std::array<std::byte, 16U> fileIdentifier{};

    bool operator==(const DatabaseFileIdentity&) const noexcept = default;
};

class DatabaseLeafLease final {
public:
    DatabaseLeafLease() noexcept = default;
    ~DatabaseLeafLease() noexcept;

    DatabaseLeafLease(const DatabaseLeafLease&) = delete;
    DatabaseLeafLease& operator=(const DatabaseLeafLease&) = delete;
    DatabaseLeafLease(DatabaseLeafLease&& other) noexcept;
    DatabaseLeafLease& operator=(DatabaseLeafLease&& other) noexcept;

    [[nodiscard]] DatabaseLeafRole role() const noexcept { return role_; }
    [[nodiscard]] HANDLE nativeHandle() const noexcept { return handle_.get(); }
    [[nodiscard]] const DatabaseFileIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] bool wasCreated() const noexcept { return wasCreated_; }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

private:
    DatabaseLeafLease(
        DatabaseLeafRole role,
        Infrastructure::Windows::Detail::UniqueHandle handle,
        DatabaseFileIdentity identity,
        bool wasCreated,
        std::shared_ptr<DatabaseNamespaceState> state) noexcept;

    void release() noexcept;

    friend class DatabaseNamespaceLease;

    DatabaseLeafRole role_{DatabaseLeafRole::Main};
    Infrastructure::Windows::Detail::UniqueHandle handle_;
    DatabaseFileIdentity identity_{};
    bool wasCreated_{};
    std::shared_ptr<DatabaseNamespaceState> state_;
};

class DatabaseMigrationLock final {
public:
    DatabaseMigrationLock() noexcept = default;
    ~DatabaseMigrationLock() noexcept;

    DatabaseMigrationLock(const DatabaseMigrationLock&) = delete;
    DatabaseMigrationLock& operator=(const DatabaseMigrationLock&) = delete;
    DatabaseMigrationLock(DatabaseMigrationLock&& other) noexcept;
    DatabaseMigrationLock& operator=(DatabaseMigrationLock&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return locked_; }

private:
    DatabaseMigrationLock(
        DatabaseLeafLease pinnedLeaf,
        Infrastructure::Windows::Detail::UniqueHandle lockHandle) noexcept;

    void release() noexcept;

    friend class DatabaseNamespaceLease;

    DatabaseLeafLease pinnedLeaf_;
    Infrastructure::Windows::Detail::UniqueHandle lockHandle_;
    bool locked_{};
};

class DatabaseNamespaceLease final {
public:
    static constexpr std::size_t MaximumLeafNameCharacters = 180U;
    static constexpr std::size_t MaximumStaleStageLeasesPerCleanup = 32U;
    static constexpr std::size_t MaximumMatchingLeafNames = 32U;
    static constexpr std::size_t MaximumDirectoryEntriesInspected = 4'096U;

    [[nodiscard]] static Domain::Result<std::shared_ptr<DatabaseNamespaceLease>> create(
        std::wstring_view canonicalDirectory,
        std::wstring_view mainBasename,
        std::wstring_view migrationLockBasename) noexcept;

    ~DatabaseNamespaceLease() noexcept = default;

    DatabaseNamespaceLease(const DatabaseNamespaceLease&) = delete;
    DatabaseNamespaceLease& operator=(const DatabaseNamespaceLease&) = delete;
    DatabaseNamespaceLease(DatabaseNamespaceLease&&) = delete;
    DatabaseNamespaceLease& operator=(DatabaseNamespaceLease&&) = delete;

    [[nodiscard]] const std::wstring& canonicalDirectory() const noexcept;
    [[nodiscard]] const std::wstring& canonicalMainDatabasePath() const noexcept;
    [[nodiscard]] const std::wstring& leafName(DatabaseLeafRole role) const noexcept;
    [[nodiscard]] const std::wstring& canonicalPath(DatabaseLeafRole role) const noexcept;
    [[nodiscard]] const std::string& canonicalUtf8Path(DatabaseLeafRole role) const noexcept;

    [[nodiscard]] Domain::Result<DatabaseLeafRole> classifyCanonicalPath(
        std::wstring_view candidate) const noexcept;

    [[nodiscard]] Domain::Result<DatabaseLeafLease> openLeaf(
        DatabaseLeafRole role,
        DatabaseLeafDisposition disposition,
        ACCESS_MASK desiredAccess) const noexcept;

    [[nodiscard]] Domain::Result<DatabaseLeafLease> openLeafWithShareAccess(
        DatabaseLeafRole role,
        DatabaseLeafDisposition disposition,
        ACCESS_MASK desiredAccess,
        ULONG shareAccess) const noexcept;

    [[nodiscard]] Domain::Result<bool> accessLeaf(
        DatabaseLeafRole role,
        DatabaseLeafAccess access) const noexcept;

    [[nodiscard]] Domain::Result<void> deleteTransientLeaf(
        DatabaseLeafRole role) const noexcept;

    [[nodiscard]] Domain::Result<void> discardCreatedLeaf(
        DatabaseLeafRole role,
        const DatabaseFileIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] Domain::Result<void> deleteClosedLeaf(
        DatabaseLeafRole role,
        const DatabaseFileIdentity& expectedIdentity) const noexcept;

    [[nodiscard]] Domain::Result<DatabaseMigrationLock> acquireMigrationLock(
        const Domain::OperationContext& context) const noexcept;

    [[nodiscard]] Domain::Result<void> revalidate() const noexcept;

    [[nodiscard]] Domain::Result<void> revalidateRetainedLeaf(
        const DatabaseLeafLease& leaf) const noexcept;

    [[nodiscard]] Domain::Result<void> revalidateCohort() const noexcept;

    // Enumerates only direct, regular, non-reparse child names that match the
    // literal case-insensitive prefix and suffix. It never recurses or derives
    // backup naming policy, and fails if either the scan or result bound is hit.
    [[nodiscard]] Domain::Result<std::vector<std::wstring>> enumerateMatchingLeafNames(
        std::wstring_view requiredPrefix,
        std::wstring_view requiredSuffix,
        std::size_t maximumResults,
        const Domain::OperationContext& context) const noexcept;

    [[nodiscard]] Domain::Result<void> publishClosedMainTo(
        DatabaseNamespaceLease& destination,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<void> publishClosedMainToWithSourceLock(
        DatabaseNamespaceLease& destination,
        const DatabaseMigrationLock& sourceLock,
        const Domain::OperationContext& context) noexcept;

    // Publishes create-only and returns the still-open source handle after its
    // identity has transferred to the destination. The returned lease denies
    // write and delete sharing so verification can remain continuous.
    [[nodiscard]] Domain::Result<DatabaseLeafLease>
    publishClosedMainToWithSourceLockAndRetain(
        DatabaseNamespaceLease& destination,
        const DatabaseMigrationLock& sourceLock,
        const Domain::OperationContext& context) noexcept;

    // Consumes the caller-held exact stage lock after deleting the closed cohort.
    [[nodiscard]] Domain::Result<bool> cleanupClosedStageWithLock(
        DatabaseMigrationLock& stageLock,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<std::size_t> cleanupClosedStages(
        std::span<const std::shared_ptr<DatabaseNamespaceLease>> staleStages,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] std::size_t openVfsFileCount() const noexcept;

    void noteVfsFileOpened() noexcept;
    void noteVfsFileClosed() noexcept;

private:
    explicit DatabaseNamespaceLease(std::shared_ptr<DatabaseNamespaceState> state) noexcept;

    friend class ForgeConductor::Persistence::Windows::DatabaseQuarantine;

    [[nodiscard]] Domain::Result<std::vector<DatabaseLeafLease>>
    captureClosedCohortForQuarantine() const noexcept;

    [[nodiscard]] Domain::Result<void> revalidateExactQuarantineCohort(
        std::span<const DatabaseLeafLease* const> expectedLeaves) const noexcept;

    [[nodiscard]] Domain::Result<void> deleteRetainedQuarantineLeaf(
        DatabaseLeafLease& leaf) const noexcept;

    [[nodiscard]] Domain::Result<void> deleteLeaf(
        DatabaseLeafRole role,
        const DatabaseFileIdentity* expectedIdentity,
        bool missingIsSuccess) const noexcept;

    [[nodiscard]] Domain::Result<bool> hasTransientLeaves() const noexcept;

    [[nodiscard]] Domain::Result<void> publishClosedMainToImpl(
        DatabaseNamespaceLease& destination,
        const DatabaseMigrationLock* sourceLock,
        DatabaseLeafLease* retainedPublishedLeaf,
        const Domain::OperationContext& context) noexcept;

    std::shared_ptr<DatabaseNamespaceState> state_;
};

} // namespace ForgeConductor::Persistence::Windows::Detail
