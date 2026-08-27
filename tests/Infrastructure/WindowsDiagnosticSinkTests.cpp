#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h"
#include "Infrastructure/Windows/Detail/DiagnosticDirectoryTree.h"
#include "Infrastructure/Windows/Detail/DiagnosticRotationPublishObserver.h"
#include "Infrastructure/Windows/Detail/RelativeFileOperations.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail
{

[[nodiscard]] Domain::Result<void> validateDiagnosticDirectoryCaseSensitivity(DWORD flags) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail

namespace ForgeConductor::Tests
{
namespace
{

using Infrastructure::Windows::BCryptSha256Hasher;
using Infrastructure::Windows::SecretRedactor;
using Infrastructure::Windows::WindowsAtomicFileStore;
using Infrastructure::Windows::WindowsDiagnosticSink;
using Infrastructure::Windows::WindowsDiagnosticSinkOptions;
namespace WindowsDetail = Infrastructure::Windows::Detail;
using Json = nlohmann::json;

static_assert(std::is_constructible_v<
              WindowsDiagnosticSink, WindowsDiagnosticSinkOptions, std::shared_ptr<Contracts::IClock>,
              std::shared_ptr<Contracts::IRedactor>, std::shared_ptr<Contracts::IHasher>,
              std::shared_ptr<Contracts::IWorkspaceAuthority>, std::shared_ptr<Contracts::IAtomicFileStore>>);
static_assert(!std::is_constructible_v<
              WindowsDiagnosticSink, WindowsDiagnosticSinkOptions, std::shared_ptr<Contracts::IClock>,
              std::shared_ptr<Contracts::IRedactor>, std::shared_ptr<Contracts::IHasher>,
              std::shared_ptr<Contracts::IWorkspaceAuthority>, std::shared_ptr<Contracts::IAtomicFileStore>,
              std::shared_ptr<WindowsDetail::IDiagnosticRotationPublishObserver>>);

constexpr std::wstring_view DiagnosticRotationTemporaryPrefix = L".forge-diagnostics-rotation-";
constexpr std::uint64_t RotationStressBytes = 64U * 1024U * 1024U;
constexpr wchar_t DiagnosticCrashChildVariable[] = L"FORGE_DIAGNOSTIC_CRASH_CHILD";
constexpr wchar_t DiagnosticCrashRootVariable[] = L"FORGE_DIAGNOSTIC_CRASH_ROOT";

[[nodiscard]] std::string utf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0, "test path must convert to UTF-8");
    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                              converted.data(), required, nullptr, nullptr);
    require(written == required, "test path UTF-8 conversion must be complete");
    return converted;
}

[[nodiscard]] std::wstring utf16(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    require(required > 0, "test path must convert to UTF-16");
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), converted.data(), required);
    require(written == required, "test path UTF-16 conversion must be complete");
    return converted;
}

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path &path)
{
    return take(Domain::PathText::create(utf8(path.native())));
}

void createDirectoryJunction(const std::filesystem::path &junction, const std::filesystem::path &target)
{
    struct MountPointBuffer final
    {
        DWORD tag{};
        WORD dataLength{};
        WORD reserved{};
        WORD substituteOffset{};
        WORD substituteLength{};
        WORD printOffset{};
        WORD printLength{};
        wchar_t pathBuffer[1]{};
    };
    static_assert(offsetof(MountPointBuffer, pathBuffer) == 16U);

    std::error_code error;
    require(std::filesystem::create_directory(junction, error) && !error, "junction fixture directory must be created");
    WindowsDetail::UniqueHandle handle{
        ::CreateFileW(junction.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(handle), "junction fixture must open for reparse metadata writes");

    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring print = target.native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = print.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    const std::size_t dataBytes = 8U + pathBytes;
    const std::size_t bufferBytes = offsetof(MountPointBuffer, pathBuffer) + pathBytes;
    require(dataBytes <= (std::numeric_limits<WORD>::max)(), "junction fixture reparse payload must remain bounded");
    const std::size_t wordCount = (bufferBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t);
    std::vector<std::uint64_t> storage(wordCount, 0U);
    auto *const reparse = reinterpret_cast<MountPointBuffer *>(storage.data());
    reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->dataLength = static_cast<WORD>(dataBytes);
    reparse->substituteLength = static_cast<WORD>(substituteBytes);
    reparse->printOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    reparse->printLength = static_cast<WORD>(printBytes);
    std::memcpy(reparse->pathBuffer, substitute.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte *>(reparse->pathBuffer) + reparse->printOffset, print.data(), printBytes);

    DWORD returned{};
    require(::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, reparse, static_cast<DWORD>(bufferBytes), nullptr,
                              0U, &returned, nullptr) != FALSE,
            "junction fixture reparse metadata must be installed");
}

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        require(length > 0U && length < buffer.size(), "GetTempPathW must succeed");
        const std::filesystem::path root{std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        for (std::uint64_t attempt = 0U; attempt < 32U; ++attempt)
        {
            path_ = root / (L"forge-diagnostic-tests-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                            std::to_wstring(::GetCurrentThreadId()) + L"-" + std::to_wstring(::GetTickCount64()) +
                            L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error))
            {
                return;
            }
        }
        throw TestFailure{"could not create isolated diagnostics test directory"};
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

class HeldDiagnosticFileLock final
{
  public:
    explicit HeldDiagnosticFileLock(const std::filesystem::path &root)
    {
        const auto path = root / L".forge-diagnostics.lock";
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle_ != INVALID_HANDLE_VALUE, "could not open the held diagnostic lock fixture");
        OVERLAPPED operation{};
        require(::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &operation) !=
                    FALSE,
                "could not acquire the held diagnostic lock fixture");
    }

    ~HeldDiagnosticFileLock() noexcept
    {
        release();
    }

    void release() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            OVERLAPPED operation{};
            static_cast<void>(::UnlockFileEx(handle_, 0U, 1U, 0U, &operation));
            static_cast<void>(::CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HeldDiagnosticFileLock(const HeldDiagnosticFileLock &) = delete;
    HeldDiagnosticFileLock &operator=(const HeldDiagnosticFileLock &) = delete;

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class FixedClock final : public Contracts::IClock
{
  public:
    FixedClock()
        : monotonic_{std::chrono::steady_clock::now()}, utc_{Domain::UtcTimePoint{std::chrono::seconds{1'787'650'000}}}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic_;
    }

    [[nodiscard]] Domain::UtcTimePoint utc() const noexcept
    {
        return utc_;
    }
    [[nodiscard]] Domain::MonotonicTimePoint monotonic() const noexcept
    {
        return monotonic_;
    }

  private:
    Domain::MonotonicTimePoint monotonic_;
    Domain::UtcTimePoint utc_;
};

[[nodiscard]] Domain::OperationContext liveContext(const FixedClock &clock, const std::stop_token cancellation = {})
{
    return Domain::OperationContext{parse<Domain::OperationId>("55555555-5555-4555-8555-555555555555"),
                                    clock.monotonic() + std::chrono::seconds{30}, cancellation,
                                    parse<Domain::CorrelationId>("p06-diagnostic-test")};
}

class DirectoryRenameObserver final : public WindowsDetail::IDiagnosticDirectoryAnchorObserver
{
  public:
    DirectoryRenameObserver(std::filesystem::path parent, std::filesystem::path renamed) noexcept
        : parent_{std::move(parent)}, renamed_{std::move(renamed)}
    {
    }

    void onDirectoryAnchored(const std::wstring_view path) noexcept override
    {
        const auto &parent = parent_.native();
        if (observed_ || path.size() != parent.size() ||
            ::CompareStringOrdinal(path.data(), static_cast<int>(path.size()), parent.data(),
                                   static_cast<int>(parent.size()), TRUE) != CSTR_EQUAL)
        {
            return;
        }

        observed_ = true;
        WindowsDetail::UniqueHandle reparseWriter{
            ::CreateFileW(parent_.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (reparseWriter)
        {
            reparseWriterOpened_ = true;
        }
        else
        {
            reparseWriterError_ = ::GetLastError();
        }
        if (::MoveFileExW(parent_.c_str(), renamed_.c_str(), 0U) == FALSE)
        {
            renameError_ = ::GetLastError();
            return;
        }
        renameSucceeded_ = true;
        restoreSucceeded_ = ::MoveFileExW(renamed_.c_str(), parent_.c_str(), 0U) != FALSE;
    }

    [[nodiscard]] bool observed() const noexcept
    {
        return observed_;
    }
    [[nodiscard]] bool reparseWriterOpened() const noexcept
    {
        return reparseWriterOpened_;
    }
    [[nodiscard]] bool renameSucceeded() const noexcept
    {
        return renameSucceeded_;
    }
    [[nodiscard]] bool restoreSucceeded() const noexcept
    {
        return restoreSucceeded_;
    }
    [[nodiscard]] DWORD reparseWriterError() const noexcept
    {
        return reparseWriterError_;
    }
    [[nodiscard]] DWORD renameError() const noexcept
    {
        return renameError_;
    }

  private:
    std::filesystem::path parent_;
    std::filesystem::path renamed_;
    bool observed_{};
    bool reparseWriterOpened_{};
    bool renameSucceeded_{};
    bool restoreSucceeded_{};
    DWORD reparseWriterError_{ERROR_SUCCESS};
    DWORD renameError_{ERROR_SUCCESS};
};

class ExportAuthority final : public Contracts::IWorkspaceAuthority
{
  public:
    explicit ExportAuthority(Domain::PathText root)
        : root_{std::move(root)},
          authority_{take(issueAuthority(
              parse<Domain::AuthorityId>("66666666-6666-4666-8666-666666666666"),
              parse<Domain::ProjectId>("77777777-7777-4777-8777-777777777777"),
              parse<Domain::ClientId>("p06-diagnostic-client"), {root_}, Domain::FileAccess::Create,
              {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create}, {}, false, 1U))}
    {
    }

    [[nodiscard]] const Contracts::WorkspaceAuthority &authority() const noexcept
    {
        return authority_;
    }

    void setDenyAll(const bool value) noexcept
    {
        denyAll_ = value;
    }

    [[nodiscard]] std::size_t authorizationCount() const noexcept
    {
        return authorizationCount_;
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId &, const Domain::OperationContext &) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::success(authority_);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority &, const std::vector<Domain::PathText> &,
        const std::vector<Domain::FileAccess> &, bool, std::uint64_t,
        const Domain::OperationContext &) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::Unauthorized, "diagnostics test authority cannot be narrowed"));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority &authority, const Domain::PathAuthorizationRequest &request,
        const Domain::OperationContext &) noexcept override
    {
        ++authorizationCount_;
        if (denyAll_)
        {
            return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "diagnostic export authority was denied by the test seam"));
        }
        const auto &requested = request.requestedPath.value();
        const auto &root = root_.value();
        const bool inside = requested == root || (requested.size() > root.size() && requested.starts_with(root) &&
                                                  (root.ends_with('\\') || root.ends_with('/') ||
                                                   requested[root.size()] == '\\' || requested[root.size()] == '/'));
        if (!request.basePath.has_value() || request.basePath.value() != root_ || !inside ||
            authority.authorityId() != authority_.authorityId())
        {
            return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized, "diagnostic export escaped the test authority root"));
        }
        return issueAuthorizedPath(authority, request.requestedPath, root_, request.access);
    }

  private:
    Domain::PathText root_;
    Contracts::WorkspaceAuthority authority_;
    bool denyAll_{};
    std::size_t authorizationCount_{};
};

class BlockingRedactorState final
{
  public:
    [[nodiscard]] bool waitUntilEntered(const std::chrono::milliseconds timeout) noexcept
    {
        try
        {
            std::unique_lock lock{mutex_};
            return condition_.wait_for(lock, timeout, [this] { return entered_; });
        }
        catch (...)
        {
            return false;
        }
    }

    void blockUntilReleased() noexcept
    {
        try
        {
            std::unique_lock lock{mutex_};
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return released_; });
        }
        catch (...)
        {
        }
    }

    void release() noexcept
    {
        try
        {
            {
                std::scoped_lock lock{mutex_};
                released_ = true;
            }
            condition_.notify_all();
        }
        catch (...)
        {
        }
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

class BlockingRedactor final : public Contracts::IRedactor
{
  public:
    explicit BlockingRedactor(std::shared_ptr<BlockingRedactorState> state) noexcept : state_{std::move(state)}
    {
    }

    [[nodiscard]] Domain::Result<std::string> redact(const std::string_view value) noexcept override
    {
        state_->blockUntilReleased();
        return Domain::Result<std::string>::success(std::string{value});
    }

  private:
    std::shared_ptr<BlockingRedactorState> state_;
};

class ReleaseBlockingRedactor final
{
  public:
    explicit ReleaseBlockingRedactor(std::shared_ptr<BlockingRedactorState> state) noexcept : state_{std::move(state)}
    {
    }

    ~ReleaseBlockingRedactor() noexcept
    {
        state_->release();
    }

  private:
    std::shared_ptr<BlockingRedactorState> state_;
};

struct DiagnosticFixture final
{
    TemporaryDirectory directory;
    std::filesystem::path logRoot{directory.path() / L"logs"};
    std::filesystem::path exportRoot{directory.path() / L"exports"};
    Domain::PathText logRootText{pathText(logRoot)};
    Domain::PathText exportRootText{pathText(exportRoot)};
    std::shared_ptr<FixedClock> clockOwner{std::make_shared<FixedClock>()};
    FixedClock &clock{*clockOwner};
    std::shared_ptr<SecretRedactor> redactorOwner{std::make_shared<SecretRedactor>()};
    SecretRedactor &redactor{*redactorOwner};
    std::shared_ptr<BCryptSha256Hasher> hasherOwner{std::make_shared<BCryptSha256Hasher>()};
    BCryptSha256Hasher &hasher{*hasherOwner};
    std::shared_ptr<ExportAuthority> authorityOwner{std::make_shared<ExportAuthority>(exportRootText)};
    ExportAuthority &authority{*authorityOwner};
    std::shared_ptr<WindowsAtomicFileStore> atomicStoreOwner{std::make_shared<WindowsAtomicFileStore>()};
    WindowsAtomicFileStore &atomicStore{*atomicStoreOwner};

    DiagnosticFixture()
    {
        std::error_code error;
        require(std::filesystem::create_directories(logRoot, error) && !error,
                "diagnostic log directory must be created");
        error.clear();
        require(std::filesystem::create_directories(exportRoot, error) && !error,
                "diagnostic export directory must be created");
    }

    [[nodiscard]] WindowsDiagnosticSinkOptions options(
        Domain::ResourceBudgets budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB)) const
    {
        return WindowsDiagnosticSinkOptions{logRootText, exportRootText, std::move(budgets), false};
    }
};

[[nodiscard]] Domain::DiagnosticEnvelope diagnostic(std::string event, const Domain::UtcTimePoint timestamp,
                                                    std::vector<Domain::DiagnosticField> fields = {})
{
    return Domain::DiagnosticEnvelope{
        timestamp,        std::move(event),        Domain::DiagnosticSeverity::Info,
        "test-role",      ::GetCurrentProcessId(), Domain::DiagnosticCategory::Diagnostics,
        std::move(fields)};
}

[[nodiscard]] std::wstring environmentValue(const wchar_t *name);

[[nodiscard]] Domain::ResourceBudgets rotationStressBudgets()
{
    auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB);
    budgets.diagnosticLogFilesMaximum = 2U;
    budgets.diagnosticLogFileBytesMaximum = static_cast<std::size_t>(RotationStressBytes);
    return budgets;
}

[[nodiscard]] bool diagnosticCrashChildRequested()
{
    return environmentValue(DiagnosticCrashChildVariable) == L"1";
}

void diagnosticRotationCrashChild();

[[nodiscard]] std::string readText(const std::filesystem::path &path)
{
    std::ifstream stream{path, std::ios::binary};
    require(stream.is_open(), "test artifact must open");
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void createSizedFile(const std::filesystem::path &path, const std::uint64_t size)
{
    WindowsDetail::UniqueHandle handle{::CreateFileW(path.c_str(), GENERIC_WRITE,
                                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(static_cast<bool>(handle), "bounded-copy fixture file must be created");
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    require(::SetFilePointerEx(handle.get(), end, nullptr, FILE_BEGIN) != FALSE,
            "bounded-copy fixture size must be positioned");
    require(::SetEndOfFile(handle.get()) != FALSE, "bounded-copy fixture size must be committed");
    require(::FlushFileBuffers(handle.get()) != FALSE, "bounded-copy fixture size must be flushed");
}

struct NativeDiagnosticCallResult final
{
    bool succeeded{};
    DWORD errorCode{ERROR_SUCCESS};
};

[[nodiscard]] NativeDiagnosticCallResult nativeHardLinkSameDirectory(const HANDLE source,
                                                                     const std::wstring_view destinationName) noexcept
{
    try
    {
        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
        auto *const information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        std::memset(information, 0, informationBytes);
        information->ReplaceIfExists = FALSE;
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';

        struct NativeIoStatusBlock final
        {
            union {
                LONG status;
                void *pointer;
            } result{};
            ULONG_PTR information{};
        } ioStatus{};
        using NtSetInformationFileFunction = LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG, ULONG);
        using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto ntSetInformationFile =
            ntdll == nullptr
                ? nullptr
                : reinterpret_cast<NtSetInformationFileFunction>(::GetProcAddress(ntdll, "NtSetInformationFile"));
        const auto rtlNtStatusToDosError =
            ntdll == nullptr
                ? nullptr
                : reinterpret_cast<RtlNtStatusToDosErrorFunction>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
        {
            return {false, ERROR_PROC_NOT_FOUND};
        }
        constexpr ULONG NativeFileLinkInformation = 11U;
        const LONG status = ntSetInformationFile(source, &ioStatus, information, static_cast<ULONG>(informationBytes),
                                                 NativeFileLinkInformation);
        if (status >= 0)
        {
            return {true, ERROR_SUCCESS};
        }
        return {false, static_cast<DWORD>(rtlNtStatusToDosError(status))};
    }
    catch (...)
    {
        return {false, ERROR_NOT_ENOUGH_MEMORY};
    }
}

[[nodiscard]] std::optional<std::filesystem::path> findDiagnosticRotationTemporary(const std::filesystem::path &root)
{
    for (const auto &entry : std::filesystem::directory_iterator{root})
    {
        if (entry.path().filename().wstring().starts_with(DiagnosticRotationTemporaryPrefix))
        {
            return entry.path();
        }
    }
    return std::nullopt;
}

void requireNoDiagnosticRotationTemporaries(const std::filesystem::path &root)
{
    require(!findDiagnosticRotationTemporary(root).has_value(), "diagnostic rotation leaked a temporary file");
}

[[nodiscard]] std::wstring environmentValue(const wchar_t *const name)
{
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U)
    {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(written > 0U && written < buffer.size(), "diagnostic crash-child environment value must be bounded");
    return std::wstring{buffer.data(), static_cast<std::size_t>(written)};
}

enum class DiagnosticRotationCheckpoint
{
    StagedFileCreation,
    BeforeStagedFileValidation,
};

class BlockingDiagnosticRotationObserver final : public WindowsDetail::IDiagnosticRotationPublishObserver
{
  public:
    explicit BlockingDiagnosticRotationObserver(const DiagnosticRotationCheckpoint checkpoint)
        : checkpoint_{checkpoint}, checkpointReached_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          allowCheckpoint_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)}
    {
        require(static_cast<bool>(checkpointReached_) && static_cast<bool>(allowCheckpoint_),
                "diagnostic rotation checkpoint events must be created");
    }

    void afterStagedFileCreation(const std::wstring_view stagedPath) noexcept override
    {
        if (checkpoint_ == DiagnosticRotationCheckpoint::StagedFileCreation)
        {
            reachCheckpoint(stagedPath);
        }
    }

    void beforeStagedFileValidation(const std::wstring_view stagedPath) noexcept override
    {
        if (checkpoint_ == DiagnosticRotationCheckpoint::BeforeStagedFileValidation)
        {
            reachCheckpoint(stagedPath);
        }
    }

    [[nodiscard]] bool waitUntilCheckpoint() const noexcept
    {
        return ::WaitForSingleObject(checkpointReached_.get(), 15'000U) == WAIT_OBJECT_0;
    }

    [[nodiscard]] std::filesystem::path stagedPath() const
    {
        return std::filesystem::path{std::wstring{stagedPath_.data(), stagedPathLength_}};
    }

    void allowCheckpoint() noexcept
    {
        static_cast<void>(::SetEvent(allowCheckpoint_.get()));
    }

    [[nodiscard]] bool callbackFailed() const noexcept
    {
        return callbackFailed_.load(std::memory_order_acquire);
    }

  private:
    void reachCheckpoint(const std::wstring_view stagedPath) noexcept
    {
        if (stagedPath.size() >= stagedPath_.size())
        {
            callbackFailed_.store(true, std::memory_order_release);
        }
        else
        {
            std::copy(stagedPath.begin(), stagedPath.end(), stagedPath_.begin());
            stagedPathLength_ = stagedPath.size();
        }
        if (::SetEvent(checkpointReached_.get()) == FALSE ||
            ::WaitForSingleObject(allowCheckpoint_.get(), 30'000U) != WAIT_OBJECT_0)
        {
            callbackFailed_.store(true, std::memory_order_release);
        }
    }

    DiagnosticRotationCheckpoint checkpoint_;
    WindowsDetail::UniqueHandle checkpointReached_;
    WindowsDetail::UniqueHandle allowCheckpoint_;
    std::array<wchar_t, 32U * 1024U> stagedPath_{};
    std::size_t stagedPathLength_{};
    std::atomic_bool callbackFailed_{};
};

class DiagnosticRotationCheckpointReleaseGuard final
{
  public:
    explicit DiagnosticRotationCheckpointReleaseGuard(BlockingDiagnosticRotationObserver &observer) noexcept
        : observer_{observer}
    {
    }

    ~DiagnosticRotationCheckpointReleaseGuard() noexcept
    {
        observer_.allowCheckpoint();
    }

    DiagnosticRotationCheckpointReleaseGuard(const DiagnosticRotationCheckpointReleaseGuard &) = delete;
    DiagnosticRotationCheckpointReleaseGuard &operator=(const DiagnosticRotationCheckpointReleaseGuard &) = delete;
    DiagnosticRotationCheckpointReleaseGuard(DiagnosticRotationCheckpointReleaseGuard &&) = delete;
    DiagnosticRotationCheckpointReleaseGuard &operator=(DiagnosticRotationCheckpointReleaseGuard &&) = delete;

  private:
    BlockingDiagnosticRotationObserver &observer_;
};

void diagnosticRotationCrashChild()
{
    const std::wstring rootValue = environmentValue(DiagnosticCrashRootVariable);
    require(!rootValue.empty(), "diagnostic crash child requires its retained log root");
    const std::filesystem::path logRoot{rootValue};
    const std::filesystem::path exportRoot = logRoot.parent_path() / L"exports";
    auto clock = std::make_shared<FixedClock>();
    auto redactor = std::make_shared<SecretRedactor>();
    auto hasher = std::make_shared<BCryptSha256Hasher>();
    const auto exportRootText = pathText(exportRoot);
    auto authority = std::make_shared<ExportAuthority>(exportRootText);
    auto atomicStore = std::make_shared<WindowsAtomicFileStore>();
    auto observer = std::make_shared<BlockingDiagnosticRotationObserver>(
        DiagnosticRotationCheckpoint::StagedFileCreation);
    const WindowsDiagnosticSinkOptions options{pathText(logRoot), exportRootText, rotationStressBudgets(), false};
    auto sink = WindowsDetail::WindowsDiagnosticSinkTestAccess::create(options, clock, redactor, hasher, authority,
                                                                       atomicStore, observer);
    const auto result = sink->record(diagnostic("crash-child", clock->utc()), liveContext(*clock));
    require(static_cast<bool>(result) && !observer->callbackFailed(),
            result ? "diagnostic crash child must remain at its bounded cooperative checkpoint"
                   : "diagnostic crash child failed before its termination boundary");
}

[[nodiscard]] std::vector<std::byte> bytesFor(const std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(),
                   [](const char value) noexcept { return static_cast<std::byte>(static_cast<unsigned char>(value)); });
    return bytes;
}

[[nodiscard]] std::vector<std::filesystem::path> diagnosticLogs(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator{root})
    {
        if (entry.is_regular_file() && entry.path().filename().wstring().starts_with(L"forge-diagnostics.jsonl"))
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

[[nodiscard]] std::string allDiagnosticText(const std::filesystem::path &root)
{
    std::string combined;
    for (const auto &path : diagnosticLogs(root))
    {
        combined += readText(path);
    }
    return combined;
}

void requireAbsent(const std::string_view content, const std::vector<std::string> &canaries,
                   const std::string_view surface)
{
    for (const auto &canary : canaries)
    {
        require(content.find(canary) == std::string_view::npos, std::string{surface} + " leaked a diagnostic canary");
    }
}

void activeFacadeDestructionRetainsOwnedDependencies()
{
    DiagnosticFixture fixture;
    auto options = fixture.options();
    auto clock = std::move(fixture.clockOwner);
    auto hasher = std::move(fixture.hasherOwner);
    auto authority = std::move(fixture.authorityOwner);
    auto atomicStore = std::move(fixture.atomicStoreOwner);
    auto blockingState = std::make_shared<BlockingRedactorState>();
    auto redactor = std::make_shared<BlockingRedactor>(blockingState);

    std::weak_ptr<FixedClock> clockLifetime{clock};
    std::weak_ptr<BlockingRedactor> redactorLifetime{redactor};
    std::weak_ptr<BCryptSha256Hasher> hasherLifetime{hasher};
    std::weak_ptr<ExportAuthority> authorityLifetime{authority};
    std::weak_ptr<WindowsAtomicFileStore> atomicStoreLifetime{atomicStore};

    const auto context = liveContext(*clock);
    const auto event = diagnostic("active-destruction", clock->utc());
    auto sink =
        std::make_unique<WindowsDiagnosticSink>(std::move(options), clock, redactor, hasher, authority, atomicStore);
    auto *const activeSink = sink.get();
    std::optional<Domain::Result<void>> outcome;
    std::jthread running{
        [activeSink, &outcome, event, context] { outcome.emplace(activeSink->record(event, context)); }};
    ReleaseBlockingRedactor releaseOnExit{blockingState};

    require(blockingState->waitUntilEntered(std::chrono::seconds{5}),
            "diagnostic record must enter the blocking owned dependency");

    clock.reset();
    redactor.reset();
    hasher.reset();
    authority.reset();
    atomicStore.reset();

    const auto destroyStarted = std::chrono::steady_clock::now();
    sink.reset();
    const auto destroyElapsed = std::chrono::steady_clock::now() - destroyStarted;
    require(destroyElapsed < std::chrono::seconds{5}, "diagnostic facade destruction must remain bounded");
    require(!clockLifetime.expired() && !redactorLifetime.expired() && !hasherLifetime.expired() &&
                !authorityLifetime.expired() && !atomicStoreLifetime.expired(),
            "active diagnostic call must retain every injected dependency");

    blockingState->release();
    running.join();
    require(outcome.has_value(), "active diagnostic call must return after release");
    require(clockLifetime.expired() && redactorLifetime.expired() && hasherLifetime.expired() &&
                authorityLifetime.expired() && atomicStoreLifetime.expired(),
            "diagnostic dependencies must release with the final Impl owner");
}

void diagnosticDirectoryCreationPinsEachParent()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / L"owned";
    const auto root = parent / L"diagnostics";
    const auto renamed = directory.path() / L"renamed-owned";
    std::error_code error;
    require(std::filesystem::create_directory(parent, error) && !error, "diagnostic parent fixture must be created");

    {
        WindowsDetail::UniqueHandle reparseWriter{
            ::CreateFileW(parent.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(reparseWriter), "diagnostic parent fixture must be writable before anchoring");
    }
    require(::MoveFileExW(parent.c_str(), renamed.c_str(), 0U) != FALSE,
            "diagnostic parent fixture must be renameable before anchoring");
    require(::MoveFileExW(renamed.c_str(), parent.c_str(), 0U) != FALSE,
            "diagnostic parent fixture must restore before anchoring");
    WindowsDetail::UniqueHandle shutdownEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(static_cast<bool>(shutdownEvent), "diagnostic shutdown event fixture must be created");
    DirectoryRenameObserver observer{parent, renamed};
    FixedClock clock;

    auto anchored = WindowsDetail::prepareAnchoredDiagnosticDirectory(pathText(root), liveContext(clock),
                                                                      shutdownEvent.get(), &observer);

    require(observer.observed(), "directory creation must anchor the parent before the child");
    require(!observer.reparseWriterOpened(), "an anchored diagnostic parent must reject in-place reparse metadata "
                                             "writes");
    require(observer.reparseWriterError() == ERROR_SHARING_VIOLATION ||
                observer.reparseWriterError() == ERROR_ACCESS_DENIED,
            "diagnostic parent writes must fail because its no-write handle is "
            "retained");
    require(!observer.renameSucceeded(), "an anchored diagnostic parent must reject replacement during child "
                                         "creation");
    require(observer.renameError() == ERROR_SHARING_VIOLATION || observer.renameError() == ERROR_ACCESS_DENIED,
            "diagnostic parent replacement must fail because its no-delete "
            "handle is retained");
    require(!observer.restoreSucceeded(), "blocked parent replacement must not require restoration");
    require(static_cast<bool>(anchored), "anchored diagnostic directory creation must succeed");
    require(anchored.value().handles.size() >= 2U, "diagnostic directory creation must retain its ancestor handles");
    require(std::filesystem::is_directory(root), "diagnostic directory creation must create the requested child");
    require(!std::filesystem::exists(renamed), "diagnostic directory creation must not move the anchored parent");

    WindowsDetail::UniqueHandle retainedRootWriter{::CreateFileW(
        root.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(!retainedRootWriter, "the returned diagnostic root anchor must "
                                 "continue blocking reparse writes");
    const DWORD retainedRootWriteError = ::GetLastError();
    require(retainedRootWriteError == ERROR_SHARING_VIOLATION || retainedRootWriteError == ERROR_ACCESS_DENIED,
            "the retained diagnostic root must deny a write-capable reparse handle");

    const auto renamedRoot = directory.path() / L"renamed-diagnostics";
    require(::MoveFileExW(root.c_str(), renamedRoot.c_str(), 0U) == FALSE,
            "the returned diagnostic root anchor must continue blocking replacement");
    const DWORD retainedRootRenameError = ::GetLastError();
    require(retainedRootRenameError == ERROR_SHARING_VIOLATION || retainedRootRenameError == ERROR_ACCESS_DENIED,
            "the retained diagnostic root must deny rename/delete access");
}

void relativeFileOperationsStayBoundToStrongParent()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / L"relative-parent";
    const auto renamedParent = directory.path() / L"renamed-relative-parent";
    const auto junctionTarget = directory.path() / L"junction-target";
    const auto junction = parent / L"redirect";
    std::error_code error;
    require(std::filesystem::create_directory(parent, error) && !error,
            "relative-operation parent fixture must be created");
    error.clear();
    require(std::filesystem::create_directory(junctionTarget, error) && !error,
            "relative-operation junction target must be created");
    createDirectoryJunction(junction, junctionTarget);

    WindowsDetail::UniqueHandle parentAnchor{
        ::CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE, FILE_SHARE_READ,
                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(parentAnchor), "relative-operation parent must open with traverse-only strong "
                                             "authority");

    WindowsDetail::UniqueHandle competingWriter{::CreateFileW(
        parent.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(!competingWriter, "strong parent anchor must reject a competing reparse writer");
    const DWORD writerError = ::GetLastError();
    require(writerError == ERROR_SHARING_VIOLATION || writerError == ERROR_ACCESS_DENIED,
            "competing parent writer must fail closed");
    require(::MoveFileExW(parent.c_str(), renamedParent.c_str(), 0U) == FALSE,
            "strong parent anchor must reject full-path rename");
    const DWORD renameError = ::GetLastError();
    require(renameError == ERROR_SHARING_VIOLATION || renameError == ERROR_ACCESS_DENIED,
            "full-path parent rename must fail closed");
    require(::RemoveDirectoryW(parent.c_str()) == FALSE, "strong parent anchor must reject full-path delete");
    const DWORD deleteError = ::GetLastError();
    require(deleteError == ERROR_SHARING_VIOLATION || deleteError == ERROR_ACCESS_DENIED,
            "full-path parent delete must fail closed");

    WindowsDetail::RelativeOpenOptions fileOptions{};
    fileOptions.desiredAccess = GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES;
    fileOptions.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    fileOptions.disposition = WindowsDetail::RelativeOpenDisposition::OpenExisting;
    fileOptions.objectType = WindowsDetail::RelativeObjectType::File;
    fileOptions.writeThrough = true;

    require(!WindowsDetail::openRelative(parentAnchor.get(), L"..", fileOptions),
            "relative open must reject parent traversal");
    require(!WindowsDetail::openRelative(parentAnchor.get(), L"NUL.txt", fileOptions),
            "relative open must reject DOS device basenames");
    require(!WindowsDetail::openRelative(parentAnchor.get(), L"NUL .txt", fileOptions) &&
                !WindowsDetail::openRelative(parentAnchor.get(), L"COM\u00b9.log", fileOptions),
            "relative open must reject normalized and superscript DOS device "
            "basenames");
    auto invalidOptions = fileOptions;
    invalidOptions.objectType = static_cast<WindowsDetail::RelativeObjectType>(0xffU);
    const auto invalid = WindowsDetail::openRelative(parentAnchor.get(), L"invalid-options.tmp", invalidOptions);
    require(!invalid && invalid.win32Error == ERROR_INVALID_PARAMETER,
            "relative open must reject invalid option combinations");

    const auto missing = WindowsDetail::openRelative(parentAnchor.get(), L"missing.tmp", fileOptions);
    require(!missing && (missing.win32Error == ERROR_FILE_NOT_FOUND || missing.win32Error == ERROR_PATH_NOT_FOUND) &&
                missing.nativeStatus != 0,
            "relative OpenExisting must report a missing file");

    fileOptions.disposition = WindowsDetail::RelativeOpenDisposition::CreateNew;
    auto created = WindowsDetail::openRelative(parentAnchor.get(), L"created.tmp", fileOptions);
    require(static_cast<bool>(created) && created.wasCreated(),
            "relative CreateNew must create beneath the anchored parent");
    constexpr std::string_view payload{"relative-write"};
    DWORD written{};
    require(::WriteFile(created.handle.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) !=
                    FALSE &&
                written == payload.size(),
            "relative-created file must support legitimate bounded writes");
    const auto collision = WindowsDetail::openRelative(parentAnchor.get(), L"created.tmp", fileOptions);
    require(!collision && (collision.win32Error == ERROR_FILE_EXISTS || collision.win32Error == ERROR_ALREADY_EXISTS) &&
                collision.nativeStatus != 0,
            "relative CreateNew must report a deterministic collision");

    fileOptions.disposition = WindowsDetail::RelativeOpenDisposition::OpenOrCreate;
    auto openedOrCreated = WindowsDetail::openRelative(parentAnchor.get(), L"open-if.tmp", fileOptions);
    require(static_cast<bool>(openedOrCreated) && openedOrCreated.wasCreated(),
            "relative OpenOrCreate must report creation");
    openedOrCreated.handle.reset();
    openedOrCreated = WindowsDetail::openRelative(parentAnchor.get(), L"open-if.tmp", fileOptions);
    require(static_cast<bool>(openedOrCreated) && openedOrCreated.wasOpened(),
            "relative OpenOrCreate must report opening an existing file");

    WindowsDetail::RelativeOpenOptions directoryOptions{};
    directoryOptions.desiredAccess = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE;
    directoryOptions.shareAccess = FILE_SHARE_READ;
    directoryOptions.disposition = WindowsDetail::RelativeOpenDisposition::OpenOrCreate;
    directoryOptions.objectType = WindowsDetail::RelativeObjectType::Directory;
    auto reparseOptions = directoryOptions;
    reparseOptions.disposition = WindowsDetail::RelativeOpenDisposition::OpenExisting;
    const auto openedReparse = WindowsDetail::openRelative(parentAnchor.get(), L"redirect", reparseOptions);
    require(static_cast<bool>(openedReparse), "relative directory open must return the final reparse object itself");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    require(
        ::GetFileInformationByHandleEx(openedReparse.handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE &&
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U && tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT,
        "relative directory open must not follow a final-component junction");

    WindowsDetail::UniqueHandle shutdownEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    require(static_cast<bool>(shutdownEvent), "relative-operation verifier shutdown event must be created");
    FixedClock clock;
    const auto rejectedJunction =
        WindowsDetail::prepareAnchoredDiagnosticDirectory(pathText(junction), liveContext(clock), shutdownEvent.get());
    require(!rejectedJunction, "the diagnostic caller-side verifier must reject "
                               "a final-component junction");

    const auto createdDirectory = WindowsDetail::openRelative(parentAnchor.get(), L"child-directory", directoryOptions);
    require(static_cast<bool>(createdDirectory) && createdDirectory.wasCreated(),
            "relative directory creation must succeed under a strong parent anchor");
}

void diagnosticLeafHardLinksFailClosed()
{
    DiagnosticFixture fixture;
    const auto canary = fixture.directory.path() / L"outside-canary.jsonl";
    const auto diagnosticLink = fixture.logRoot / L"forge-diagnostics.jsonl";
    {
        std::ofstream stream{canary, std::ios::binary | std::ios::trunc};
        require(stream.is_open(), "hard-link canary must open");
        stream << "outside-canary";
    }
    require(::CreateHardLinkW(diagnosticLink.c_str(), canary.c_str(), nullptr) != FALSE,
            "diagnostic hard-link fixture must be created");

    WindowsDiagnosticSink sink{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                               fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};
    const auto rejected =
        sink.record(diagnostic("hard-link-rejected", fixture.clock.utc()), liveContext(fixture.clock));
    requireError(rejected, Domain::ErrorCodes::PathOutsideAuthority,
                 "a multiply linked diagnostic leaf must fail closed");
    require(readText(canary) == "outside-canary", "diagnostic hard-link rejection must not mutate the outside canary");
}

void caseSensitiveDiagnosticDirectoriesFailClosed()
{
    TemporaryDirectory directory;
    const auto logRoot = directory.path() / L"case-sensitive-logs";
    const auto exportRoot = directory.path() / L"exports";
    std::error_code error;
    require(std::filesystem::create_directory(logRoot, error) && !error,
            "case-sensitive diagnostic root fixture must be created");
    error.clear();
    require(std::filesystem::create_directory(exportRoot, error) && !error,
            "case-sensitive diagnostic export fixture must be created");

    WindowsDetail::UniqueHandle directoryHandle{
        ::CreateFileW(logRoot.c_str(),
                      FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                          FILE_ADD_SUBDIRECTORY | DELETE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(directoryHandle), "case-sensitive diagnostic root fixture must open for attribute "
                                                "inspection");

    FILE_CASE_SENSITIVE_INFO initial{};
    require(::GetFileInformationByHandleEx(directoryHandle.get(), FileCaseSensitiveInfo, &initial, sizeof(initial)) !=
                FALSE,
            "Windows 11 must expose per-directory case-sensitivity information");
    require((initial.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) == 0U,
            "fresh diagnostics test directory must begin case-insensitive");

    FILE_CASE_SENSITIVE_INFO enabled{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    if (::SetFileInformationByHandle(directoryHandle.get(), FileCaseSensitiveInfo, &enabled, sizeof(enabled)) == FALSE)
    {
        const DWORD nativeError = ::GetLastError();
        require(nativeError == ERROR_ACCESS_DENIED || nativeError == ERROR_PRIVILEGE_NOT_HELD ||
                    nativeError == ERROR_NOT_SUPPORTED || nativeError == ERROR_INVALID_PARAMETER,
                "case-sensitivity capability probe failed unexpectedly");
        const auto deterministic =
            WindowsDetail::validateDiagnosticDirectoryCaseSensitivity(FILE_CS_FLAG_CASE_SENSITIVE_DIR);
        requireError(deterministic, Domain::ErrorCodes::PathOutsideAuthority,
                     "the deterministic case-sensitive-directory seam must fail closed");
        std::cout << "[INFO] case-sensitive diagnostics integration requires an "
                     "elevated NTFS "
                     "fixture; the Windows query and deterministic rejection seam "
                     "passed.\n";
        return;
    }
    directoryHandle.reset();

    const auto lowerTwin = logRoot / L"forge-diagnostics.jsonl";
    const auto upperTwin = logRoot / L"FORGE-DIAGNOSTICS.JSONL";
    WindowsDetail::UniqueHandle lower{
        ::CreateFileW(lowerTwin.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS, nullptr)};
    WindowsDetail::UniqueHandle upper{
        ::CreateFileW(upperTwin.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS, nullptr)};
    require(static_cast<bool>(lower) && static_cast<bool>(upper),
            "case-sensitive directory must retain case-twin diagnostic leaves");
    lower.reset();
    upper.reset();

    auto clock = std::make_shared<FixedClock>();
    auto redactor = std::make_shared<SecretRedactor>();
    auto hasher = std::make_shared<BCryptSha256Hasher>();
    const auto exportRootText = pathText(exportRoot);
    auto authority = std::make_shared<ExportAuthority>(exportRootText);
    auto atomicStore = std::make_shared<WindowsAtomicFileStore>();
    const WindowsDiagnosticSinkOptions options{
        pathText(logRoot), exportRootText, Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB), false};
    WindowsDiagnosticSink sink{options, clock, redactor, hasher, authority, atomicStore};
    const auto rejected = sink.record(diagnostic("case-twin-rejected", clock->utc()), liveContext(*clock));
    requireError(rejected, Domain::ErrorCodes::PathOutsideAuthority,
                 "case-sensitive diagnostic directory must be rejected before "
                 "child lookup");
}

void finalDiagnosticRootStaysProtectedDuringLogTransaction()
{
    DiagnosticFixture fixture;
    WindowsDiagnosticSink sink{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                               fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};
    HeldDiagnosticFileLock heldLock{fixture.logRoot};
    std::atomic_bool entered{};
    std::optional<Domain::Result<void>> outcome;
    std::jthread writer{[&]() {
        entered.store(true, std::memory_order_release);
        outcome.emplace(sink.record(diagnostic("protected-root", fixture.clock.utc()), liveContext(fixture.clock)));
    }};

    const auto entryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < entryDeadline)
    {
        std::this_thread::yield();
    }
    require(entered.load(std::memory_order_acquire),
            "diagnostic root protection fixture did not enter its transaction");

    bool rootWriteBlocked{};
    DWORD rootWriteError{};
    const auto protectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!rootWriteBlocked && std::chrono::steady_clock::now() < protectionDeadline)
    {
        WindowsDetail::UniqueHandle competingWriter{
            ::CreateFileW(fixture.logRoot.c_str(), FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!competingWriter)
        {
            rootWriteError = ::GetLastError();
            rootWriteBlocked = rootWriteError == ERROR_SHARING_VIOLATION || rootWriteError == ERROR_ACCESS_DENIED;
        }
        if (!rootWriteBlocked)
        {
            std::this_thread::yield();
        }
    }
    require(rootWriteBlocked, "an active diagnostic transaction must retain a no-write root anchor");

    const auto renamedRoot = fixture.directory.path() / L"renamed-active-logs";
    require(::MoveFileExW(fixture.logRoot.c_str(), renamedRoot.c_str(), 0U) == FALSE,
            "an active diagnostic transaction must reject root replacement");
    const DWORD renameError = ::GetLastError();
    require(renameError == ERROR_SHARING_VIOLATION || renameError == ERROR_ACCESS_DENIED,
            "active diagnostic root replacement must fail closed");

    heldLock.release();
    writer.join();
    require(outcome.has_value() && static_cast<bool>(outcome.value()),
            "the protected diagnostic transaction must complete after its lock "
            "is released");
    require(std::filesystem::exists(fixture.logRoot / L"forge-diagnostics.jsonl"),
            "the protected root must still permit legitimate relative log creation");
    require(allDiagnosticText(fixture.logRoot).find("protected-root") != std::string::npos,
            "the protected root must still permit legitimate bounded log writes");
    sink.shutdown();
}

void redactionPrecedesEveryDiagnosticSurface()
{
    DiagnosticFixture fixture;
    WindowsDiagnosticSink sink{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                               fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};

    const std::vector<std::string> canaries{
        "COOKIE_CANARY_4317",  "AUTH_CANARY_4317",          "BEARER_CANARY_4317", "PRIVATE_KEY_CANARY_4317",
        "API_KEY_CANARY_4317", "CLIENT_SECRET_CANARY_4317", "PROMPT_CANARY_4317", "PATH_CANARY_4317"};
    const auto event = diagnostic("redaction-check", fixture.clock.utc(),
                                  {{"session_cookie", canaries[0]},
                                   {"auth", canaries[1]},
                                   {"bearer", canaries[2]},
                                   {"private-key", canaries[3]},
                                   {"api_key", canaries[4]},
                                   {"client-secret", canaries[5]},
                                   {"prompt", canaries[6]},
                                   {"workspace", "C:\\Users\\PATH_CANARY_4317\\private.txt"}});
    require(static_cast<bool>(sink.record(event, liveContext(fixture.clock))), "redacted diagnostic must persist");

    const auto recent = take(sink.recent(1U, liveContext(fixture.clock)));
    require(recent.size() == 1U, "recent ring must contain the event");
    std::string recentValues;
    for (const auto &field : recent.front().fields)
    {
        recentValues += field.value;
    }
    requireAbsent(recentValues, canaries, "recent ring");
    requireAbsent(allDiagnosticText(fixture.logRoot), canaries, "JSONL");

    const auto exported = take(sink.exportData(Domain::DiagnosticExportRequest{std::nullopt, "redaction_bundle"},
                                               fixture.authority.authority(), liveContext(fixture.clock)));
    const std::filesystem::path jsonPath{utf16(exported.artifact.value())};
    require(jsonPath.parent_path() == fixture.exportRoot,
            "default diagnostic export must use the distinct exports root");
    require(exported.markdownArtifact.has_value(), "diagnostic export must include Markdown");
    const std::filesystem::path markdownPath{utf16(exported.markdownArtifact->value())};
    const auto json = readText(jsonPath);
    const auto markdown = readText(markdownPath);
    requireAbsent(json, canaries, "JSON export");
    requireAbsent(markdown, canaries, "Markdown export");
    require(take(fixture.hasher.sha256(bytesFor(json))) == exported.checksum,
            "JSON export checksum must cover exact bytes");
    require(exported.markdownChecksum.has_value(), "Markdown export must have a checksum");
    require(take(fixture.hasher.sha256(bytesFor(markdown))) == exported.markdownChecksum.value(),
            "Markdown export checksum must cover exact bytes");
}

void exportLoadsDurableRecordsAndUsesCreateThenWrite()
{
    DiagnosticFixture fixture;
    {
        WindowsDiagnosticSink first{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                                    fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};
        require(static_cast<bool>(first.record(diagnostic("later-event", fixture.clock.utc() + std::chrono::seconds{2}),
                                               liveContext(fixture.clock))),
                "first durable diagnostic must persist");
        first.shutdown();
    }

    WindowsDiagnosticSink second{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                                 fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};
    require(static_cast<bool>(second.record(diagnostic("earlier-event", fixture.clock.utc() + std::chrono::seconds{1}),
                                            liveContext(fixture.clock))),
            "second durable diagnostic must persist");

    const Domain::DiagnosticExportRequest request{std::nullopt, "restart_bundle"};
    const auto created = take(second.exportData(request, fixture.authority.authority(), liveContext(fixture.clock)));
    require(created.recordCount == 2U, "restart export must merge durable and local records without duplicates");
    const auto document = Json::parse(readText(std::filesystem::path{utf16(created.artifact.value())}));
    require(document.at("record_count").get<std::size_t>() == 2U, "export document count must match its records");
    require(document.at("records").at(0).at("event").get<std::string>() == "earlier-event" &&
                document.at("records").at(1).at("event").get<std::string>() == "later-event",
            "durable export must use deterministic timestamp ordering");

    const auto replaced = take(second.exportData(request, fixture.authority.authority(), liveContext(fixture.clock)));
    require(replaced.recordCount == created.recordCount && replaced.checksum == created.checksum,
            "second export must replace through Write authority deterministically");
}

void concurrentSinksRespectRotationAndDownshiftCaps()
{
    DiagnosticFixture fixture;
    auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Constrained8GiB);
    budgets.diagnosticLogFilesMaximum = 3U;
    budgets.diagnosticLogFileBytesMaximum = 512U;
    WindowsDiagnosticSink first{fixture.options(budgets), fixture.clockOwner,     fixture.redactorOwner,
                                fixture.hasherOwner,      fixture.authorityOwner, fixture.atomicStoreOwner};
    WindowsDiagnosticSink second{fixture.options(budgets), fixture.clockOwner,     fixture.redactorOwner,
                                 fixture.hasherOwner,      fixture.authorityOwner, fixture.atomicStoreOwner};

    std::atomic<bool> successful{true};
    std::mutex failureMutex;
    std::string failure;
    auto writeMany = [&](WindowsDiagnosticSink &sink, const char prefix) {
        try
        {
            for (std::size_t index = 0U; index < 32U; ++index)
            {
                auto result = sink.record(diagnostic(std::string{prefix} + "-" + std::to_string(index),
                                                     fixture.clock.utc(), {{"sequence", std::to_string(index)}}),
                                          liveContext(fixture.clock));
                if (!result)
                {
                    {
                        const std::scoped_lock lock{failureMutex};
                        if (failure.empty())
                        {
                            failure = result.error().code + ": " + result.error().message;
                        }
                    }
                    successful.store(false);
                    return;
                }
            }
        }
        catch (...)
        {
            successful.store(false);
        }
    };
    std::jthread left{writeMany, std::ref(first), 'a'};
    std::jthread right{writeMany, std::ref(second), 'b'};
    left.join();
    right.join();
    if (!successful.load())
    {
        throw TestFailure{"concurrent sinks must both persist successfully: " + failure};
    }
    first.shutdown();
    second.shutdown();

    const auto logs = diagnosticLogs(fixture.logRoot);
    require(!logs.empty() && logs.size() <= budgets.diagnosticLogFilesMaximum,
            "rotation must retain no more than the configured file count");
    for (const auto &path : logs)
    {
        require(std::filesystem::file_size(path) <= budgets.diagnosticLogFileBytesMaximum,
                "cross-instance append must not exceed the active file byte cap");
        std::istringstream lines{readText(path)};
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.empty())
            {
                require(Json::parse(line).is_object(), "concurrent JSONL records must never interleave");
            }
        }
    }

    for (std::size_t generation = 3U; generation < WindowsDiagnosticSink::MaximumRetainedLogFiles; ++generation)
    {
        std::ofstream stale{fixture.logRoot / (L"forge-diagnostics.jsonl." + std::to_wstring(generation)),
                            std::ios::binary | std::ios::trunc};
        stale << "{}\n";
    }
    WindowsDiagnosticSink downshifted{fixture.options(budgets), fixture.clockOwner,     fixture.redactorOwner,
                                      fixture.hasherOwner,      fixture.authorityOwner, fixture.atomicStoreOwner};
    require(static_cast<bool>(
                downshifted.record(diagnostic("profile-downshift", fixture.clock.utc()), liveContext(fixture.clock))),
            "profile downshift append must succeed");
    downshifted.shutdown();
    for (std::size_t generation = 3U; generation < WindowsDiagnosticSink::MaximumRetainedLogFiles; ++generation)
    {
        require(!std::filesystem::exists(fixture.logRoot / (L"forge-diagnostics.jsonl." + std::to_wstring(generation))),
                "profile downshift must prune stale higher generations");
    }
}

void rotationTemporaryDeniesReadersAndCancelsCleanly()
{
    DiagnosticFixture fixture;
    const auto master = fixture.logRoot / L"forge-diagnostics.jsonl";
    const auto firstArchive = fixture.logRoot / L"forge-diagnostics.jsonl.1";
    createSizedFile(master, RotationStressBytes);

    {
        auto observer = std::make_shared<BlockingDiagnosticRotationObserver>(
            DiagnosticRotationCheckpoint::StagedFileCreation);
        auto sink = WindowsDetail::WindowsDiagnosticSinkTestAccess::create(
            fixture.options(rotationStressBudgets()), fixture.clockOwner, fixture.redactorOwner, fixture.hasherOwner,
            fixture.authorityOwner, fixture.atomicStoreOwner, observer);
        std::stop_source cancellation;
        std::optional<Domain::Result<void>> outcome;
        std::jthread writer{[&]() {
            outcome.emplace(sink->record(diagnostic("cancel-rotation-copy", fixture.clock.utc()),
                                         liveContext(fixture.clock, cancellation.get_token())));
        }};
        DiagnosticRotationCheckpointReleaseGuard releaseObserver{*observer};

        const bool reachedCreation = observer->waitUntilCheckpoint();
        require(reachedCreation, "diagnostic rotation must reach its cooperative staging-copy checkpoint");
        const std::filesystem::path temporary = observer->stagedPath();
        require(std::filesystem::exists(temporary),
                "diagnostic rotation temporary must remain named at its cooperative checkpoint");
        require(!std::filesystem::exists(firstArchive),
                "a partial rotation must not publish its canonical archive name");

        WindowsDetail::UniqueHandle externalReader{::CreateFileW(
            temporary.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        const DWORD readerError = externalReader ? ERROR_SUCCESS : ::GetLastError();
        require(!externalReader && (readerError == ERROR_SHARING_VIOLATION || readerError == ERROR_ACCESS_DENIED),
                "an in-progress diagnostic rotation temporary must deny external readers");

        cancellation.request_stop();
        observer->allowCheckpoint();
        writer.join();
        require(!observer->callbackFailed(),
                "diagnostic rotation creation observer must complete its bounded wait");
        require(outcome.has_value(), "cancelled diagnostic rotation must return a result");
        requireError(outcome.value(), Domain::ErrorCodes::Cancelled,
                     "cancellation at the rotation copy boundary must fail before publication");
        require(std::filesystem::exists(master) && std::filesystem::file_size(master) == RotationStressBytes,
                "cancelled diagnostic rotation must preserve its complete source");
        require(!std::filesystem::exists(firstArchive),
                "cancelled diagnostic rotation must not publish a canonical archive");
        requireNoDiagnosticRotationTemporaries(fixture.logRoot);
    }

    {
        auto observer = std::make_shared<BlockingDiagnosticRotationObserver>(
            DiagnosticRotationCheckpoint::BeforeStagedFileValidation);
        auto sink = WindowsDetail::WindowsDiagnosticSinkTestAccess::create(
            fixture.options(rotationStressBudgets()), fixture.clockOwner, fixture.redactorOwner, fixture.hasherOwner,
            fixture.authorityOwner, fixture.atomicStoreOwner, observer);
        std::optional<Domain::Result<void>> outcome;
        std::jthread writer{[&]() {
            outcome.emplace(
                sink->record(diagnostic("collision-rotation-copy", fixture.clock.utc()), liveContext(fixture.clock)));
        }};
        DiagnosticRotationCheckpointReleaseGuard releaseObserver{*observer};

        const bool reachedValidation = observer->waitUntilCheckpoint();
        require(reachedValidation,
                "diagnostic rotation collision test must reach its prepublication validation checkpoint");
        const std::filesystem::path temporary = observer->stagedPath();
        require(std::filesystem::exists(temporary) && !std::filesystem::exists(firstArchive),
                "diagnostic rotation collision fixture must pause before publication");

        WindowsDetail::UniqueHandle collisionRoot{::CreateFileW(
            fixture.logRoot.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        require(static_cast<bool>(collisionRoot),
                "diagnostic collision fixture must share the retained root read anchor");
        WindowsDetail::RelativeOpenOptions collisionOptions{};
        collisionOptions.desiredAccess = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        collisionOptions.shareAccess = 0U;
        collisionOptions.disposition = WindowsDetail::RelativeOpenDisposition::CreateNew;
        collisionOptions.objectType = WindowsDetail::RelativeObjectType::File;
        collisionOptions.writeThrough = true;
        auto collision =
            WindowsDetail::openRelative(collisionRoot.get(), L"forge-diagnostics.jsonl.1", collisionOptions);
        require(static_cast<bool>(collision),
                "diagnostic collision fixture must create a handle-relative destination");
        constexpr std::string_view CollisionCanary = "rotation-collision-canary";
        DWORD collisionWritten{};
        require(::WriteFile(collision.handle.get(), CollisionCanary.data(), static_cast<DWORD>(CollisionCanary.size()),
                            &collisionWritten, nullptr) != FALSE &&
                    collisionWritten == CollisionCanary.size() && ::FlushFileBuffers(collision.handle.get()) != FALSE,
                "diagnostic collision canary must be durable before publication resumes");
        collision.handle.reset();
        collisionRoot.reset();

        observer->allowCheckpoint();
        writer.join();
        require(!observer->callbackFailed(),
                "diagnostic rotation validation observer must complete its bounded wait");
        require(outcome.has_value(), "colliding diagnostic rotation must return a result");
        requireError(outcome.value(), Domain::ErrorCodes::Conflict,
                     "a newly introduced archive destination must fail closed");
        require(std::filesystem::exists(master) && std::filesystem::file_size(master) == RotationStressBytes,
                "colliding diagnostic rotation must preserve its complete source");
        require(readText(firstArchive) == CollisionCanary,
                "colliding diagnostic rotation must not replace its destination canary");
        requireNoDiagnosticRotationTemporaries(fixture.logRoot);
    }
}

void rotationStageHardLinkFailsClosedAtPublishValidation()
{
    DiagnosticFixture fixture;
    const auto master = fixture.logRoot / L"forge-diagnostics.jsonl";
    const auto firstArchive = fixture.logRoot / L"forge-diagnostics.jsonl.1";
    const auto injectedAlias = fixture.logRoot / L"diagnostic-rotation-stage-hard-link-alias.tmp";
    createSizedFile(master, RotationStressBytes);

    WindowsDetail::UniqueHandle attackerParent{
        ::CreateFileW(fixture.logRoot.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(attackerParent),
            "diagnostic hard-link fixture must retain a parent handle before strong anchoring");

    auto observer = std::make_shared<BlockingDiagnosticRotationObserver>(
        DiagnosticRotationCheckpoint::BeforeStagedFileValidation);
    auto sink = WindowsDetail::WindowsDiagnosticSinkTestAccess::create(
        fixture.options(rotationStressBudgets()), fixture.clockOwner, fixture.redactorOwner, fixture.hasherOwner,
        fixture.authorityOwner, fixture.atomicStoreOwner, observer);
    std::optional<Domain::Result<void>> outcome;
    std::jthread writer{[&]() {
        outcome.emplace(
            sink->record(diagnostic("hard-link-rotation-stage", fixture.clock.utc()), liveContext(fixture.clock)));
    }};
    DiagnosticRotationCheckpointReleaseGuard releaseObserver{*observer};

    const bool reachedValidation = observer->waitUntilCheckpoint();
    std::filesystem::path stagedPath;
    bool hardLinkCreated{};
    DWORD hardLinkError{ERROR_SUCCESS};
    if (reachedValidation)
    {
        stagedPath = observer->stagedPath();
        WindowsDetail::RelativeOpenOptions options{};
        options.desiredAccess = FILE_READ_ATTRIBUTES;
        options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        options.disposition = WindowsDetail::RelativeOpenDisposition::OpenExisting;
        options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        options.objectType = WindowsDetail::RelativeObjectType::File;
        auto staged = WindowsDetail::openRelative(attackerParent.get(), stagedPath.filename().wstring(), options);
        if (!staged)
        {
            hardLinkError = staged.win32Error;
        }
        else
        {
            const auto linked = nativeHardLinkSameDirectory(staged.handle.get(), injectedAlias.filename().wstring());
            hardLinkCreated = linked.succeeded;
            hardLinkError = linked.errorCode;
        }
    }
    observer->allowCheckpoint();
    writer.join();

    require(reachedValidation, "diagnostic rotation must pause immediately before staged-handle "
                               "publication validation");
    require(hardLinkCreated, std::string{"native FileLinkInformation must add a link through the pre-opened "
                                         "diagnostics parent, native error "} +
                                 std::to_string(hardLinkError));
    require(!observer->callbackFailed(), "diagnostic rotation publication observer must complete its bounded "
                                         "wait");
    require(outcome.has_value(), "hard-linked diagnostic rotation must return a result");
    requireError(outcome.value(), Domain::ErrorCodes::PathOutsideAuthority,
                 "a hard-linked diagnostic stage must fail its immediate "
                 "prepublication handle check");
    require(std::filesystem::exists(master) && std::filesystem::file_size(master) == RotationStressBytes,
            "hard-linked diagnostic staging must preserve its complete source");
    require(!std::filesystem::exists(firstArchive),
            "hard-linked diagnostic staging must not publish a canonical archive");
    require(!std::filesystem::exists(stagedPath), "hard-link rejection must remove the known staging name through its "
                                                  "exact handle");
    require(std::filesystem::exists(injectedAlias) && std::filesystem::file_size(injectedAlias) == RotationStressBytes,
            "the native hard-link canary must retain the fully copied file and "
            "prove the same-token residual boundary");
    attackerParent.reset();
    require(::DeleteFileW(injectedAlias.c_str()) != FALSE,
            "the native diagnostic hard-link fixture must remove its injected alias");
    requireNoDiagnosticRotationTemporaries(fixture.logRoot);
}

void rotationCrashLeavesNoPartialCanonicalAndRestartCleansStaging()
{
    DiagnosticFixture fixture;
    const auto master = fixture.logRoot / L"forge-diagnostics.jsonl";
    const auto firstArchive = fixture.logRoot / L"forge-diagnostics.jsonl.1";
    createSizedFile(master, RotationStressBytes);
    const auto preCrashSource = readText(master);

    std::vector<wchar_t> moduleBuffer(32U * 1024U, L'\0');
    const DWORD moduleLength =
        ::GetModuleFileNameW(nullptr, moduleBuffer.data(), static_cast<DWORD>(moduleBuffer.size()));
    require(moduleLength > 0U && moduleLength < moduleBuffer.size(),
            "diagnostic crash test executable path must be resolved");
    const std::wstring modulePath{moduleBuffer.data(), static_cast<std::size_t>(moduleLength)};
    std::wstring commandLine = L"\"" + modulePath + L"\"";

    require(::SetEnvironmentVariableW(DiagnosticCrashChildVariable, L"1") != FALSE,
            "diagnostic crash-child mode must be configured");
    require(::SetEnvironmentVariableW(DiagnosticCrashRootVariable, fixture.logRoot.native().c_str()) != FALSE,
            "diagnostic crash-child root must be configured");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInformation{};
    const BOOL created = ::CreateProcessW(modulePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInformation);
    const DWORD createError = created != FALSE ? ERROR_SUCCESS : ::GetLastError();
    static_cast<void>(::SetEnvironmentVariableW(DiagnosticCrashRootVariable, nullptr));
    static_cast<void>(::SetEnvironmentVariableW(DiagnosticCrashChildVariable, nullptr));
    require(created != FALSE,
            std::string{"diagnostic crash child must launch, native error "} + std::to_string(createError));
    WindowsDetail::UniqueHandle childProcess{processInformation.hProcess};
    WindowsDetail::UniqueHandle childThread{processInformation.hThread};

    std::optional<std::filesystem::path> temporary;
    const auto discoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (!temporary.has_value() && std::chrono::steady_clock::now() < discoveryDeadline)
    {
        require(::WaitForSingleObject(childProcess.get(), 0U) == WAIT_TIMEOUT,
                "diagnostic crash child exited before entering its staging copy");
        temporary = findDiagnosticRotationTemporary(fixture.logRoot);
        if (!temporary.has_value())
        {
            std::this_thread::yield();
        }
    }
    require(temporary.has_value(), "diagnostic crash child must reach its noncanonical staging copy");
    const bool stagingStillNamed = findDiagnosticRotationTemporary(fixture.logRoot).has_value();
    const bool canonicalAbsent = !std::filesystem::exists(firstArchive);
    const BOOL terminated = ::TerminateProcess(childProcess.get(), 0xD1A6U);
    const DWORD terminationWait = ::WaitForSingleObject(childProcess.get(), 5000U);
    require(stagingStillNamed, "diagnostic crash child must remain at the "
                               "cooperative staging checkpoint");
    require(canonicalAbsent, "diagnostic crash child must not expose a partial canonical archive");
    require(terminated != FALSE, "diagnostic crash child must terminate at the staging boundary");
    require(terminationWait == WAIT_OBJECT_0, "diagnostic crash child termination must drain within five seconds");
    childThread.reset();
    childProcess.reset();

    require(std::filesystem::exists(master) && std::filesystem::file_size(master) == RotationStressBytes,
            "process termination during rotation must preserve the complete source");
    require(readText(master) == preCrashSource,
            "process termination during rotation must preserve the exact source bytes");
    require(!std::filesystem::exists(firstArchive), "process termination during rotation must not leave a partial "
                                                    "canonical archive");
    require(findDiagnosticRotationTemporary(fixture.logRoot).has_value(),
            "process termination must leave only a recoverable noncanonical "
            "staging file");

    WindowsDiagnosticSink restarted{fixture.options(rotationStressBudgets()),
                                    fixture.clockOwner,
                                    fixture.redactorOwner,
                                    fixture.hasherOwner,
                                    fixture.authorityOwner,
                                    fixture.atomicStoreOwner};
    const auto recovered =
        restarted.record(diagnostic("restart-after-crash", fixture.clock.utc()), liveContext(fixture.clock));
    require(static_cast<bool>(recovered), recovered ? "restart after diagnostic rotation crash must succeed"
                                                    : "restart after diagnostic rotation crash failed with " +
                                                          recovered.error().code + ": " + recovered.error().message);
    requireNoDiagnosticRotationTemporaries(fixture.logRoot);
    require(std::filesystem::exists(firstArchive) && std::filesystem::file_size(firstArchive) == RotationStressBytes,
            "restart must publish the complete pre-crash source as the first "
            "archive");
    require(readText(firstArchive) == preCrashSource,
            "restart must publish the exact pre-crash source bytes as the first archive");
    require(readText(master).find("restart-after-crash") != std::string::npos,
            "restart must append the post-recovery diagnostic record");
}

void boundsCancellationAndShutdownFailClosed()
{
    DiagnosticFixture fixture;
    WindowsDiagnosticSink sink{fixture.options(),   fixture.clockOwner,     fixture.redactorOwner,
                               fixture.hasherOwner, fixture.authorityOwner, fixture.atomicStoreOwner};

    requireError(sink.record(diagnostic("flattened-overflow", fixture.clock.utc(),
                                        {{"a", std::string(260U, 'a')}, {"b", std::string(260U, 'b')}}),
                             liveContext(fixture.clock)),
                 Domain::ErrorCodes::PayloadTooLarge, "flattened diagnostic fields must enforce the 512-byte cap");
    requireError(sink.exportData(Domain::DiagnosticExportRequest{std::nullopt, "../escape"},
                                 fixture.authority.authority(), liveContext(fixture.clock)),
                 Domain::ErrorCodes::InvalidRequest, "diagnostic export basename must reject traversal");
    requireError(sink.exportData(
                     Domain::DiagnosticExportRequest{
                         std::nullopt, std::string(WindowsDiagnosticSink::MaximumExportBasenameBytes + 1U, 'x')},
                     fixture.authority.authority(), liveContext(fixture.clock)),
                 Domain::ErrorCodes::PayloadTooLarge, "diagnostic export basename must enforce its byte cap");

    const auto staleArchive = fixture.logRoot / L"forge-diagnostics.jsonl.9";
    {
        std::ofstream stale{staleArchive, std::ios::binary | std::ios::trunc};
        stale << "{}\n";
    }
    fixture.authority.setDenyAll(true);
    const auto authorizationCount = fixture.authority.authorizationCount();
    requireError(sink.exportData(Domain::DiagnosticExportRequest{fixture.exportRootText, "denied_existing_root"},
                                 fixture.authority.authority(), liveContext(fixture.clock)),
                 Domain::ErrorCodes::Unauthorized, "diagnostic export touched storage before denying authority");
    require(fixture.authority.authorizationCount() > authorizationCount, "diagnostic export did not consult authority");
    require(std::filesystem::exists(staleArchive), "denied diagnostic export pruned a stale archive");
    const auto missingExportDirectory = fixture.exportRoot / L"missing";
    requireError(
        sink.exportData(Domain::DiagnosticExportRequest{pathText(missingExportDirectory), "denied_missing_root"},
                        fixture.authority.authority(), liveContext(fixture.clock)),
        Domain::ErrorCodes::Unauthorized, "diagnostic export probed a missing directory before authority denial");
    require(!std::filesystem::exists(missingExportDirectory),
            "denied diagnostic export created its requested directory");
    fixture.authority.setDenyAll(false);

    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        sink.record(diagnostic("cancelled", fixture.clock.utc()), liveContext(fixture.clock, cancellation.get_token())),
        Domain::ErrorCodes::Cancelled, "cancelled diagnostic append must fail before mutation");

    {
        HeldDiagnosticFileLock heldLock{fixture.logRoot};

        std::stop_source inFlightCancellation;
        std::atomic_bool cancellationEntered{};
        std::optional<Domain::Result<void>> cancellationResult;
        std::jthread cancellationThread{[&]() {
            cancellationEntered.store(true, std::memory_order_release);
            cancellationResult.emplace(sink.record(diagnostic("in-flight-cancel", fixture.clock.utc()),
                                                   liveContext(fixture.clock, inFlightCancellation.get_token())));
        }};
        const auto cancellationEntryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (!cancellationEntered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < cancellationEntryDeadline)
        {
            std::this_thread::yield();
        }
        require(cancellationEntered.load(std::memory_order_acquire),
                "in-flight cancellation fixture did not enter record");
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const auto cancelledAt = std::chrono::steady_clock::now();
        inFlightCancellation.request_stop();
        cancellationThread.join();
        require(cancellationResult.has_value(), "in-flight cancellation did not return a result");
        requireError(cancellationResult.value(), Domain::ErrorCodes::Cancelled,
                     "held diagnostic lock did not honor in-flight cancellation");
        require(std::chrono::steady_clock::now() - cancelledAt < std::chrono::seconds{1},
                "in-flight diagnostic cancellation exceeded its bounded drain");

        const auto deadlineStarted = std::chrono::steady_clock::now();
        const Domain::OperationContext deadlineContext{
            parse<Domain::OperationId>("88888888-8888-4888-8888-888888888888"),
            deadlineStarted + std::chrono::milliseconds{100},
            {},
            parse<Domain::CorrelationId>("p06-diagnostic-deadline")};
        requireError(sink.record(diagnostic("in-flight-deadline", fixture.clock.utc()), deadlineContext),
                     Domain::ErrorCodes::DeadlineExceeded, "held diagnostic lock ignored its in-flight deadline");
        require(std::chrono::steady_clock::now() - deadlineStarted < std::chrono::seconds{1},
                "in-flight diagnostic deadline exceeded its bounded drain");

        std::atomic_bool shutdownEntered{};
        std::optional<Domain::Result<void>> shutdownResult;
        std::jthread shutdownThread{[&]() {
            shutdownEntered.store(true, std::memory_order_release);
            shutdownResult.emplace(
                sink.record(diagnostic("in-flight-shutdown", fixture.clock.utc()), liveContext(fixture.clock)));
        }};
        const auto shutdownEntryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (!shutdownEntered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < shutdownEntryDeadline)
        {
            std::this_thread::yield();
        }
        require(shutdownEntered.load(std::memory_order_acquire), "in-flight shutdown fixture did not enter record");
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const auto shutdownStarted = std::chrono::steady_clock::now();
        sink.shutdown();
        require(std::chrono::steady_clock::now() - shutdownStarted < std::chrono::seconds{1},
                "diagnostic shutdown exceeded its bounded active-lock drain");
        shutdownThread.join();
        require(shutdownResult.has_value(), "in-flight shutdown did not return a result");
        requireError(shutdownResult.value(), Domain::ErrorCodes::TransportClosed,
                     "held diagnostic lock did not wake for sink shutdown");
    }

    requireError(sink.record(diagnostic("after-shutdown", fixture.clock.utc()), liveContext(fixture.clock)),
                 Domain::ErrorCodes::TransportClosed, "diagnostic sink must reject records after shutdown");
    requireError(sink.recent(1U, liveContext(fixture.clock)), Domain::ErrorCodes::TransportClosed,
                 "diagnostic sink must reject ring reads after shutdown");
    requireError(sink.exportData(Domain::DiagnosticExportRequest{std::nullopt, "after_shutdown"},
                                 fixture.authority.authority(), liveContext(fixture.clock)),
                 Domain::ErrorCodes::TransportClosed, "diagnostic sink must reject exports after shutdown");

    auto invalidBudgets = Domain::budgetsForProfile(Domain::ResourceProfile::Expanded32GiBPlus);
    invalidBudgets.diagnosticLogFilesMaximum = WindowsDiagnosticSink::MaximumRetainedLogFiles + 1U;
    WindowsDiagnosticSink invalid{fixture.options(invalidBudgets), fixture.clockOwner,
                                  fixture.redactorOwner,           fixture.hasherOwner,
                                  fixture.authorityOwner,          fixture.atomicStoreOwner};
    requireError(invalid.record(diagnostic("invalid-budget", fixture.clock.utc()), liveContext(fixture.clock)),
                 Domain::ErrorCodes::InvalidRequest, "diagnostic file budget must reject more than ten retained files");
}

} // namespace

void registerDiagnosticWindowsTests(TestRegistry &tests)
{
    if (diagnosticCrashChildRequested())
    {
        tests.clear();
        addTest(tests, "diagnostics.rotation-crash-child", diagnosticRotationCrashChild);
        return;
    }
    addTest(tests, "diagnostics.active-destructor-dependency-lifetime",
            activeFacadeDestructionRetainsOwnedDependencies);
    addTest(tests, "diagnostics.directory-creation-parent-anchor", diagnosticDirectoryCreationPinsEachParent);
    addTest(tests, "diagnostics.relative-file-operations", relativeFileOperationsStayBoundToStrongParent);
    addTest(tests, "diagnostics.hard-link-leaf-rejection", diagnosticLeafHardLinksFailClosed);
    addTest(tests, "diagnostics.case-sensitive-root-rejection", caseSensitiveDiagnosticDirectoriesFailClosed);
    addTest(tests, "diagnostics.final-root-protected-transaction",
            finalDiagnosticRootStaysProtectedDuringLogTransaction);
    addTest(tests, "diagnostics.redaction-all-surfaces", redactionPrecedesEveryDiagnosticSurface);
    addTest(tests, "diagnostics.restart-export-create-write", exportLoadsDurableRecordsAndUsesCreateThenWrite);
    addTest(tests, "diagnostics.concurrent-rotation-downshift", concurrentSinksRespectRotationAndDownshiftCaps);
    addTest(tests, "diagnostics.rotation-exclusive-cancellation", rotationTemporaryDeniesReadersAndCancelsCleanly);
    addTest(tests, "diagnostics.rotation-stage-hard-link-rejection",
            rotationStageHardLinkFailsClosedAtPublishValidation);
    addTest(tests, "diagnostics.rotation-crash-restart", rotationCrashLeavesNoPartialCanonicalAndRestartCleansStaging);
    addTest(tests, "diagnostics.bounds-cancel-shutdown", boundsCancellationAndShutdownFailClosed);
}

} // namespace ForgeConductor::Tests
