#include "AnchoredSqliteVfs.h"

#include <winsqlite/winsqlite3.h>

#include <Windows.h>
#include <bcrypt.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace ForgeConductor::Persistence::Windows::Detail {
namespace {

using NtReadFileFunction = NTSTATUS(NTAPI*)(
    HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
    PLARGE_INTEGER, PULONG);

struct ForwardingSqliteFile;
struct FrozenSqliteFile;

int SQLITE_CALLBACK anchoredOpen(
    sqlite3_vfs* vfs,
    sqlite3_filename name,
    sqlite3_file* file,
    int flags,
    int* outputFlags) noexcept;
int SQLITE_CALLBACK anchoredDelete(
    sqlite3_vfs* vfs,
    const char* name,
    int synchronizeDirectory) noexcept;
int SQLITE_CALLBACK anchoredAccess(
    sqlite3_vfs* vfs,
    const char* name,
    int flags,
    int* result) noexcept;
int SQLITE_CALLBACK anchoredFullPathname(
    sqlite3_vfs* vfs,
    const char* name,
    int outputBytes,
    char* output) noexcept;
void* SQLITE_CALLBACK anchoredDlOpen(sqlite3_vfs*, const char*) noexcept;
void SQLITE_CALLBACK anchoredDlError(sqlite3_vfs*, int outputBytes, char* output) noexcept;
void(SQLITE_CALLBACK* SQLITE_CALLBACK anchoredDlSym(sqlite3_vfs*, void*, const char*) noexcept)(void);
void SQLITE_CALLBACK anchoredDlClose(sqlite3_vfs*, void*) noexcept;
int SQLITE_CALLBACK anchoredRandomness(sqlite3_vfs* vfs, int bytes, char* output) noexcept;
int SQLITE_CALLBACK anchoredSleep(sqlite3_vfs* vfs, int microseconds) noexcept;
int SQLITE_CALLBACK anchoredCurrentTime(sqlite3_vfs* vfs, double* result) noexcept;
int SQLITE_CALLBACK anchoredGetLastError(sqlite3_vfs* vfs, int bytes, char* output) noexcept;
int SQLITE_CALLBACK anchoredCurrentTimeInt64(sqlite3_vfs* vfs, sqlite3_int64* result) noexcept;
int SQLITE_CALLBACK anchoredSetSystemCall(
    sqlite3_vfs*, const char*, sqlite3_syscall_ptr) noexcept;
sqlite3_syscall_ptr SQLITE_CALLBACK anchoredGetSystemCall(
    sqlite3_vfs*, const char*) noexcept;
const char* SQLITE_CALLBACK anchoredNextSystemCall(
    sqlite3_vfs*, const char*) noexcept;

int SQLITE_CALLBACK anchoredFileClose(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK anchoredFileRead(
    sqlite3_file* file, void* output, int amount, sqlite3_int64 offset) noexcept;
int SQLITE_CALLBACK anchoredFileWrite(
    sqlite3_file* file, const void* input, int amount, sqlite3_int64 offset) noexcept;
int SQLITE_CALLBACK anchoredFileTruncate(sqlite3_file* file, sqlite3_int64 size) noexcept;
int SQLITE_CALLBACK anchoredFileSync(sqlite3_file* file, int flags) noexcept;
int SQLITE_CALLBACK anchoredFileSize(sqlite3_file* file, sqlite3_int64* size) noexcept;
int SQLITE_CALLBACK anchoredFileLock(sqlite3_file* file, int lock) noexcept;
int SQLITE_CALLBACK anchoredFileUnlock(sqlite3_file* file, int lock) noexcept;
int SQLITE_CALLBACK anchoredFileCheckReservedLock(sqlite3_file* file, int* result) noexcept;
int SQLITE_CALLBACK anchoredFileControl(sqlite3_file* file, int operation, void* argument) noexcept;
int SQLITE_CALLBACK anchoredFileSectorSize(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK anchoredFileDeviceCharacteristics(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK anchoredFileShmMap(
    sqlite3_file* file,
    int page,
    int pageSize,
    int extend,
    void volatile** mapping) noexcept;
int SQLITE_CALLBACK anchoredFileShmLock(
    sqlite3_file* file, int offset, int count, int flags) noexcept;
void SQLITE_CALLBACK anchoredFileShmBarrier(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK anchoredFileShmUnmap(sqlite3_file* file, int deleteFlag) noexcept;
int SQLITE_CALLBACK anchoredFileFetch(
    sqlite3_file* file,
    sqlite3_int64 offset,
    int amount,
    void** output) noexcept;
int SQLITE_CALLBACK anchoredFileUnfetch(
    sqlite3_file* file,
    sqlite3_int64 offset,
    void* pointer) noexcept;

int SQLITE_CALLBACK frozenFileClose(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK frozenFileRead(
    sqlite3_file* file, void* output, int amount, sqlite3_int64 offset) noexcept;
int SQLITE_CALLBACK frozenFileWrite(
    sqlite3_file* file, const void* input, int amount, sqlite3_int64 offset) noexcept;
int SQLITE_CALLBACK frozenFileTruncate(sqlite3_file* file, sqlite3_int64 size) noexcept;
int SQLITE_CALLBACK frozenFileSync(sqlite3_file* file, int flags) noexcept;
int SQLITE_CALLBACK frozenFileSize(sqlite3_file* file, sqlite3_int64* size) noexcept;
int SQLITE_CALLBACK frozenFileLock(sqlite3_file* file, int lock) noexcept;
int SQLITE_CALLBACK frozenFileUnlock(sqlite3_file* file, int lock) noexcept;
int SQLITE_CALLBACK frozenFileCheckReservedLock(sqlite3_file* file, int* result) noexcept;
int SQLITE_CALLBACK frozenFileControl(
    sqlite3_file* file, int operation, void* argument) noexcept;
int SQLITE_CALLBACK frozenFileSectorSize(sqlite3_file* file) noexcept;
int SQLITE_CALLBACK frozenFileDeviceCharacteristics(sqlite3_file* file) noexcept;

constexpr sqlite3_io_methods ForwardingIoMethods{
    3,
    &anchoredFileClose,
    &anchoredFileRead,
    &anchoredFileWrite,
    &anchoredFileTruncate,
    &anchoredFileSync,
    &anchoredFileSize,
    &anchoredFileLock,
    &anchoredFileUnlock,
    &anchoredFileCheckReservedLock,
    &anchoredFileControl,
    &anchoredFileSectorSize,
    &anchoredFileDeviceCharacteristics,
    &anchoredFileShmMap,
    &anchoredFileShmLock,
    &anchoredFileShmBarrier,
    &anchoredFileShmUnmap,
    &anchoredFileFetch,
    &anchoredFileUnfetch,
};

[[nodiscard]] Domain::Error vfsError(
    const std::string_view code,
    std::string message,
    const bool retryable = false) noexcept
{
    try {
        return Domain::makeError(code, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored SQLite VFS failed and its diagnostic could not be retained.",
            retryable);
    }
}

[[nodiscard]] int sqliteCodeForError(
    const Domain::Error& error,
    const int fallback) noexcept
{
    if (error.code == Domain::ErrorCodes::DatabaseBusy ||
        error.code == Domain::ErrorCodes::Conflict) {
        return SQLITE_BUSY;
    }
    if (error.code == Domain::ErrorCodes::StorageFull) {
        return SQLITE_FULL;
    }
    if (error.code == Domain::ErrorCodes::Cancelled ||
        error.code == Domain::ErrorCodes::DeadlineExceeded) {
        return SQLITE_INTERRUPT;
    }
    if (error.code == Domain::ErrorCodes::IntegrityFailure) {
        return SQLITE_CORRUPT;
    }
    if (error.code == Domain::ErrorCodes::InvalidRequest ||
        error.code == Domain::ErrorCodes::Unauthorized ||
        error.code == Domain::ErrorCodes::PathOutsideAuthority ||
        error.code == Domain::ErrorCodes::RecordNotFound) {
        return SQLITE_CANTOPEN;
    }
    return fallback;
}

[[nodiscard]] std::size_t alignUp(
    const std::size_t value,
    const std::size_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

class ExclusiveSrwLockGuard final {
public:
    explicit ExclusiveSrwLockGuard(SRWLOCK& lock) noexcept
        : lock_{&lock}
    {
        ::AcquireSRWLockExclusive(lock_);
    }

    ~ExclusiveSrwLockGuard() noexcept
    {
        ::ReleaseSRWLockExclusive(lock_);
    }

    ExclusiveSrwLockGuard(const ExclusiveSrwLockGuard&) = delete;
    ExclusiveSrwLockGuard& operator=(const ExclusiveSrwLockGuard&) = delete;

private:
    SRWLOCK* lock_{};
};

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToUtf16(
    const char* value,
    const std::size_t maximumBytes) noexcept
{
    try {
        if (value == nullptr || maximumBytes == 0U) {
            return Domain::Result<std::wstring>::failure(vfsError(
                Domain::ErrorCodes::InvalidRequest,
                "SQLite supplied a null or empty database filename."));
        }
        const std::size_t length = ::strnlen_s(value, maximumBytes + 1U);
        if (length == 0U || length > maximumBytes ||
            length > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(vfsError(
                Domain::ErrorCodes::LimitExceeded,
                "SQLite supplied an unterminated or oversized database filename."));
        }
        const int inputLength = static_cast<int>(length);
        const int required = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value, inputLength, nullptr, 0);
        if (required <= 0) {
            return Domain::Result<std::wstring>::failure(vfsError(
                Domain::ErrorCodes::InvalidRequest,
                "SQLite supplied a database filename that is not strict UTF-8."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value, inputLength,
                converted.data(), required) != required) {
            return Domain::Result<std::wstring>::failure(vfsError(
                Domain::ErrorCodes::InvalidRequest,
                "SQLite supplied a database filename that could not be converted."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(vfsError(
            Domain::ErrorCodes::InternalFailure,
            "SQLite filename conversion failed."));
    }
}

[[nodiscard]] bool allowedSqliteRole(
    const DatabaseLeafRole role,
    const int flags) noexcept
{
    constexpr int TypeMask = SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL |
                             SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_TEMP_JOURNAL |
                             SQLITE_OPEN_TRANSIENT_DB | SQLITE_OPEN_SUBJOURNAL |
                             SQLITE_OPEN_SUPER_JOURNAL | SQLITE_OPEN_WAL;
    const int type = flags & TypeMask;
    switch (role) {
    case DatabaseLeafRole::Main:
        return type == SQLITE_OPEN_MAIN_DB;
    case DatabaseLeafRole::Journal:
        return type == SQLITE_OPEN_MAIN_JOURNAL;
    case DatabaseLeafRole::Wal:
    case DatabaseLeafRole::SharedMemory:
        return type == SQLITE_OPEN_WAL;
    case DatabaseLeafRole::MigrationLock:
        return false;
    }
    return false;
}

[[nodiscard]] bool forbiddenOpenFlags(const int flags) noexcept
{
    constexpr int Forbidden = SQLITE_OPEN_DELETEONCLOSE |
                              SQLITE_OPEN_AUTOPROXY |
                              SQLITE_OPEN_URI |
                              SQLITE_OPEN_MEMORY |
                              SQLITE_OPEN_TEMP_DB |
                              SQLITE_OPEN_TEMP_JOURNAL |
                              SQLITE_OPEN_TRANSIENT_DB |
                              SQLITE_OPEN_SUBJOURNAL |
                              SQLITE_OPEN_SUPER_JOURNAL;
    return (flags & Forbidden) != 0;
}

[[nodiscard]] DatabaseLeafDisposition leafDispositionForFlags(const int flags) noexcept
{
    if ((flags & SQLITE_OPEN_CREATE) == 0) {
        return DatabaseLeafDisposition::OpenExisting;
    }
    if ((flags & SQLITE_OPEN_EXCLUSIVE) != 0) {
        return DatabaseLeafDisposition::CreateNew;
    }
    return DatabaseLeafDisposition::OpenOrCreate;
}

[[nodiscard]] ACCESS_MASK leafAccessForFlags(const int flags) noexcept
{
    if ((flags & SQLITE_OPEN_READWRITE) != 0) {
        return GENERIC_READ | GENERIC_WRITE;
    }
    return GENERIC_READ;
}

[[nodiscard]] Domain::Result<std::string> makeUniqueVfsName() noexcept
{
    try {
        constexpr std::size_t MaximumAttempts = 8U;
        constexpr std::array<char, 16U> Hexadecimal{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        for (std::size_t attempt = 0U; attempt < MaximumAttempts; ++attempt) {
            std::array<unsigned char, 16U> random{};
            const NTSTATUS status = ::BCryptGenRandom(
                nullptr, random.data(), static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            if (status < 0) {
                return Domain::Result<std::string>::failure(vfsError(
                    Domain::ErrorCodes::InternalFailure,
                    "The anchored SQLite VFS could not obtain a unique system-random name."));
            }
            std::string candidate{"forge-anchored-"};
            candidate.reserve(candidate.size() + random.size() * 2U);
            for (const unsigned char value : random) {
                candidate.push_back(Hexadecimal[(value >> 4U) & 0x0fU]);
                candidate.push_back(Hexadecimal[value & 0x0fU]);
            }
            if (candidate.size() <= AnchoredSqliteVfs::MaximumVfsNameBytes &&
                ::sqlite3_vfs_find(candidate.c_str()) == nullptr) {
                return Domain::Result<std::string>::success(std::move(candidate));
            }
        }
        return Domain::Result<std::string>::failure(vfsError(
            Domain::ErrorCodes::Conflict,
            "The anchored SQLite VFS could not reserve a unique bounded name.",
            true));
    } catch (...) {
        return Domain::Result<std::string>::failure(vfsError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored SQLite VFS could not form a unique name."));
    }
}

} // namespace

struct AnchoredSqliteVfsState final {
    sqlite3_vfs vfs{};
    sqlite3_vfs* underlying{};
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease;
    std::string name;
    bool frozenReadOnlyVerification{};
    Infrastructure::Windows::Detail::UniqueHandle frozenMainHandle;
    DatabaseFileIdentity frozenMainIdentity{};
    NtReadFileFunction readFileAt{};

    mutable SRWLOCK lifecycleLock{SRWLOCK_INIT};
    std::shared_ptr<AnchoredSqliteVfsState> registrationKeepAlive;
    std::size_t openFiles{};
    std::size_t activeCallbacks{};
    bool registered{};
    bool unregistering{};
    bool ownerReleased{};

    [[nodiscard]] std::shared_ptr<AnchoredSqliteVfsState> enterCallback() noexcept
    {
        ExclusiveSrwLockGuard lock{lifecycleLock};
        if (!registered || unregistering || !registrationKeepAlive) {
            return {};
        }
        ++activeCallbacks;
        return registrationKeepAlive;
    }

    void leaveCallback() noexcept
    {
        bool attemptUnregister{};
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            if (activeCallbacks != 0U) {
                --activeCallbacks;
            }
            attemptUnregister = ownerReleased && registered && !unregistering &&
                                activeCallbacks == 0U && openFiles == 0U;
        }
        if (attemptUnregister) {
            static_cast<void>(unregisterWhenIdle());
        }
    }

    void noteFileOpened() noexcept
    {
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            ++openFiles;
        }
        namespaceLease->noteVfsFileOpened();
    }

    void noteFileClosed() noexcept
    {
        namespaceLease->noteVfsFileClosed();
        bool attemptUnregister{};
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            if (openFiles != 0U) {
                --openFiles;
            }
            attemptUnregister = ownerReleased && registered && !unregistering &&
                                activeCallbacks == 0U && openFiles == 0U;
        }
        if (attemptUnregister) {
            static_cast<void>(unregisterWhenIdle());
        }
    }

    [[nodiscard]] bool unregisterWhenIdle() noexcept
    {
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            if (!registered) {
                return true;
            }
            if (unregistering || activeCallbacks != 0U || openFiles != 0U) {
                return false;
            }
            unregistering = true;
        }

        const int result = ::sqlite3_vfs_unregister(&vfs);
        std::shared_ptr<AnchoredSqliteVfsState> releaseKeepAlive;
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            unregistering = false;
            if (result == SQLITE_OK) {
                registered = false;
                releaseKeepAlive = std::move(registrationKeepAlive);
            }
        }
        return result == SQLITE_OK;
    }

    void releaseOwner() noexcept
    {
        {
            ExclusiveSrwLockGuard lock{lifecycleLock};
            ownerReleased = true;
        }
        static_cast<void>(unregisterWhenIdle());
    }
};

namespace {

class VfsCallbackScope final {
public:
    explicit VfsCallbackScope(sqlite3_vfs* vfs) noexcept
    {
        if (vfs != nullptr && vfs->pAppData != nullptr) {
            auto* const raw = static_cast<AnchoredSqliteVfsState*>(vfs->pAppData);
            state_ = raw->enterCallback();
        }
    }

    ~VfsCallbackScope() noexcept
    {
        if (state_) {
            state_->leaveCallback();
        }
    }

    VfsCallbackScope(const VfsCallbackScope&) = delete;
    VfsCallbackScope& operator=(const VfsCallbackScope&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] AnchoredSqliteVfsState& state() const noexcept { return *state_; }
    [[nodiscard]] const std::shared_ptr<AnchoredSqliteVfsState>& sharedState() const noexcept
    {
        return state_;
    }

private:
    std::shared_ptr<AnchoredSqliteVfsState> state_;
};

struct ForwardingSqliteFile final {
    sqlite3_file base{};
    sqlite3_file* delegated{};
    std::shared_ptr<AnchoredSqliteVfsState> state;
    DatabaseLeafLease leaf;
    std::optional<DatabaseLeafLease> sharedMemoryLeaf;
    DatabaseLeafRole role{DatabaseLeafRole::Main};
};

static_assert(offsetof(ForwardingSqliteFile, base) == 0U);

struct FrozenSqliteFile final {
    sqlite3_file base{};
    std::shared_ptr<AnchoredSqliteVfsState> state;
};

static_assert(offsetof(FrozenSqliteFile, base) == 0U);

constexpr sqlite3_io_methods FrozenIoMethods{
    1,
    &frozenFileClose,
    &frozenFileRead,
    &frozenFileWrite,
    &frozenFileTruncate,
    &frozenFileSync,
    &frozenFileSize,
    &frozenFileLock,
    &frozenFileUnlock,
    &frozenFileCheckReservedLock,
    &frozenFileControl,
    &frozenFileSectorSize,
    &frozenFileDeviceCharacteristics,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

[[nodiscard]] FrozenSqliteFile* frozenFile(sqlite3_file* file) noexcept
{
    return reinterpret_cast<FrozenSqliteFile*>(file);
}

int SQLITE_CALLBACK frozenFileClose(sqlite3_file* const file) noexcept
{
    try {
        auto* const frozen = frozenFile(file);
        if (frozen == nullptr || !frozen->state) {
            return SQLITE_IOERR_CLOSE;
        }
        const auto state = frozen->state;
        frozen->base.pMethods = nullptr;
        frozen->~FrozenSqliteFile();
        state->noteFileClosed();
        return SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR_CLOSE;
    }
}

int SQLITE_CALLBACK frozenFileRead(
    sqlite3_file* const file,
    void* const output,
    const int amount,
    const sqlite3_int64 offset) noexcept
{
    try {
        auto* const frozen = frozenFile(file);
        if (frozen == nullptr || !frozen->state || output == nullptr ||
            amount < 0 || offset < 0 ||
            offset > (std::numeric_limits<sqlite3_int64>::max)() - amount) {
            return SQLITE_IOERR_READ;
        }
        if (amount == 0) {
            return SQLITE_OK;
        }
        auto& state = *frozen->state;
        if (!state.frozenMainHandle) {
            return SQLITE_IOERR_READ;
        }

        LARGE_INTEGER position{};
        position.QuadPart = offset;
        IO_STATUS_BLOCK ioStatus{};
        constexpr NTSTATUS EndOfFileStatus = static_cast<NTSTATUS>(0xC0000011L);
        if (state.readFileAt == nullptr) {
            return SQLITE_IOERR_READ;
        }
        const NTSTATUS status = state.readFileAt(
            state.frozenMainHandle.get(), nullptr, nullptr, nullptr,
            &ioStatus, output, static_cast<ULONG>(amount), &position, nullptr);
        if (status < 0 && status != EndOfFileStatus) {
            return SQLITE_IOERR_READ;
        }
        const std::size_t bytesRead = status == EndOfFileStatus
            ? 0U
            : static_cast<std::size_t>(ioStatus.Information);
        if (bytesRead > static_cast<std::size_t>(amount)) {
            return SQLITE_IOERR_READ;
        }
        if (bytesRead != static_cast<std::size_t>(amount)) {
            std::memset(
                static_cast<std::byte*>(output) + bytesRead,
                0,
                static_cast<std::size_t>(amount) - bytesRead);
            return SQLITE_IOERR_SHORT_READ;
        }
        return SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR_READ;
    }
}

int SQLITE_CALLBACK frozenFileWrite(
    sqlite3_file*, const void*, int, sqlite3_int64) noexcept
{
    return SQLITE_READONLY;
}

int SQLITE_CALLBACK frozenFileTruncate(sqlite3_file*, sqlite3_int64) noexcept
{
    return SQLITE_READONLY;
}

int SQLITE_CALLBACK frozenFileSync(sqlite3_file*, int) noexcept
{
    return SQLITE_READONLY;
}

int SQLITE_CALLBACK frozenFileSize(
    sqlite3_file* const file,
    sqlite3_int64* const size) noexcept
{
    if (size != nullptr) {
        *size = 0;
    }
    try {
        auto* const frozen = frozenFile(file);
        if (frozen == nullptr || !frozen->state || size == nullptr ||
            !frozen->state->frozenMainHandle) {
            return SQLITE_IOERR_FSTAT;
        }
        LARGE_INTEGER nativeSize{};
        if (::GetFileSizeEx(
                frozen->state->frozenMainHandle.get(), &nativeSize) == FALSE ||
            nativeSize.QuadPart < 0) {
            return SQLITE_IOERR_FSTAT;
        }
        *size = nativeSize.QuadPart;
        return SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR_FSTAT;
    }
}

int SQLITE_CALLBACK frozenFileLock(sqlite3_file*, const int lock) noexcept
{
    return lock <= SQLITE_LOCK_SHARED ? SQLITE_OK : SQLITE_READONLY;
}

int SQLITE_CALLBACK frozenFileUnlock(sqlite3_file*, int) noexcept
{
    return SQLITE_OK;
}

int SQLITE_CALLBACK frozenFileCheckReservedLock(
    sqlite3_file*, int* const result) noexcept
{
    if (result == nullptr) {
        return SQLITE_IOERR_CHECKRESERVEDLOCK;
    }
    *result = 0;
    return SQLITE_OK;
}

int SQLITE_CALLBACK frozenFileControl(
    sqlite3_file*, const int operation, void* const argument) noexcept
{
    if (operation == SQLITE_FCNTL_LOCKSTATE && argument != nullptr) {
        *static_cast<int*>(argument) = SQLITE_LOCK_SHARED;
        return SQLITE_OK;
    }
    if (operation == SQLITE_FCNTL_HAS_MOVED && argument != nullptr) {
        *static_cast<int*>(argument) = 0;
        return SQLITE_OK;
    }
    return SQLITE_NOTFOUND;
}

int SQLITE_CALLBACK frozenFileSectorSize(sqlite3_file*) noexcept
{
    return 4'096;
}

int SQLITE_CALLBACK frozenFileDeviceCharacteristics(sqlite3_file*) noexcept
{
    return SQLITE_IOCAP_IMMUTABLE;
}

[[nodiscard]] ForwardingSqliteFile* forwardingFile(sqlite3_file* file) noexcept
{
    return reinterpret_cast<ForwardingSqliteFile*>(file);
}

[[nodiscard]] sqlite3_file* delegatedStorage(
    sqlite3_file* wrapper,
    const std::size_t offset) noexcept
{
    return reinterpret_cast<sqlite3_file*>(
        reinterpret_cast<std::byte*>(wrapper) + offset);
}

[[nodiscard]] std::size_t delegatedOffset() noexcept
{
    return alignUp(sizeof(ForwardingSqliteFile), alignof(sqlite3_file));
}

[[nodiscard]] Domain::Result<DatabaseLeafRole> classifyName(
    AnchoredSqliteVfsState& state,
    const char* name) noexcept
{
    const std::size_t maximum = static_cast<std::size_t>(
        (std::max)(state.vfs.mxPathname, 1));
    auto wide = strictUtf8ToUtf16(name, maximum);
    if (!wide) {
        return Domain::Result<DatabaseLeafRole>::failure(std::move(wide).error());
    }
    return state.namespaceLease->classifyCanonicalPath(wide.value());
}

[[nodiscard]] bool safeFileControl(const int operation) noexcept
{
    switch (operation) {
    case SQLITE_FCNTL_LOCKSTATE:
    case SQLITE_FCNTL_LAST_ERRNO:
    case SQLITE_FCNTL_SIZE_HINT:
    case SQLITE_FCNTL_CHUNK_SIZE:
    case SQLITE_FCNTL_SYNC_OMITTED:
    case SQLITE_FCNTL_WIN32_AV_RETRY:
    case SQLITE_FCNTL_PERSIST_WAL:
    case SQLITE_FCNTL_OVERWRITE:
    case SQLITE_FCNTL_POWERSAFE_OVERWRITE:
    case SQLITE_FCNTL_PRAGMA:
    case SQLITE_FCNTL_BUSYHANDLER:
    case SQLITE_FCNTL_MMAP_SIZE:
    case SQLITE_FCNTL_TRACE:
    case SQLITE_FCNTL_HAS_MOVED:
    case SQLITE_FCNTL_SYNC:
    case SQLITE_FCNTL_COMMIT_PHASETWO:
    case SQLITE_FCNTL_WAL_BLOCK:
    case SQLITE_FCNTL_BEGIN_ATOMIC_WRITE:
    case SQLITE_FCNTL_COMMIT_ATOMIC_WRITE:
    case SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE:
    case SQLITE_FCNTL_LOCK_TIMEOUT:
    case SQLITE_FCNTL_DATA_VERSION:
    case SQLITE_FCNTL_SIZE_LIMIT:
    case SQLITE_FCNTL_CKPT_DONE:
    case SQLITE_FCNTL_RESERVE_BYTES:
    case SQLITE_FCNTL_CKPT_START:
    case SQLITE_FCNTL_EXTERNAL_READER:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] const sqlite3_io_methods* delegatedMethods(
    const ForwardingSqliteFile* file) noexcept
{
    return file != nullptr && file->delegated != nullptr
               ? file->delegated->pMethods
               : nullptr;
}

int SQLITE_CALLBACK anchoredFileClose(sqlite3_file* const file) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        if (wrapper == nullptr || !wrapper->state || wrapper->delegated == nullptr) {
            return SQLITE_IOERR_CLOSE;
        }
        const auto state = wrapper->state;
        const sqlite3_io_methods* const methods = delegatedMethods(wrapper);
        int result = SQLITE_OK;
        if (methods == nullptr || methods->xClose == nullptr) {
            result = SQLITE_IOERR_CLOSE;
        } else {
            result = methods->xClose(wrapper->delegated);
        }
        wrapper->base.pMethods = nullptr;
        wrapper->sharedMemoryLeaf.reset();
        wrapper->leaf = DatabaseLeafLease{};
        wrapper->~ForwardingSqliteFile();
        state->noteFileClosed();
        return result;
    } catch (...) {
        return SQLITE_IOERR_CLOSE;
    }
}

int SQLITE_CALLBACK anchoredFileRead(
    sqlite3_file* const file,
    void* const output,
    const int amount,
    const sqlite3_int64 offset) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xRead != nullptr
                   ? methods->xRead(wrapper->delegated, output, amount, offset)
                   : SQLITE_IOERR_READ;
    } catch (...) {
        return SQLITE_IOERR_READ;
    }
}

int SQLITE_CALLBACK anchoredFileWrite(
    sqlite3_file* const file,
    const void* const input,
    const int amount,
    const sqlite3_int64 offset) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xWrite != nullptr
                   ? methods->xWrite(wrapper->delegated, input, amount, offset)
                   : SQLITE_IOERR_WRITE;
    } catch (...) {
        return SQLITE_IOERR_WRITE;
    }
}

int SQLITE_CALLBACK anchoredFileTruncate(
    sqlite3_file* const file,
    const sqlite3_int64 size) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xTruncate != nullptr
                   ? methods->xTruncate(wrapper->delegated, size)
                   : SQLITE_IOERR_TRUNCATE;
    } catch (...) {
        return SQLITE_IOERR_TRUNCATE;
    }
}

int SQLITE_CALLBACK anchoredFileSync(
    sqlite3_file* const file,
    const int flags) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xSync != nullptr
                   ? methods->xSync(wrapper->delegated, flags)
                   : SQLITE_IOERR_FSYNC;
    } catch (...) {
        return SQLITE_IOERR_FSYNC;
    }
}

int SQLITE_CALLBACK anchoredFileSize(
    sqlite3_file* const file,
    sqlite3_int64* const size) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xFileSize != nullptr
                   ? methods->xFileSize(wrapper->delegated, size)
                   : SQLITE_IOERR_FSTAT;
    } catch (...) {
        return SQLITE_IOERR_FSTAT;
    }
}

int SQLITE_CALLBACK anchoredFileLock(
    sqlite3_file* const file,
    const int lock) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xLock != nullptr
                   ? methods->xLock(wrapper->delegated, lock)
                   : SQLITE_IOERR_LOCK;
    } catch (...) {
        return SQLITE_IOERR_LOCK;
    }
}

int SQLITE_CALLBACK anchoredFileUnlock(
    sqlite3_file* const file,
    const int lock) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xUnlock != nullptr
                   ? methods->xUnlock(wrapper->delegated, lock)
                   : SQLITE_IOERR_UNLOCK;
    } catch (...) {
        return SQLITE_IOERR_UNLOCK;
    }
}

int SQLITE_CALLBACK anchoredFileCheckReservedLock(
    sqlite3_file* const file,
    int* const result) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xCheckReservedLock != nullptr
                   ? methods->xCheckReservedLock(wrapper->delegated, result)
                   : SQLITE_IOERR_CHECKRESERVEDLOCK;
    } catch (...) {
        return SQLITE_IOERR_CHECKRESERVEDLOCK;
    }
}

int SQLITE_CALLBACK anchoredFileControl(
    sqlite3_file* const file,
    const int operation,
    void* const argument) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        if (wrapper == nullptr || !wrapper->state || methods == nullptr ||
            methods->xFileControl == nullptr) {
            return SQLITE_NOTFOUND;
        }
        if (operation == SQLITE_FCNTL_VFS_POINTER) {
            if (argument == nullptr) {
                return SQLITE_MISUSE;
            }
            *static_cast<sqlite3_vfs**>(argument) = &wrapper->state->vfs;
            return SQLITE_OK;
        }
        if (operation == SQLITE_FCNTL_FILE_POINTER) {
            if (argument == nullptr) {
                return SQLITE_MISUSE;
            }
            *static_cast<sqlite3_file**>(argument) = &wrapper->base;
            return SQLITE_OK;
        }
        if (operation == SQLITE_FCNTL_VFSNAME) {
            if (argument == nullptr) {
                return SQLITE_MISUSE;
            }
            auto** const output = static_cast<char**>(argument);
            *output = nullptr;
            char* delegatedName = nullptr;
            const int delegatedResult = methods->xFileControl(
                wrapper->delegated, SQLITE_FCNTL_VFSNAME, &delegatedName);
            if (delegatedResult != SQLITE_OK && delegatedResult != SQLITE_NOTFOUND) {
                ::sqlite3_free(delegatedName);
                return delegatedResult;
            }
            const char* const lowerName = delegatedName != nullptr
                                              ? delegatedName
                                              : wrapper->state->underlying->zName;
            *output = ::sqlite3_mprintf(
                "%s/%s", wrapper->state->name.c_str(),
                lowerName != nullptr ? lowerName : "win32");
            ::sqlite3_free(delegatedName);
            return *output != nullptr ? SQLITE_OK : SQLITE_NOMEM;
        }

        if (operation == SQLITE_FCNTL_TEMPFILENAME ||
            operation == SQLITE_FCNTL_WIN32_SET_HANDLE ||
            operation == SQLITE_FCNTL_WIN32_GET_HANDLE ||
            !safeFileControl(operation)) {
            return SQLITE_NOTFOUND;
        }
        return methods->xFileControl(wrapper->delegated, operation, argument);
    } catch (const std::bad_alloc&) {
        return SQLITE_NOMEM;
    } catch (...) {
        return SQLITE_IOERR;
    }
}

int SQLITE_CALLBACK anchoredFileSectorSize(sqlite3_file* const file) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->xSectorSize != nullptr
                   ? methods->xSectorSize(wrapper->delegated)
                   : 0;
    } catch (...) {
        return 0;
    }
}

int SQLITE_CALLBACK anchoredFileDeviceCharacteristics(sqlite3_file* const file) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        const int delegated = methods != nullptr && methods->xDeviceCharacteristics != nullptr
                                  ? methods->xDeviceCharacteristics(wrapper->delegated)
                                  : 0;
        return delegated | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
    } catch (...) {
        return SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
    }
}

int SQLITE_CALLBACK anchoredFileShmMap(
    sqlite3_file* const file,
    const int page,
    const int pageSize,
    const int extend,
    void volatile** const mapping) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        if (wrapper == nullptr || !wrapper->state || mapping == nullptr ||
            wrapper->role != DatabaseLeafRole::Main || methods == nullptr ||
            methods->iVersion < 2 || methods->xShmMap == nullptr) {
            return SQLITE_IOERR_SHMOPEN;
        }
        const bool newlyPinned = !wrapper->sharedMemoryLeaf.has_value();
        bool pinAfterMapping{};
        if (newlyPinned) {
            auto pinned = wrapper->state->namespaceLease->openLeaf(
                DatabaseLeafRole::SharedMemory,
                extend != 0 ? DatabaseLeafDisposition::OpenOrCreate
                            : DatabaseLeafDisposition::OpenExisting,
                extend != 0 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ);
            if (!pinned) {
                if (extend == 0 &&
                    pinned.error().code == Domain::ErrorCodes::RecordNotFound) {
                    pinAfterMapping = true;
                } else {
                    return sqliteCodeForError(pinned.error(), SQLITE_IOERR_SHMOPEN);
                }
            } else {
                wrapper->sharedMemoryLeaf.emplace(std::move(pinned).value());
            }
        }
        const int result = methods->xShmMap(
            wrapper->delegated, page, pageSize, extend, mapping);
        if (result != SQLITE_OK) {
            if (newlyPinned) {
                wrapper->sharedMemoryLeaf.reset();
            }
            return result;
        }
        if (pinAfterMapping) {
            auto pinned = wrapper->state->namespaceLease->openLeaf(
                DatabaseLeafRole::SharedMemory,
                DatabaseLeafDisposition::OpenExisting,
                GENERIC_READ);
            if (!pinned) {
                if (pinned.error().code == Domain::ErrorCodes::RecordNotFound &&
                    *mapping == nullptr) {
                    return SQLITE_OK;
                }
                static_cast<void>(methods->xShmUnmap(wrapper->delegated, 0));
                return sqliteCodeForError(pinned.error(), SQLITE_IOERR_SHMOPEN);
            }
            wrapper->sharedMemoryLeaf.emplace(std::move(pinned).value());
            auto stableCohort = wrapper->state->namespaceLease->revalidateCohort();
            if (!stableCohort) {
                static_cast<void>(methods->xShmUnmap(wrapper->delegated, 0));
                wrapper->sharedMemoryLeaf.reset();
                return sqliteCodeForError(stableCohort.error(), SQLITE_IOERR_SHMOPEN);
            }
        }
        return SQLITE_OK;
    } catch (const std::bad_alloc&) {
        return SQLITE_NOMEM;
    } catch (...) {
        return SQLITE_IOERR_SHMOPEN;
    }
}

int SQLITE_CALLBACK anchoredFileShmLock(
    sqlite3_file* const file,
    const int offset,
    const int count,
    const int flags) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->iVersion >= 2 &&
                       methods->xShmLock != nullptr
                   ? methods->xShmLock(wrapper->delegated, offset, count, flags)
                   : SQLITE_IOERR_SHMLOCK;
    } catch (...) {
        return SQLITE_IOERR_SHMLOCK;
    }
}

void SQLITE_CALLBACK anchoredFileShmBarrier(sqlite3_file* const file) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        if (methods != nullptr && methods->iVersion >= 2 &&
            methods->xShmBarrier != nullptr) {
            methods->xShmBarrier(wrapper->delegated);
        }
    } catch (...) {
    }
}

int SQLITE_CALLBACK anchoredFileShmUnmap(
    sqlite3_file* const file,
    const int deleteFlag) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        if (wrapper == nullptr || !wrapper->state || methods == nullptr ||
            methods->iVersion < 2 || methods->xShmUnmap == nullptr) {
            return SQLITE_IOERR_SHMOPEN;
        }
        // The inbox VFS closes its maps first. Deletion is then performed by the
        // namespace lease using the retained directory anchor and fixed SHM role.
        const int result = methods->xShmUnmap(wrapper->delegated, 0);
        wrapper->sharedMemoryLeaf.reset();
        if (result != SQLITE_OK || deleteFlag == 0) {
            return result;
        }
        auto removed = wrapper->state->namespaceLease->deleteTransientLeaf(
            DatabaseLeafRole::SharedMemory);
        return removed ? SQLITE_OK
                       : sqliteCodeForError(removed.error(), SQLITE_IOERR_DELETE);
    } catch (...) {
        return SQLITE_IOERR_SHMOPEN;
    }
}

int SQLITE_CALLBACK anchoredFileFetch(
    sqlite3_file* const file,
    const sqlite3_int64 offset,
    const int amount,
    void** const output) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        if (output != nullptr) {
            *output = nullptr;
        }
        return methods != nullptr && methods->iVersion >= 3 && methods->xFetch != nullptr
                   ? methods->xFetch(wrapper->delegated, offset, amount, output)
                   : SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR;
    }
}

int SQLITE_CALLBACK anchoredFileUnfetch(
    sqlite3_file* const file,
    const sqlite3_int64 offset,
    void* const pointer) noexcept
{
    try {
        auto* const wrapper = forwardingFile(file);
        const auto* const methods = delegatedMethods(wrapper);
        return methods != nullptr && methods->iVersion >= 3 && methods->xUnfetch != nullptr
                   ? methods->xUnfetch(wrapper->delegated, offset, pointer)
                   : SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR;
    }
}

[[nodiscard]] Domain::Result<void> initializeNativeVfs(
    const std::shared_ptr<AnchoredSqliteVfsState>& state) noexcept
{
    try {
        sqlite3_vfs* underlying = ::sqlite3_vfs_find("win32-longpath");
        if (underlying == nullptr) {
            underlying = ::sqlite3_vfs_find("win32");
        }
        if (underlying == nullptr || underlying->zName == nullptr ||
            underlying->szOsFile <= 0 || underlying->mxPathname <= 0 ||
            underlying->xOpen == nullptr || underlying->xDelete == nullptr ||
            underlying->xAccess == nullptr || underlying->xFullPathname == nullptr) {
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The Windows SDK SQLite inbox VFS is unavailable or incomplete."));
        }
        if (std::string_view{underlying->zName} != "win32" &&
            std::string_view{underlying->zName} != "win32-longpath") {
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The anchored VFS refuses to forward to a non-inbox SQLite VFS."));
        }

        const std::size_t wrapperBytes = delegatedOffset();
        const std::size_t totalBytes = (std::max)(
            wrapperBytes + static_cast<std::size_t>(underlying->szOsFile),
            sizeof(FrozenSqliteFile));
        if (totalBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::LimitExceeded,
                "The inbox SQLite file object exceeds the anchored VFS allocation limit."));
        }
        for (const DatabaseLeafRole role :
             {DatabaseLeafRole::Main, DatabaseLeafRole::Wal,
              DatabaseLeafRole::SharedMemory, DatabaseLeafRole::Journal}) {
            if (state->namespaceLease->canonicalUtf8Path(role).size() + 1U >
                static_cast<std::size_t>(underlying->mxPathname)) {
                return Domain::Result<void>::failure(vfsError(
                    Domain::ErrorCodes::LimitExceeded,
                    "A canonical database path exceeds the inbox SQLite VFS pathname limit."));
            }
        }

        auto uniqueName = makeUniqueVfsName();
        if (!uniqueName) {
            return Domain::Result<void>::failure(std::move(uniqueName).error());
        }

        state->underlying = underlying;
        const HMODULE nativeModule = ::GetModuleHandleW(L"ntdll.dll");
        state->readFileAt = nativeModule != nullptr
            ? reinterpret_cast<NtReadFileFunction>(
                  ::GetProcAddress(nativeModule, "NtReadFile"))
            : nullptr;
        if (state->readFileAt == nullptr) {
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The Windows native explicit-offset file read API is unavailable."));
        }
        state->name = std::move(uniqueName).value();
        state->vfs.iVersion = 3;
        state->vfs.szOsFile = static_cast<int>(totalBytes);
        state->vfs.mxPathname = underlying->mxPathname;
        state->vfs.pNext = nullptr;
        state->vfs.zName = state->name.c_str();
        state->vfs.pAppData = state.get();
        state->vfs.xOpen = &anchoredOpen;
        state->vfs.xDelete = &anchoredDelete;
        state->vfs.xAccess = &anchoredAccess;
        state->vfs.xFullPathname = &anchoredFullPathname;
        state->vfs.xDlOpen = &anchoredDlOpen;
        state->vfs.xDlError = &anchoredDlError;
        state->vfs.xDlSym = &anchoredDlSym;
        state->vfs.xDlClose = &anchoredDlClose;
        state->vfs.xRandomness = &anchoredRandomness;
        state->vfs.xSleep = &anchoredSleep;
        state->vfs.xCurrentTime = &anchoredCurrentTime;
        state->vfs.xGetLastError = &anchoredGetLastError;
        state->vfs.xCurrentTimeInt64 = &anchoredCurrentTimeInt64;
        state->vfs.xSetSystemCall = &anchoredSetSystemCall;
        state->vfs.xGetSystemCall = &anchoredGetSystemCall;
        state->vfs.xNextSystemCall = &anchoredNextSystemCall;

        {
            ExclusiveSrwLockGuard lock{state->lifecycleLock};
            state->registrationKeepAlive = state;
        }
        const int registration = ::sqlite3_vfs_register(&state->vfs, 0);
        if (registration != SQLITE_OK ||
            ::sqlite3_vfs_find(state->name.c_str()) != &state->vfs) {
            if (registration == SQLITE_OK) {
                static_cast<void>(::sqlite3_vfs_unregister(&state->vfs));
            }
            ExclusiveSrwLockGuard lock{state->lifecycleLock};
            state->registrationKeepAlive.reset();
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::InternalFailure,
                "The anchored SQLite VFS could not be registered as a non-default VFS."));
        }
        {
            ExclusiveSrwLockGuard lock{state->lifecycleLock};
            state->registered = true;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(vfsError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored SQLite VFS could not be initialized."));
    }
}

} // namespace

AnchoredSqliteVfs::AnchoredSqliteVfs(
    std::shared_ptr<AnchoredSqliteVfsState> state) noexcept
    : state_{std::move(state)}
{
}

Domain::Result<std::unique_ptr<AnchoredSqliteVfs>> AnchoredSqliteVfs::create(
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease) noexcept
{
    return createWithMode(std::move(namespaceLease), false, nullptr);
}

Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>
AnchoredSqliteVfs::createFrozenReadOnlyVerification(
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
    const DatabaseLeafLease& retainedMain) noexcept
{
    return createWithMode(std::move(namespaceLease), true, &retainedMain);
}

Domain::Result<std::unique_ptr<AnchoredSqliteVfs>> AnchoredSqliteVfs::createWithMode(
    std::shared_ptr<DatabaseNamespaceLease> namespaceLease,
    const bool frozenReadOnlyVerification,
    const DatabaseLeafLease* const retainedMain) noexcept
{
    try {
        if (!namespaceLease) {
            return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(vfsError(
                Domain::ErrorCodes::InvalidRequest,
                "An anchored SQLite VFS requires an owned database namespace lease."));
        }
        if (frozenReadOnlyVerification &&
            (retainedMain == nullptr || !*retainedMain ||
             retainedMain->role() != DatabaseLeafRole::Main)) {
            return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(vfsError(
                Domain::ErrorCodes::InvalidRequest,
                "Frozen SQLite verification requires a retained exact main leaf."));
        }
        auto validNamespace = frozenReadOnlyVerification
            ? namespaceLease->revalidateRetainedLeaf(*retainedMain)
            : namespaceLease->revalidate();
        if (!validNamespace) {
            return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(
                std::move(validNamespace).error());
        }
        auto state = std::make_shared<AnchoredSqliteVfsState>();
        state->namespaceLease = std::move(namespaceLease);
        state->frozenReadOnlyVerification = frozenReadOnlyVerification;
        if (frozenReadOnlyVerification) {
            HANDLE duplicate{};
            if (::DuplicateHandle(
                    ::GetCurrentProcess(), retainedMain->nativeHandle(),
                    ::GetCurrentProcess(), &duplicate, 0U, FALSE,
                    DUPLICATE_SAME_ACCESS) == FALSE ||
                duplicate == nullptr || duplicate == INVALID_HANDLE_VALUE) {
                return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(vfsError(
                    Domain::ErrorCodes::InternalFailure,
                    "The retained database handle could not be duplicated for immutable verification."));
            }
            state->frozenMainHandle =
                Infrastructure::Windows::Detail::UniqueHandle{duplicate};
            state->frozenMainIdentity = retainedMain->identity();
        }
        // Allocate the public owner before registration so no post-registration
        // allocation failure can strand a self-retained VFS registration.
        auto owner = std::unique_ptr<AnchoredSqliteVfs>{
            new AnchoredSqliteVfs{state}};
        auto initialized = initializeNativeVfs(state);
        if (!initialized) {
            return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(
                std::move(initialized).error());
        }
        return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::success(
            std::move(owner));
    } catch (...) {
        return Domain::Result<std::unique_ptr<AnchoredSqliteVfs>>::failure(vfsError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored SQLite VFS owner could not be created."));
    }
}

AnchoredSqliteVfs::~AnchoredSqliteVfs() noexcept
{
    if (state_) {
        state_->releaseOwner();
    }
}

std::string_view AnchoredSqliteVfs::vfsName() const noexcept
{
    return state_ ? std::string_view{state_->name} : std::string_view{};
}

bool AnchoredSqliteVfs::registrationBelongsTo(
    const std::string_view vfsName,
    const DatabaseNamespaceLease& namespaceLease) noexcept
{
    try {
        if (vfsName.empty() || vfsName.size() > MaximumVfsNameBytes ||
            vfsName.find('\0') != std::string_view::npos) {
            return false;
        }
        const std::string name{vfsName};
        sqlite3_vfs* const vfs = ::sqlite3_vfs_find(name.c_str());
        if (vfs == nullptr || vfs->pAppData == nullptr ||
            vfs->xOpen != &anchoredOpen || vfs->xDelete != &anchoredDelete ||
            vfs->xAccess != &anchoredAccess || vfs->xFullPathname != &anchoredFullPathname) {
            return false;
        }
        auto* const raw = static_cast<AnchoredSqliteVfsState*>(vfs->pAppData);
        auto state = raw->enterCallback();
        if (!state) {
            return false;
        }
        const bool matches = &state->vfs == vfs && state->name == vfsName &&
            state->namespaceLease.get() == &namespaceLease;
        state->leaveCallback();
        return matches;
    } catch (...) {
        return false;
    }
}

std::size_t AnchoredSqliteVfs::openFileCount() const noexcept
{
    if (!state_) {
        return 0U;
    }
    ExclusiveSrwLockGuard lock{state_->lifecycleLock};
    return state_->openFiles;
}

bool AnchoredSqliteVfs::isRegistered() const noexcept
{
    if (!state_) {
        return false;
    }
    ExclusiveSrwLockGuard lock{state_->lifecycleLock};
    return state_->registered;
}

Domain::Result<void> AnchoredSqliteVfs::close() noexcept
{
    try {
        if (!state_) {
            return Domain::Result<void>::success();
        }
        {
            ExclusiveSrwLockGuard lock{state_->lifecycleLock};
            if (!state_->registered) {
                state_->ownerReleased = true;
                return Domain::Result<void>::success();
            }
            if (state_->openFiles != 0U || state_->activeCallbacks != 0U ||
                state_->unregistering) {
                return Domain::Result<void>::failure(vfsError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The anchored SQLite VFS cannot unregister until every connection file is closed.",
                    true));
            }
        }
        if (!state_->unregisterWhenIdle()) {
            return Domain::Result<void>::failure(vfsError(
                Domain::ErrorCodes::InternalFailure,
                "The anchored SQLite VFS could not be unregistered."));
        }
        {
            ExclusiveSrwLockGuard lock{state_->lifecycleLock};
            state_->ownerReleased = true;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(vfsError(
            Domain::ErrorCodes::InternalFailure,
            "The anchored SQLite VFS could not be closed."));
    }
}


namespace {

int SQLITE_CALLBACK anchoredOpen(
    sqlite3_vfs* const vfs,
    const sqlite3_filename name,
    sqlite3_file* const file,
    const int flags,
    int* const outputFlags) noexcept
{
    if (file != nullptr) {
        file->pMethods = nullptr;
    }
    if (outputFlags != nullptr) {
        *outputFlags = 0;
    }
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || file == nullptr || name == nullptr ||
            forbiddenOpenFlags(flags)) {
            return SQLITE_CANTOPEN;
        }
        const bool readOnly = (flags & SQLITE_OPEN_READONLY) != 0;
        const bool readWrite = (flags & SQLITE_OPEN_READWRITE) != 0;
        if (readOnly == readWrite ||
            ((flags & SQLITE_OPEN_CREATE) != 0 && !readWrite)) {
            return SQLITE_CANTOPEN;
        }

        auto roleResult = classifyName(callback.state(), name);
        if (!roleResult || !allowedSqliteRole(roleResult.value(), flags)) {
            return SQLITE_CANTOPEN;
        }
        const DatabaseLeafRole role = roleResult.value();
        if (callback.state().frozenReadOnlyVerification &&
            (role != DatabaseLeafRole::Main || !readOnly || readWrite ||
             (flags & SQLITE_OPEN_CREATE) != 0)) {
            return SQLITE_CANTOPEN;
        }
        if (callback.state().frozenReadOnlyVerification) {
            if (!callback.state().frozenMainHandle) {
                return SQLITE_CANTOPEN;
            }
            auto* const frozen = ::new (file) FrozenSqliteFile{};
            frozen->state = callback.sharedState();
            frozen->base.pMethods = &FrozenIoMethods;
            if (outputFlags != nullptr) {
                *outputFlags = SQLITE_OPEN_READONLY | SQLITE_OPEN_MAIN_DB;
            }
            callback.state().noteFileOpened();
            return SQLITE_OK;
        }

        auto leafResult = callback.state().namespaceLease->openLeaf(
            role, leafDispositionForFlags(flags), leafAccessForFlags(flags));
        if (!leafResult) {
            return sqliteCodeForError(leafResult.error(), SQLITE_CANTOPEN);
        }

        auto* const wrapper = ::new (file) ForwardingSqliteFile{};
        wrapper->state = callback.sharedState();
        wrapper->leaf = std::move(leafResult).value();
        wrapper->role = role;
        wrapper->delegated = delegatedStorage(file, delegatedOffset());
        std::memset(
            wrapper->delegated, 0,
            static_cast<std::size_t>(callback.state().underlying->szOsFile));

        int delegatedFlags = flags | SQLITE_OPEN_NOFOLLOW;
        if (wrapper->leaf.wasCreated()) {
            // The handle-relative create already linearized exclusive creation.
            // The inbox VFS must now open that exact pinned object, not attempt a
            // second pathname-based exclusive create.
            delegatedFlags &= ~SQLITE_OPEN_EXCLUSIVE;
        }
        const std::string& canonicalName =
            callback.state().namespaceLease->canonicalUtf8Path(role);
        const int result = callback.state().underlying->xOpen(
            callback.state().underlying,
            canonicalName.c_str(),
            wrapper->delegated,
            delegatedFlags,
            outputFlags);
        if (result != SQLITE_OK || wrapper->delegated->pMethods == nullptr) {
            if (wrapper->delegated->pMethods != nullptr &&
                wrapper->delegated->pMethods->xClose != nullptr) {
                static_cast<void>(wrapper->delegated->pMethods->xClose(wrapper->delegated));
            }
            const bool created = wrapper->leaf.wasCreated();
            const DatabaseFileIdentity identity = wrapper->leaf.identity();
            const auto state = wrapper->state;
            wrapper->~ForwardingSqliteFile();
            file->pMethods = nullptr;
            if (created) {
                static_cast<void>(state->namespaceLease->discardCreatedLeaf(role, identity));
            }
            return result == SQLITE_OK ? SQLITE_IOERR : result;
        }

        wrapper->base.pMethods = &ForwardingIoMethods;
        callback.state().noteFileOpened();
        return SQLITE_OK;
    } catch (const std::bad_alloc&) {
        if (file != nullptr) {
            file->pMethods = nullptr;
        }
        return SQLITE_NOMEM;
    } catch (...) {
        if (file != nullptr) {
            file->pMethods = nullptr;
        }
        return SQLITE_IOERR;
    }
}

int SQLITE_CALLBACK anchoredDelete(
    sqlite3_vfs* const vfs,
    const char* const name,
    const int) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || name == nullptr) {
            return SQLITE_IOERR_DELETE;
        }
        if (callback.state().frozenReadOnlyVerification) {
            return SQLITE_READONLY;
        }
        auto role = classifyName(callback.state(), name);
        if (!role ||
            (role.value() != DatabaseLeafRole::Wal &&
             role.value() != DatabaseLeafRole::SharedMemory &&
             role.value() != DatabaseLeafRole::Journal)) {
            return SQLITE_IOERR_DELETE;
        }
        auto removed = callback.state().namespaceLease->deleteTransientLeaf(role.value());
        return removed ? SQLITE_OK
                       : sqliteCodeForError(removed.error(), SQLITE_IOERR_DELETE);
    } catch (...) {
        return SQLITE_IOERR_DELETE;
    }
}

int SQLITE_CALLBACK anchoredAccess(
    sqlite3_vfs* const vfs,
    const char* const name,
    const int flags,
    int* const result) noexcept
{
    if (result != nullptr) {
        *result = 0;
    }
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || name == nullptr || result == nullptr ||
            (flags != SQLITE_ACCESS_EXISTS && flags != SQLITE_ACCESS_READ &&
             flags != SQLITE_ACCESS_READWRITE)) {
            return SQLITE_IOERR_ACCESS;
        }
        auto role = classifyName(callback.state(), name);
        if (!role || role.value() == DatabaseLeafRole::MigrationLock) {
            return SQLITE_IOERR_ACCESS;
        }
        if (callback.state().frozenReadOnlyVerification) {
            if (role.value() == DatabaseLeafRole::Main) {
                *result = flags == SQLITE_ACCESS_READWRITE ? 0 : 1;
            } else {
                *result = 0;
            }
            return SQLITE_OK;
        }
        DatabaseLeafAccess access = DatabaseLeafAccess::Exists;
        if (flags == SQLITE_ACCESS_READ) {
            access = DatabaseLeafAccess::Read;
        } else if (flags == SQLITE_ACCESS_READWRITE) {
            access = DatabaseLeafAccess::ReadWrite;
        }
        auto accessible = callback.state().namespaceLease->accessLeaf(role.value(), access);
        if (!accessible) {
            return sqliteCodeForError(accessible.error(), SQLITE_IOERR_ACCESS);
        }
        *result = accessible.value() ? 1 : 0;
        return SQLITE_OK;
    } catch (...) {
        return SQLITE_IOERR_ACCESS;
    }
}

int SQLITE_CALLBACK anchoredFullPathname(
    sqlite3_vfs* const vfs,
    const char* const name,
    const int outputBytes,
    char* const output) noexcept
{
    if (output != nullptr && outputBytes > 0) {
        output[0] = '\0';
    }
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || name == nullptr || output == nullptr || outputBytes <= 0) {
            return SQLITE_CANTOPEN;
        }
        auto role = classifyName(callback.state(), name);
        if (!role || role.value() == DatabaseLeafRole::MigrationLock) {
            return SQLITE_CANTOPEN;
        }
        const std::string& canonical =
            callback.state().namespaceLease->canonicalUtf8Path(role.value());
        if (canonical.size() + 1U > static_cast<std::size_t>(outputBytes)) {
            return SQLITE_CANTOPEN;
        }
        std::memcpy(output, canonical.c_str(), canonical.size() + 1U);
        return SQLITE_OK;
    } catch (...) {
        return SQLITE_CANTOPEN;
    }
}

void* SQLITE_CALLBACK anchoredDlOpen(sqlite3_vfs*, const char*) noexcept
{
    return nullptr;
}

void SQLITE_CALLBACK anchoredDlError(
    sqlite3_vfs*,
    const int outputBytes,
    char* const output) noexcept
{
    if (output != nullptr && outputBytes > 0) {
        ::sqlite3_snprintf(
            outputBytes, output,
            "Dynamic extension loading is disabled by the anchored VFS");
    }
}

void(SQLITE_CALLBACK* SQLITE_CALLBACK anchoredDlSym(
    sqlite3_vfs*, void*, const char*) noexcept)(void)
{
    return nullptr;
}

void SQLITE_CALLBACK anchoredDlClose(sqlite3_vfs*, void*) noexcept
{
}

int SQLITE_CALLBACK anchoredRandomness(
    sqlite3_vfs* const vfs,
    const int bytes,
    char* const output) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || callback.state().underlying->xRandomness == nullptr) {
            return 0;
        }
        return callback.state().underlying->xRandomness(
            callback.state().underlying, bytes, output);
    } catch (...) {
        return 0;
    }
}

int SQLITE_CALLBACK anchoredSleep(
    sqlite3_vfs* const vfs,
    const int microseconds) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || callback.state().underlying->xSleep == nullptr) {
            return 0;
        }
        return callback.state().underlying->xSleep(
            callback.state().underlying, microseconds);
    } catch (...) {
        return 0;
    }
}

int SQLITE_CALLBACK anchoredCurrentTime(
    sqlite3_vfs* const vfs,
    double* const result) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || callback.state().underlying->xCurrentTime == nullptr) {
            return SQLITE_IOERR;
        }
        return callback.state().underlying->xCurrentTime(
            callback.state().underlying, result);
    } catch (...) {
        return SQLITE_IOERR;
    }
}

int SQLITE_CALLBACK anchoredGetLastError(
    sqlite3_vfs* const vfs,
    const int bytes,
    char* const output) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || callback.state().underlying->xGetLastError == nullptr) {
            return 0;
        }
        return callback.state().underlying->xGetLastError(
            callback.state().underlying, bytes, output);
    } catch (...) {
        return 0;
    }
}

int SQLITE_CALLBACK anchoredCurrentTimeInt64(
    sqlite3_vfs* const vfs,
    sqlite3_int64* const result) noexcept
{
    try {
        VfsCallbackScope callback{vfs};
        if (!callback || callback.state().underlying->iVersion < 2 ||
            callback.state().underlying->xCurrentTimeInt64 == nullptr) {
            return SQLITE_IOERR;
        }
        return callback.state().underlying->xCurrentTimeInt64(
            callback.state().underlying, result);
    } catch (...) {
        return SQLITE_IOERR;
    }
}

int SQLITE_CALLBACK anchoredSetSystemCall(
    sqlite3_vfs*, const char*, sqlite3_syscall_ptr) noexcept
{
    return SQLITE_NOTFOUND;
}

sqlite3_syscall_ptr SQLITE_CALLBACK anchoredGetSystemCall(
    sqlite3_vfs*, const char*) noexcept
{
    return nullptr;
}

const char* SQLITE_CALLBACK anchoredNextSystemCall(
    sqlite3_vfs*, const char*) noexcept
{
    return nullptr;
}

} // namespace

} // namespace ForgeConductor::Persistence::Windows::Detail
