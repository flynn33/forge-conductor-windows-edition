#include "DatabaseQuarantine.h"

#include "Infrastructure/Windows/Detail/SecureBuffer.h"
#include "Infrastructure/Windows/Detail/UniqueBCryptHandle.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Persistence::Windows {
namespace {

constexpr ULONG Sha256Bytes = 32U;
constexpr ULONG MaximumHashObjectBytes = 1024U * 1024U;
constexpr DWORD ReadChunkBytes = 64U * 1024U;
constexpr std::size_t MaximumManifestBytes = 64U * 1024U;

struct PreservedFile final {
    Detail::DatabaseLeafRole role{Detail::DatabaseLeafRole::Main};
    std::uint64_t bytes{};
    std::string sha256;
    Detail::DatabaseFileIdentity sourceIdentity{};
    Detail::DatabaseFileIdentity identity{};
    Detail::DatabaseLeafLease lease;
};

struct CapturedFile final {
    Detail::DatabaseLeafRole role{Detail::DatabaseLeafRole::Main};
    std::uint64_t bytes{};
    Detail::DatabaseLeafLease lease;
};

[[nodiscard]] Domain::Result<void> checkContext(
    const Domain::OperationContext& context) noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "Database quarantine was cancelled."));
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The database quarantine deadline expired."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<ULONG> bcryptProperty(
    const BCRYPT_ALG_HANDLE algorithm,
    const wchar_t* const name) noexcept
{
    ULONG value{};
    ULONG returned{};
    const NTSTATUS status = ::BCryptGetProperty(
        algorithm,
        name,
        reinterpret_cast<PUCHAR>(&value),
        static_cast<ULONG>(sizeof(value)),
        &returned,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        return Domain::Result<ULONG>::failure(
            Infrastructure::Windows::Detail::makeNtStatusError(
                "read a quarantine SHA-256 property", status));
    }
    if (returned != sizeof(value)) {
        return Domain::Result<ULONG>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "BCrypt returned an invalid quarantine SHA-256 property size."));
    }
    return Domain::Result<ULONG>::success(value);
}

[[nodiscard]] std::string hexBytes(const std::span<const std::byte> bytes)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const auto value = static_cast<unsigned char>(bytes[index]);
        result[index * 2U] = Hex[(value >> 4U) & 0x0fU];
        result[index * 2U + 1U] = Hex[value & 0x0fU];
    }
    return result;
}

[[nodiscard]] Domain::Result<std::pair<std::uint64_t, std::string>> hashFile(
    const HANDLE file,
    const Domain::OperationContext& context) noexcept
{
    try {
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Infrastructure::Windows::Detail::makeWin32Error(
                    "read a quarantined database file size", ::GetLastError()));
        }
        if (static_cast<std::uint64_t>(size.QuadPart) >
            DatabaseQuarantine::MaximumFileBytes) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "A database quarantine source exceeds the per-file evidence limit."));
        }
        LARGE_INTEGER zero{};
        if (::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Infrastructure::Windows::Detail::makeWin32Error(
                    "rewind a quarantined database file", ::GetLastError()));
        }

        BCRYPT_ALG_HANDLE rawAlgorithm{};
        NTSTATUS status = ::BCryptOpenAlgorithmProvider(
            &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Infrastructure::Windows::Detail::makeNtStatusError(
                    "open the quarantine SHA-256 provider", status));
        }
        Infrastructure::Windows::Detail::UniqueBCryptAlgorithmHandle algorithm{
            rawAlgorithm};
        auto objectLength = bcryptProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH);
        if (!objectLength) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                std::move(objectLength).error());
        }
        auto hashLength = bcryptProperty(algorithm.get(), BCRYPT_HASH_LENGTH);
        if (!hashLength) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                std::move(hashLength).error());
        }
        if (objectLength.value() == 0U ||
            objectLength.value() > MaximumHashObjectBytes ||
            hashLength.value() != Sha256Bytes) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "BCrypt exposed unsafe quarantine SHA-256 dimensions."));
        }
        Infrastructure::Windows::Detail::SecureBuffer object{objectLength.value()};
        BCRYPT_HASH_HANDLE rawHash{};
        status = ::BCryptCreateHash(
            algorithm.get(),
            &rawHash,
            reinterpret_cast<PUCHAR>(object.data()),
            objectLength.value(),
            nullptr,
            0U,
            0U);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Infrastructure::Windows::Detail::makeNtStatusError(
                    "create the quarantine SHA-256 state", status));
        }
        Infrastructure::Windows::Detail::UniqueBCryptHashHandle hash{rawHash};

        std::array<std::byte, ReadChunkBytes> buffer{};
        std::uint64_t total{};
        for (;;) {
            auto validContext = checkContext(context);
            if (!validContext) {
                return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                    std::move(validContext).error());
            }
            DWORD read{};
            if (::ReadFile(file, buffer.data(), ReadChunkBytes, &read, nullptr) == FALSE) {
                return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                    Infrastructure::Windows::Detail::makeWin32Error(
                        "read a quarantined database file", ::GetLastError()));
            }
            if (read == 0U) {
                break;
            }
            status = ::BCryptHashData(
                hash.get(),
                reinterpret_cast<PUCHAR>(buffer.data()),
                read,
                0U);
            if (!BCRYPT_SUCCESS(status)) {
                return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                    Infrastructure::Windows::Detail::makeNtStatusError(
                        "hash a quarantined database file", status));
            }
            total += read;
        }
        if (total != static_cast<std::uint64_t>(size.QuadPart)) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "A quarantined database file changed while it was hashed.",
                    true));
        }
        std::array<std::byte, Sha256Bytes> digest{};
        status = ::BCryptFinishHash(
            hash.get(),
            reinterpret_cast<PUCHAR>(digest.data()),
            Sha256Bytes,
            0U);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
                Infrastructure::Windows::Detail::makeNtStatusError(
                    "finish a quarantine SHA-256 digest", status));
        }
        return Domain::Result<std::pair<std::uint64_t, std::string>>::success(
            {total, hexBytes(digest)});
    } catch (...) {
        return Domain::Result<std::pair<std::uint64_t, std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A quarantined database file could not be hashed with bounded memory."));
    }
}

[[nodiscard]] Domain::Result<void> copyFile(
    const HANDLE source,
    const HANDLE destination,
    const std::uint64_t expectedBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (expectedBytes > DatabaseQuarantine::MaximumFileBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A database quarantine source exceeds the per-file evidence limit."));
        }
        LARGE_INTEGER zero{};
        if (::SetFilePointerEx(source, zero, nullptr, FILE_BEGIN) == FALSE ||
            ::SetFilePointerEx(destination, zero, nullptr, FILE_BEGIN) == FALSE) {
            return Domain::Result<void>::failure(
                Infrastructure::Windows::Detail::makeWin32Error(
                    "rewind a database quarantine copy", ::GetLastError()));
        }

        std::array<std::byte, ReadChunkBytes> buffer{};
        std::uint64_t total{};
        for (;;) {
            auto validContext = checkContext(context);
            if (!validContext) {
                return validContext;
            }
            DWORD read{};
            if (::ReadFile(source, buffer.data(), ReadChunkBytes, &read, nullptr) == FALSE) {
                return Domain::Result<void>::failure(
                    Infrastructure::Windows::Detail::makeWin32Error(
                        "read a database quarantine source", ::GetLastError()));
            }
            if (read == 0U) {
                break;
            }
            if (total > expectedBytes ||
                static_cast<std::uint64_t>(read) > expectedBytes - total) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "A database quarantine source grew while it was copied.",
                    true));
            }

            DWORD offset{};
            while (offset < read) {
                DWORD written{};
                if (::WriteFile(
                        destination,
                        buffer.data() + offset,
                        read - offset,
                        &written,
                        nullptr) == FALSE) {
                    return Domain::Result<void>::failure(
                        Infrastructure::Windows::Detail::makeWin32Error(
                            "write a database quarantine evidence file", ::GetLastError()));
                }
                if (written == 0U) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::StorageFull,
                        "A database quarantine evidence write made no progress."));
                }
                offset += written;
            }
            total += read;
        }
        if (total != expectedBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "A database quarantine source changed size while it was copied.",
                true));
        }
        if (::FlushFileBuffers(destination) == FALSE) {
            return Domain::Result<void>::failure(
                Infrastructure::Windows::Detail::makeWin32Error(
                    "flush a database quarantine evidence file", ::GetLastError()));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A database quarantine file could not be copied with bounded memory."));
    }
}

void appendCleanupFailure(
    Domain::Error& error,
    const Domain::Error& cleanupError) noexcept
{
    try {
        error.message += " Exact cleanup of an uncommitted quarantine artifact also failed: ";
        error.message += cleanupError.message;
    } catch (...) {
        // Preserve the original typed failure under memory pressure.
    }
}

void appendCommittedFailure(
    Domain::Error& error,
    const Domain::Error& failure) noexcept
{
    try {
        error.message += " A post-commit quarantine operation also failed: ";
        error.message += failure.message;
    } catch (...) {
        // Preserve the committed evidence identifier and primary integrity failure.
    }
}

void cleanupPreservedFiles(
    const std::shared_ptr<Detail::DatabaseNamespaceLease>& quarantineNamespace,
    std::vector<PreservedFile>& preserved,
    Domain::Error& error) noexcept
{
    for (auto& file : preserved) {
        file.lease = Detail::DatabaseLeafLease{};
    }
    for (auto file = preserved.rbegin(); file != preserved.rend(); ++file) {
        auto removed = quarantineNamespace->deleteClosedLeaf(file->role, file->identity);
        if (!removed) {
            appendCleanupFailure(error, removed.error());
        }
    }
    preserved.clear();
}

class CreatedLeafRollback final {
public:
    CreatedLeafRollback(
        Detail::DatabaseNamespaceLease& namespaceLease,
        const Detail::DatabaseLeafRole role,
        Detail::DatabaseLeafLease& lease) noexcept
        : namespaceLease_{&namespaceLease},
          role_{role},
          identity_{lease.identity()},
          lease_{&lease}
    {
    }

    ~CreatedLeafRollback() noexcept
    {
        if (active_) {
            static_cast<void>(rollback());
        }
    }

    CreatedLeafRollback(const CreatedLeafRollback&) = delete;
    CreatedLeafRollback& operator=(const CreatedLeafRollback&) = delete;
    CreatedLeafRollback(CreatedLeafRollback&&) = delete;
    CreatedLeafRollback& operator=(CreatedLeafRollback&&) = delete;

    [[nodiscard]] Domain::Result<void> rollback() noexcept
    {
        if (!active_) {
            return Domain::Result<void>::success();
        }
        active_ = false;
        *lease_ = Detail::DatabaseLeafLease{};
        return namespaceLease_->deleteClosedLeaf(role_, identity_);
    }

    void dismiss() noexcept { active_ = false; }

private:
    Detail::DatabaseNamespaceLease* namespaceLease_{};
    Detail::DatabaseLeafRole role_{Detail::DatabaseLeafRole::Main};
    Detail::DatabaseFileIdentity identity_{};
    Detail::DatabaseLeafLease* lease_{};
    bool active_{true};
};

class EvidenceSetRollback final {
public:
    EvidenceSetRollback(
        std::shared_ptr<Detail::DatabaseNamespaceLease> namespaceLease,
        std::vector<PreservedFile>& preserved) noexcept
        : namespaceLease_{std::move(namespaceLease)}, preserved_{&preserved}
    {
    }

    ~EvidenceSetRollback() noexcept
    {
        if (active_ && namespaceLease_ && preserved_ != nullptr) {
            for (auto& file : *preserved_) {
                file.lease = Detail::DatabaseLeafLease{};
            }
            for (auto file = preserved_->rbegin(); file != preserved_->rend(); ++file) {
                static_cast<void>(namespaceLease_->deleteClosedLeaf(
                    file->role, file->identity));
            }
        }
    }

    EvidenceSetRollback(const EvidenceSetRollback&) = delete;
    EvidenceSetRollback& operator=(const EvidenceSetRollback&) = delete;
    EvidenceSetRollback(EvidenceSetRollback&&) = delete;
    EvidenceSetRollback& operator=(EvidenceSetRollback&&) = delete;

    void dismiss() noexcept { active_ = false; }

private:
    std::shared_ptr<Detail::DatabaseNamespaceLease> namespaceLease_;
    std::vector<PreservedFile>* preserved_{};
    bool active_{true};
};

[[nodiscard]] Domain::Result<PreservedFile> copyAndVerifyFile(
    const CapturedFile& source,
    const std::shared_ptr<Detail::DatabaseNamespaceLease>& quarantineNamespace,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto sourceBefore = hashFile(source.lease.nativeHandle(), context);
        if (!sourceBefore) {
            return Domain::Result<PreservedFile>::failure(
                std::move(sourceBefore).error());
        }
        if (sourceBefore.value().first != source.bytes) {
            return Domain::Result<PreservedFile>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "A database quarantine source changed after bounded preflight.",
                true));
        }

        auto destinationResult = quarantineNamespace->openLeafWithShareAccess(
            source.role,
            Detail::DatabaseLeafDisposition::CreateNew,
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ);
        if (!destinationResult) {
            return Domain::Result<PreservedFile>::failure(
                std::move(destinationResult).error());
        }
        auto destination = std::move(destinationResult).value();
        const Detail::DatabaseFileIdentity destinationIdentity = destination.identity();
        CreatedLeafRollback rollback{
            *quarantineNamespace, source.role, destination};
        const auto failAndRemove =
            [&](Domain::Error error) -> Domain::Result<PreservedFile> {
            auto removed = rollback.rollback();
            if (!removed) {
                appendCleanupFailure(error, removed.error());
            }
            return Domain::Result<PreservedFile>::failure(std::move(error));
        };

        auto copied = copyFile(
            source.lease.nativeHandle(),
            destination.nativeHandle(),
            sourceBefore.value().first,
            context);
        if (!copied) {
            return failAndRemove(std::move(copied).error());
        }
        auto destinationDigest = hashFile(destination.nativeHandle(), context);
        if (!destinationDigest) {
            return failAndRemove(std::move(destinationDigest).error());
        }
        auto sourceAfter = hashFile(source.lease.nativeHandle(), context);
        if (!sourceAfter) {
            return failAndRemove(std::move(sourceAfter).error());
        }
        if (sourceBefore.value() != sourceAfter.value() ||
            sourceBefore.value() != destinationDigest.value()) {
            return failAndRemove(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "A database quarantine source changed while its evidence copy was verified.",
                true));
        }

        const std::uint64_t verifiedBytes = destinationDigest.value().first;
        std::string verifiedHash = std::move(destinationDigest).value().second;
        rollback.dismiss();
        return Domain::Result<PreservedFile>::success(PreservedFile{
            source.role,
            verifiedBytes,
            std::move(verifiedHash),
            source.lease.identity(),
            destinationIdentity,
            std::move(destination)});
    } catch (...) {
        return Domain::Result<PreservedFile>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A database quarantine evidence file could not be created safely."));
    }
}

[[nodiscard]] std::string roleName(const Detail::DatabaseLeafRole role)
{
    switch (role) {
    case Detail::DatabaseLeafRole::Main:
        return "main";
    case Detail::DatabaseLeafRole::Wal:
        return "wal";
    case Detail::DatabaseLeafRole::SharedMemory:
        return "shared_memory";
    case Detail::DatabaseLeafRole::Journal:
        return "journal";
    case Detail::DatabaseLeafRole::MigrationLock:
        return "migration_lock";
    default:
        return "unknown";
    }
}

[[nodiscard]] std::string identityText(
    const Detail::DatabaseFileIdentity& identity)
{
    std::ostringstream value;
    value << std::hex << std::setfill('0') << std::setw(16)
          << identity.volumeSerialNumber << ':'
          << hexBytes(identity.fileIdentifier);
    return value.str();
}

[[nodiscard]] Domain::Result<Detail::DatabaseLeafLease> writeManifest(
    const std::shared_ptr<Detail::DatabaseNamespaceLease>& manifestNamespace,
    const nlohmann::json& manifest,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto validContext = checkContext(context);
        if (!validContext) {
            return Domain::Result<Detail::DatabaseLeafLease>::failure(
                std::move(validContext).error());
        }
        std::string bytes = manifest.dump(2);
        bytes.push_back('\n');
        if (bytes.size() > MaximumManifestBytes ||
            bytes.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
            return Domain::Result<Detail::DatabaseLeafLease>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The database quarantine manifest exceeds its bounded size."));
        }
        auto leaf = manifestNamespace->openLeafWithShareAccess(
            Detail::DatabaseLeafRole::Main,
            Detail::DatabaseLeafDisposition::CreateNew,
            GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ);
        if (!leaf) {
            return Domain::Result<Detail::DatabaseLeafLease>::failure(
                std::move(leaf).error());
        }
        auto manifestLeaf = std::move(leaf).value();
        CreatedLeafRollback rollback{
            *manifestNamespace,
            Detail::DatabaseLeafRole::Main,
            manifestLeaf};
        const auto failAndRemove =
            [&](Domain::Error error) -> Domain::Result<Detail::DatabaseLeafLease> {
            auto removed = rollback.rollback();
            if (!removed) {
                appendCleanupFailure(error, removed.error());
            }
            return Domain::Result<Detail::DatabaseLeafLease>::failure(std::move(error));
        };

        DWORD offset{};
        const DWORD total = static_cast<DWORD>(bytes.size());
        while (offset < total) {
            validContext = checkContext(context);
            if (!validContext) {
                return failAndRemove(std::move(validContext).error());
            }
            DWORD written{};
            if (::WriteFile(
                    manifestLeaf.nativeHandle(),
                    bytes.data() + offset,
                    total - offset,
                    &written,
                    nullptr) == FALSE) {
                return failAndRemove(
                    Infrastructure::Windows::Detail::makeWin32Error(
                        "write a database quarantine manifest", ::GetLastError()));
            }
            if (written == 0U) {
                return failAndRemove(Domain::makeError(
                    Domain::ErrorCodes::StorageFull,
                    "The database quarantine manifest write made no progress."));
            }
            offset += written;
        }
        if (::FlushFileBuffers(manifestLeaf.nativeHandle()) == FALSE) {
            return failAndRemove(
                Infrastructure::Windows::Detail::makeWin32Error(
                    "flush a database quarantine manifest", ::GetLastError()));
        }
        rollback.dismiss();
        return Domain::Result<Detail::DatabaseLeafLease>::success(
            std::move(manifestLeaf));
    } catch (...) {
        return Domain::Result<Detail::DatabaseLeafLease>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The database quarantine manifest could not be serialized."));
    }
}

} // namespace

Domain::Result<DatabaseQuarantineReport> DatabaseQuarantine::preserve(
    const std::shared_ptr<Detail::DatabaseNamespaceLease>& sourceNamespace,
    const Domain::Error& reason,
    const Domain::OperationContext& context,
    IDatabaseQuarantineObserver* const observer) noexcept
{
    std::optional<DatabaseQuarantineReport> committedReport;
    std::optional<Domain::Error> committedFailure;
    try {
        if (!sourceNamespace || reason.code != Domain::ErrorCodes::IntegrityFailure) {
            return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "Only an anchored integrity failure may enter database quarantine."));
        }
        auto validContext = checkContext(context);
        if (!validContext) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(validContext).error());
        }
        if (sourceNamespace->openVfsFileCount() != 0U) {
            return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                Domain::ErrorCodes::DatabaseBusy,
                "Database quarantine requires every SQLite VFS file to be closed.",
                true));
        }

        std::wstring operation{
            context.operationId.value().begin(), context.operationId.value().end()};
        std::wstring quarantineMain =
            sourceNamespace->leafName(Detail::DatabaseLeafRole::Main) +
            L".corrupt." + operation + L".sqlite";
        std::wstring quarantineLock = L"quarantine-" + operation + L".lock";
        std::wstring manifestMain = quarantineMain + L".manifest.json";
        std::wstring manifestLock = L"quarantine-manifest-" + operation + L".lock";
        if (quarantineMain.size() > Detail::DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            quarantineLock.size() > Detail::DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            manifestMain.size() > Detail::DatabaseNamespaceLease::MaximumLeafNameCharacters ||
            manifestLock.size() > Detail::DatabaseNamespaceLease::MaximumLeafNameCharacters) {
            return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The database quarantine leaf names exceed their bounded Windows limit."));
        }

        auto quarantineResult = Detail::DatabaseNamespaceLease::create(
            sourceNamespace->canonicalDirectory(), quarantineMain, quarantineLock);
        if (!quarantineResult) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(quarantineResult).error());
        }
        auto quarantineNamespace = std::move(quarantineResult).value();
        auto manifestResult = Detail::DatabaseNamespaceLease::create(
            sourceNamespace->canonicalDirectory(), manifestMain, manifestLock);
        if (!manifestResult) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(manifestResult).error());
        }
        auto manifestNamespace = std::move(manifestResult).value();

        auto cohortPath = Domain::PathText::create(
            quarantineNamespace->canonicalUtf8Path(Detail::DatabaseLeafRole::Main));
        if (!cohortPath) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(cohortPath).error());
        }
        auto manifestPath = Domain::PathText::create(
            manifestNamespace->canonicalUtf8Path(Detail::DatabaseLeafRole::Main));
        if (!manifestPath) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(manifestPath).error());
        }

        constexpr std::array CaptureOrder{
            Detail::DatabaseLeafRole::Main,
            Detail::DatabaseLeafRole::Wal,
            Detail::DatabaseLeafRole::SharedMemory,
            Detail::DatabaseLeafRole::Journal};
        auto capturedResult = sourceNamespace->captureClosedCohortForQuarantine();
        if (!capturedResult) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(capturedResult).error());
        }
        auto capturedLeases = std::move(capturedResult).value();
        std::vector<CapturedFile> captured;
        captured.reserve(capturedLeases.size());
        std::uint64_t cohortBytes{};
        for (auto& lease : capturedLeases) {
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(lease.nativeHandle(), &size) == FALSE || size.QuadPart < 0) {
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    Infrastructure::Windows::Detail::makeWin32Error(
                        "preflight a database quarantine source size", ::GetLastError()));
            }
            const std::uint64_t bytes = static_cast<std::uint64_t>(size.QuadPart);
            if (bytes > MaximumFileBytes ||
                cohortBytes > MaximumCohortBytes ||
                bytes > MaximumCohortBytes - cohortBytes) {
                return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The database quarantine cohort exceeds its bounded evidence size."));
            }
            cohortBytes += bytes;
            const Detail::DatabaseLeafRole role = lease.role();
            captured.push_back(CapturedFile{role, bytes, std::move(lease)});
        }
        capturedLeases.clear();
        if (observer != nullptr) {
            observer->onSourceCohortCaptured();
        }

        std::array<const Detail::DatabaseLeafLease*, CaptureOrder.size()> sourceLeaves{};
        for (std::size_t index = 0U; index < captured.size(); ++index) {
            sourceLeaves[index] = std::addressof(captured[index].lease);
        }
        auto stableSource = sourceNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{
                sourceLeaves.data(), captured.size()});
        if (!stableSource) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(stableSource).error());
        }

        for (const Detail::DatabaseLeafRole role : CaptureOrder) {
            auto existsAtDestination = quarantineNamespace->accessLeaf(
                role, Detail::DatabaseLeafAccess::Exists);
            if (!existsAtDestination) {
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    std::move(existsAtDestination).error());
            }
            if (existsAtDestination.value()) {
                return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "Database quarantine refuses to overwrite an existing evidence file.",
                    true));
            }
        }
        auto existingManifest = manifestNamespace->accessLeaf(
            Detail::DatabaseLeafRole::Main,
            Detail::DatabaseLeafAccess::Exists);
        if (!existingManifest) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(existingManifest).error());
        }
        if (existingManifest.value()) {
            return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "Database quarantine refuses to overwrite an existing evidence manifest.",
                true));
        }

        std::vector<PreservedFile> preserved;
        preserved.reserve(captured.size());
        EvidenceSetRollback evidenceRollback{quarantineNamespace, preserved};
        for (const auto& source : captured) {
            auto copied = copyAndVerifyFile(source, quarantineNamespace, context);
            if (!copied) {
                Domain::Error error = std::move(copied).error();
                cleanupPreservedFiles(quarantineNamespace, preserved, error);
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    std::move(error));
            }
            preserved.push_back(std::move(copied).value());
        }
        if (observer != nullptr) {
            observer->onEvidenceCohortCopied();
        }

        stableSource = sourceNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{
                sourceLeaves.data(), captured.size()});
        if (!stableSource) {
            Domain::Error error = std::move(stableSource).error();
            cleanupPreservedFiles(quarantineNamespace, preserved, error);
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(error));
        }
        std::array<const Detail::DatabaseLeafLease*, CaptureOrder.size()> evidenceLeaves{};
        for (std::size_t index = 0U; index < preserved.size(); ++index) {
            evidenceLeaves[index] = std::addressof(preserved[index].lease);
        }
        auto stableEvidence = quarantineNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{
                evidenceLeaves.data(), preserved.size()});
        if (!stableEvidence) {
            Domain::Error error = std::move(stableEvidence).error();
            cleanupPreservedFiles(quarantineNamespace, preserved, error);
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(error));
        }
        for (std::size_t index = 0U; index < captured.size(); ++index) {
            auto sourceDigest = hashFile(captured[index].lease.nativeHandle(), context);
            if (!sourceDigest) {
                Domain::Error error = std::move(sourceDigest).error();
                cleanupPreservedFiles(quarantineNamespace, preserved, error);
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    std::move(error));
            }
            auto evidenceDigest = hashFile(preserved[index].lease.nativeHandle(), context);
            if (!evidenceDigest) {
                Domain::Error error = std::move(evidenceDigest).error();
                cleanupPreservedFiles(quarantineNamespace, preserved, error);
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    std::move(error));
            }
            const std::pair<std::uint64_t, std::string> expectedDigest{
                preserved[index].bytes, preserved[index].sha256};
            if (sourceDigest.value() != expectedDigest ||
                evidenceDigest.value() != expectedDigest) {
                Domain::Error error = Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "A quarantine cohort changed before evidence-manifest commit.",
                    true);
                cleanupPreservedFiles(quarantineNamespace, preserved, error);
                return Domain::Result<DatabaseQuarantineReport>::failure(
                    std::move(error));
            }
        }

        std::string evidenceId{"p07-quarantine:"};
        evidenceId += context.operationId.value();
        nlohmann::json manifest = {
            {"schema_version", 1},
            {"evidence_id", evidenceId},
            {"copy_semantics", "anchored_stream_copy_before_source_removal"},
            {"source_cleanup_policy", "exact_identity_delete_after_evidence_commit"},
            {"reason", {
                {"code", reason.code},
                {"message", reason.message},
                {"retryable", reason.retryable}}},
            {"source_main", sourceNamespace->canonicalUtf8Path(
                Detail::DatabaseLeafRole::Main)},
            {"quarantine_main", quarantineNamespace->canonicalUtf8Path(
                Detail::DatabaseLeafRole::Main)},
            {"files", nlohmann::json::array()}};
        for (const auto& file : preserved) {
            manifest["files"].push_back({
                {"role", roleName(file.role)},
                {"path", quarantineNamespace->canonicalUtf8Path(file.role)},
                {"bytes", file.bytes},
                {"sha256", file.sha256},
                {"source_file_identity", identityText(file.sourceIdentity)},
                {"file_identity", identityText(file.identity)}});
        }
        auto manifestWritten = writeManifest(manifestNamespace, manifest, context);
        if (!manifestWritten) {
            Domain::Error error = std::move(manifestWritten).error();
            cleanupPreservedFiles(quarantineNamespace, preserved, error);
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(error));
        }
        auto manifestLease = std::move(manifestWritten).value();
        CreatedLeafRollback manifestRollback{
            *manifestNamespace,
            Detail::DatabaseLeafRole::Main,
            manifestLease};
        const std::array<const Detail::DatabaseLeafLease*, 1U> manifestLeaves{
            std::addressof(manifestLease)};
        auto stableManifest = manifestNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{manifestLeaves});
        if (!stableManifest) {
            Domain::Error error = std::move(stableManifest).error();
            auto manifestRemoved = manifestRollback.rollback();
            if (!manifestRemoved) {
                appendCleanupFailure(error, manifestRemoved.error());
            }
            cleanupPreservedFiles(quarantineNamespace, preserved, error);
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(error));
        }

        const std::size_t preservedCount = preserved.size();
        committedReport.emplace(DatabaseQuarantineReport{
            std::move(cohortPath).value(),
            std::move(manifestPath).value(),
            evidenceId,
            preservedCount});
        std::string committedFailureMessage{
            "The corrupt database evidence and manifest were committed, but exact removal "
            "of the source cohort was incomplete. Evidence is retained at "};
        committedFailureMessage += committedReport->cohortMainPath.value();
        committedFailureMessage += " with manifest ";
        committedFailureMessage += committedReport->manifestPath.value();
        committedFailureMessage += '.';
        committedFailure.emplace(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            std::move(committedFailureMessage),
            false,
            committedReport->evidenceId));
        evidenceRollback.dismiss();
        manifestRollback.dismiss();
        if (observer != nullptr) {
            observer->onManifestCommitted();
        }

        constexpr std::array RemovalOrder{
            Detail::DatabaseLeafRole::Journal,
            Detail::DatabaseLeafRole::Wal,
            Detail::DatabaseLeafRole::SharedMemory,
            Detail::DatabaseLeafRole::Main};
        bool removalFailed{};
        stableSource = sourceNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{
                sourceLeaves.data(), captured.size()});
        if (!stableSource) {
            removalFailed = true;
            appendCommittedFailure(*committedFailure, stableSource.error());
        }
        stableEvidence = quarantineNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{
                evidenceLeaves.data(), preserved.size()});
        if (!stableEvidence) {
            removalFailed = true;
            appendCommittedFailure(*committedFailure, stableEvidence.error());
        }
        stableManifest = manifestNamespace->revalidateExactQuarantineCohort(
            std::span<const Detail::DatabaseLeafLease* const>{manifestLeaves});
        if (!stableManifest) {
            removalFailed = true;
            appendCommittedFailure(*committedFailure, stableManifest.error());
        }
        if (removalFailed) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(committedFailure).value());
        }
        for (const Detail::DatabaseLeafRole role : RemovalOrder) {
            const auto found = std::find_if(
                captured.begin(), captured.end(),
                [role](const CapturedFile& file) noexcept {
                    return file.role == role;
                });
            if (found == captured.end()) {
                continue;
            }
            auto removed = sourceNamespace->deleteRetainedQuarantineLeaf(found->lease);
            if (!removed) {
                removalFailed = true;
                appendCommittedFailure(*committedFailure, removed.error());
            }
        }
        if (removalFailed) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(committedFailure).value());
        }

        return Domain::Result<DatabaseQuarantineReport>::success(
            std::move(committedReport).value());
    } catch (...) {
        if (committedFailure.has_value()) {
            return Domain::Result<DatabaseQuarantineReport>::failure(
                std::move(committedFailure).value());
        }
        return Domain::Result<DatabaseQuarantineReport>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The corrupt database cohort could not be quarantined safely."));
    }
}

} // namespace ForgeConductor::Persistence::Windows
