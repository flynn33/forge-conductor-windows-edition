#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticLogTailReader.h"

#include "Detail/DiagnosticDirectoryTree.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/RelativeFileOperations.h"
#include "Detail/UniqueHandle.h"
#include "Detail/Win32Error.h"
#include "Detail/WindowsPathResolver.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Detail::AnchoredDiagnosticDirectoryTree;
using Detail::RelativeObjectType;
using Detail::RelativeOpenDisposition;
using Detail::RelativeOpenOptions;
using Detail::UniqueHandle;

constexpr std::wstring_view DiagnosticLogName = L"forge-diagnostics.jsonl";
constexpr std::wstring_view DiagnosticLockName = L".forge-diagnostics.lock";
constexpr std::size_t ScanBlockBytes = 16U * 1024U;
constexpr auto MaximumLockPoll = std::chrono::milliseconds{25};

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) noexcept
{
    return Detail::validateOperationContext(
        context, std::chrono::steady_clock::now(), action);
}

[[nodiscard]] Domain::Result<void> validateRequest(
    const std::size_t maximumLines,
    const std::size_t maximumLineBytes,
    const std::size_t maximumAggregateBytes) noexcept
{
    if (maximumLines > WindowsDiagnosticLogTailReader::MaximumRequestedLines ||
        maximumLineBytes == 0U ||
        maximumLineBytes >
            WindowsDiagnosticLogTailReader::MaximumRequestedLineBytes ||
        maximumAggregateBytes == 0U ||
        maximumAggregateBytes >
            WindowsDiagnosticLogTailReader::MaximumRequestedAggregateBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Diagnostic tail bounds must be positive and within the released "
            "dashboard limits."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value)
{
    constexpr std::wstring_view ExtendedUncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view ExtendedPrefix = L"\\\\?\\";
    if (value.starts_with(ExtendedUncPrefix)) {
        value = L"\\\\" + value.substr(ExtendedUncPrefix.size());
    } else if (value.starts_with(ExtendedPrefix)) {
        value.erase(0U, ExtendedPrefix.size());
    }
    return value;
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view value)
{
    constexpr std::wstring_view UncPrefix = L"\\\\";
    constexpr std::wstring_view ExtendedUncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view ExtendedPrefix = L"\\\\?\\";
    if (value.starts_with(ExtendedPrefix)) {
        return std::wstring{value};
    }
    if (value.starts_with(UncPrefix)) {
        return std::wstring{ExtendedUncPrefix} +
               std::wstring{value.substr(UncPrefix.size())};
    }
    return std::wstring{ExtendedPrefix} + std::wstring{value};
}

[[nodiscard]] bool equalPath(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return ::CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()), right.data(),
               static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] Domain::Result<void> verifyOpenedFile(
    const HANDLE file,
    const std::wstring_view expectedPath) noexcept
{
    try {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (::GetFileInformationByHandleEx(
                file, FileAttributeTagInfo, &attributes,
                sizeof(attributes)) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect the diagnostic tail file attributes", ::GetLastError()));
        }
        if ((attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The diagnostic tail file is a directory or reparse point."));
        }

        FILE_STANDARD_INFO standard{};
        if (::GetFileInformationByHandleEx(
                file, FileStandardInfo, &standard, sizeof(standard)) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect the diagnostic tail file link state", ::GetLastError()));
        }
        if (standard.NumberOfLinks != 1U || standard.DeletePending != FALSE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The diagnostic tail file is delete-pending or has more than "
                "one hard link."));
        }

        const DWORD required = ::GetFinalPathNameByHandleW(
            file, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "resolve the diagnostic tail file", ::GetLastError()));
        }
        if (required > Domain::PathText::MaximumBytes + 4U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The resolved diagnostic tail path exceeds its path bound."));
        }
        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            file, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "resolve the diagnostic tail file", ::GetLastError()));
        }
        const auto opened = withoutExtendedPrefix(
            std::wstring{buffer.data(), static_cast<std::size_t>(written)});
        if (!equalPath(opened, expectedPath)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The opened diagnostic tail file escaped its anchored path."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The diagnostic tail file identity could not be verified."));
    }
}

[[nodiscard]] Domain::Result<void> verifyOpenedDirectory(
    const HANDLE directory,
    const std::wstring_view expectedPath,
    std::wstring* const openedPath = nullptr) noexcept
{
    try {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (::GetFileInformationByHandleEx(
                directory, FileAttributeTagInfo, &attributes,
                sizeof(attributes)) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect the diagnostic directory attributes",
                ::GetLastError()));
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The diagnostic directory is not a direct directory."));
        }

        FILE_CASE_SENSITIVE_INFO caseSensitivity{};
        if (::GetFileInformationByHandleEx(
                directory, FileCaseSensitiveInfo, &caseSensitivity,
                sizeof(caseSensitivity)) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect the diagnostic directory case policy",
                ::GetLastError()));
        }
        auto supportedCasePolicy =
            Detail::WindowsPathResolver::validateDirectoryCaseSensitivityFlags(
                caseSensitivity.Flags);
        if (!supportedCasePolicy) {
            return supportedCasePolicy;
        }

        FILE_STANDARD_INFO standard{};
        if (::GetFileInformationByHandleEx(
                directory, FileStandardInfo, &standard, sizeof(standard)) ==
            FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "inspect the diagnostic directory state", ::GetLastError()));
        }
        if (standard.DeletePending != FALSE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The diagnostic directory is delete-pending."));
        }

        const DWORD required = ::GetFinalPathNameByHandleW(
            directory, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "resolve the diagnostic directory", ::GetLastError()));
        }
        if (required > Domain::PathText::MaximumBytes + 4U) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The resolved diagnostic directory exceeds its path bound."));
        }
        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            directory, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size()) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "resolve the diagnostic directory", ::GetLastError()));
        }
        const auto opened = withoutExtendedPrefix(
            std::wstring{buffer.data(), static_cast<std::size_t>(written)});
        if (!equalPath(opened, expectedPath) &&
            !Detail::WindowsPathResolver::
                isExpectedPackagedLocalAppDataRedirect(expectedPath, opened)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority,
                "The opened diagnostic directory escaped its anchored path."));
        }
        if (openedPath != nullptr) {
            *openedPath = opened;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The diagnostic directory identity could not be verified."));
    }
}

[[nodiscard]] DWORD boundedWaitMilliseconds(
    const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - now);
    return static_cast<DWORD>((std::min)(remaining, MaximumLockPoll).count());
}

[[nodiscard]] Domain::Error lockInterruption(
    const Domain::OperationContext& context) noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The diagnostic tail read was cancelled while waiting for the "
            "diagnostic lock.");
    }
    return Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "The diagnostic tail read exceeded its deadline while waiting for the "
        "diagnostic lock.");
}

class DiagnosticReadLock final {
public:
    DiagnosticReadLock(const DiagnosticReadLock&) = delete;
    DiagnosticReadLock& operator=(const DiagnosticReadLock&) = delete;
    DiagnosticReadLock& operator=(DiagnosticReadLock&&) = delete;

    DiagnosticReadLock(DiagnosticReadLock&& other) noexcept
        : handle_{std::move(other.handle_)},
          locked_{std::exchange(other.locked_, false)}
    {
    }

    ~DiagnosticReadLock() noexcept
    {
        if (locked_ && handle_) {
            OVERLAPPED operation{};
            static_cast<void>(
                ::UnlockFileEx(handle_.get(), 0U, 1U, 0U, &operation));
        }
    }

    [[nodiscard]] static Domain::Result<DiagnosticReadLock> acquire(
        AnchoredDiagnosticDirectoryTree& root,
        const std::wstring_view expectedPath,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, "acquire the diagnostic lock");
            if (!valid) {
                return Domain::Result<DiagnosticReadLock>::failure(
                    std::move(valid).error());
            }
            if (root.handles.empty()) {
                return Domain::Result<DiagnosticReadLock>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "The diagnostic tail reader has no retained root anchor."));
            }

            RelativeOpenOptions options{};
            options.desiredAccess = GENERIC_READ | GENERIC_WRITE;
            options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
            options.disposition = RelativeOpenDisposition::OpenOrCreate;
            options.objectType = RelativeObjectType::File;
            auto opened = Detail::openRelative(
                root.handles.back().get(), DiagnosticLockName, options);
            if (!opened) {
                return Domain::Result<DiagnosticReadLock>::failure(
                    Detail::makeWin32Error(
                        "open the handle-relative diagnostic tail lock",
                        opened.win32Error, Domain::ErrorCodes::InternalFailure,
                        opened.win32Error == ERROR_SHARING_VIOLATION));
            }
            valid = verifyOpenedFile(opened.handle.get(), expectedPath);
            if (!valid) {
                return Domain::Result<DiagnosticReadLock>::failure(
                    std::move(valid).error());
            }

            UniqueHandle cancellationEvent{
                ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!cancellationEvent) {
                return Domain::Result<DiagnosticReadLock>::failure(
                    Detail::makeWin32Error(
                        "create the diagnostic tail cancellation event",
                        ::GetLastError()));
            }
            std::stop_callback cancellationWake{
                context.cancellation,
                [event = cancellationEvent.get()]() noexcept {
                    static_cast<void>(::SetEvent(event));
                }};

            UniqueHandle handle{std::move(opened.handle)};
            while (true) {
                valid = validateContext(context, "acquire the diagnostic lock");
                if (!valid) {
                    return Domain::Result<DiagnosticReadLock>::failure(
                        std::move(valid).error());
                }

                OVERLAPPED operation{};
                if (::LockFileEx(
                        handle.get(),
                        LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                        0U, 1U, 0U, &operation) != FALSE) {
                    DiagnosticReadLock acquired{std::move(handle)};
                    valid = validateContext(
                        context, "read the diagnostic log after locking");
                    if (!valid) {
                        return Domain::Result<DiagnosticReadLock>::failure(
                            std::move(valid).error());
                    }
                    return Domain::Result<DiagnosticReadLock>::success(
                        std::move(acquired));
                }
                const DWORD lockError = ::GetLastError();
                if (lockError != ERROR_LOCK_VIOLATION) {
                    return Domain::Result<DiagnosticReadLock>::failure(
                        Detail::makeWin32Error(
                            "acquire the diagnostic tail lock", lockError,
                            Domain::ErrorCodes::InternalFailure,
                            lockError == ERROR_SHARING_VIOLATION));
                }

                const DWORD waitResult = ::WaitForSingleObject(
                    cancellationEvent.get(),
                    boundedWaitMilliseconds(context.deadline));
                if (waitResult == WAIT_OBJECT_0 ||
                    (waitResult == WAIT_TIMEOUT &&
                     context.isExpired(std::chrono::steady_clock::now()))) {
                    return Domain::Result<DiagnosticReadLock>::failure(
                        lockInterruption(context));
                }
                if (waitResult != WAIT_TIMEOUT) {
                    const DWORD waitError = waitResult == WAIT_FAILED
                                                ? ::GetLastError()
                                                : ERROR_INVALID_FUNCTION;
                    return Domain::Result<DiagnosticReadLock>::failure(
                        Detail::makeWin32Error(
                            "wait for the diagnostic tail lock", waitError));
                }
            }
        } catch (...) {
            return Domain::Result<DiagnosticReadLock>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The diagnostic tail lock could not be acquired."));
        }
    }

private:
    explicit DiagnosticReadLock(UniqueHandle handle) noexcept
        : handle_{std::move(handle)}, locked_{true}
    {
    }

    UniqueHandle handle_;
    bool locked_{};
};

struct DiagnosticReadTransaction final {
    AnchoredDiagnosticDirectoryTree root;
    DiagnosticReadLock lock;
    std::wstring masterPath;
};

struct ExistingDiagnosticRoot final {
    bool exists{};
    AnchoredDiagnosticDirectoryTree root;
};

[[nodiscard]] Domain::Result<ExistingDiagnosticRoot>
openExistingDiagnosticRoot(
    const Domain::PathText& rootText,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto root = Detail::WindowsPathResolver::resolveAppOwnedRoot(
            rootText.value());
        if (!root) {
            return Domain::Result<ExistingDiagnosticRoot>::failure(
                std::move(root).error());
        }

        auto valid = validateContext(
            context, "anchor the diagnostic tail directory");
        if (!valid) {
            return Domain::Result<ExistingDiagnosticRoot>::failure(
                std::move(valid).error());
        }

        AnchoredDiagnosticDirectoryTree anchored;
        std::vector<std::wstring> anchoredPaths;
        anchoredPaths.reserve(16U);
        anchored.handles.reserve(16U);

        const bool volumeRootIsFinal = root.value().size() == 3U;
        std::wstring current = root.value().substr(0U, 3U);
        const auto nativePath = extendedPath(current);
        UniqueHandle volumeRoot{::CreateFileW(
            nativePath.c_str(),
            FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE,
            FILE_SHARE_READ |
                (volumeRootIsFinal ? 0U : FILE_SHARE_WRITE),
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!volumeRoot) {
            const DWORD nativeError = ::GetLastError();
            return Domain::Result<ExistingDiagnosticRoot>::failure(
                Detail::makeWin32Error(
                    "open the diagnostic tail volume root",
                    nativeError,
                    Domain::ErrorCodes::InternalFailure,
                    nativeError == ERROR_SHARING_VIOLATION));
        }
        auto verified = verifyOpenedDirectory(
            volumeRoot.get(), current);
        if (!verified) {
            return Domain::Result<ExistingDiagnosticRoot>::failure(
                std::move(verified).error());
        }
        anchoredPaths.push_back(current);
        anchored.handles.push_back(std::move(volumeRoot));

        std::size_t cursor = 3U;
        while (cursor < root.value().size()) {
            valid = validateContext(
                context, "anchor the diagnostic tail directory");
            if (!valid) {
                return Domain::Result<ExistingDiagnosticRoot>::failure(
                    std::move(valid).error());
            }

            const std::size_t separator = root.value().find(L'\\', cursor);
            const std::size_t end = separator == std::wstring::npos
                                        ? root.value().size()
                                        : separator;
            const std::wstring_view component{
                root.value().data() + cursor, end - cursor};
            const std::wstring expectedPath{root.value().substr(0U, end)};
            const bool finalComponent = end == root.value().size();

            RelativeOpenOptions options{};
            options.desiredAccess =
                FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE;
            options.shareAccess = FILE_SHARE_READ |
                                  (finalComponent ? 0U : FILE_SHARE_WRITE);
            options.disposition = RelativeOpenDisposition::OpenExisting;
            options.objectType = RelativeObjectType::Directory;
            auto opened = Detail::openRelative(
                anchored.handles.back().get(), component, options);
            if (!opened) {
                valid = validateContext(
                    context, "anchor the diagnostic tail directory");
                if (!valid) {
                    return Domain::Result<ExistingDiagnosticRoot>::failure(
                        std::move(valid).error());
                }
                if (opened.win32Error == ERROR_FILE_NOT_FOUND ||
                    opened.win32Error == ERROR_PATH_NOT_FOUND) {
                    return Domain::Result<ExistingDiagnosticRoot>::success(
                        ExistingDiagnosticRoot{});
                }
                return Domain::Result<ExistingDiagnosticRoot>::failure(
                    Detail::makeWin32Error(
                        "open an anchored diagnostic tail directory",
                        opened.win32Error,
                        Domain::ErrorCodes::InternalFailure,
                        opened.win32Error == ERROR_SHARING_VIOLATION));
            }

            verified = verifyOpenedDirectory(
                opened.handle.get(), expectedPath);
            if (!verified) {
                return Domain::Result<ExistingDiagnosticRoot>::failure(
                    std::move(verified).error());
            }
            anchoredPaths.push_back(expectedPath);
            anchored.handles.push_back(std::move(opened.handle));
            current = expectedPath;
            cursor = end + 1U;
        }

        std::wstring openedRoot;
        for (std::size_t index = 0U; index < anchored.handles.size(); ++index) {
            verified = verifyOpenedDirectory(
                anchored.handles[index].get(),
                anchoredPaths[index],
                index + 1U == anchored.handles.size() ? &openedRoot : nullptr);
            if (!verified) {
                return Domain::Result<ExistingDiagnosticRoot>::failure(
                    std::move(verified).error());
            }
        }
        valid = validateContext(
            context, "finish anchoring the diagnostic tail directory");
        if (!valid) {
            return Domain::Result<ExistingDiagnosticRoot>::failure(
                std::move(valid).error());
        }

        anchored.root = std::move(openedRoot);
        return Domain::Result<ExistingDiagnosticRoot>::success(
            ExistingDiagnosticRoot{true, std::move(anchored)});
    } catch (...) {
        return Domain::Result<ExistingDiagnosticRoot>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The diagnostic tail directory could not be opened."));
    }
}

[[nodiscard]] Domain::Result<std::optional<DiagnosticReadTransaction>>
prepareTransaction(
    const Domain::PathText& rootText,
    const Domain::OperationContext& context) noexcept
{
    auto valid = validateContext(context, "open the diagnostic tail directory");
    if (!valid) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
                std::move(valid).error());
    }
    auto anchored = openExistingDiagnosticRoot(rootText, context);
    if (!anchored) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
            std::move(anchored).error());
    }
    valid = validateContext(context, "open the diagnostic tail directory");
    if (!valid) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
                std::move(valid).error());
    }
    if (!anchored.value().exists) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::success(std::nullopt);
    }
    auto masterPath = Detail::WindowsPathResolver::resolveAppOwnedChild(
        anchored.value().root.root, DiagnosticLogName,
        Detail::MissingPathPolicy::AllowLeaf);
    if (!masterPath) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
            std::move(masterPath).error());
    }
    auto lockPath = Detail::WindowsPathResolver::resolveAppOwnedChild(
        anchored.value().root.root, DiagnosticLockName,
        Detail::MissingPathPolicy::AllowLeaf);
    if (!lockPath) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
            std::move(lockPath).error());
    }
    auto lock = DiagnosticReadLock::acquire(
        anchored.value().root, lockPath.value(), context);
    if (!lock) {
        return Domain::Result<
            std::optional<DiagnosticReadTransaction>>::failure(
            std::move(lock).error());
    }
    return Domain::Result<
        std::optional<DiagnosticReadTransaction>>::success(
            DiagnosticReadTransaction{
                std::move(anchored).value().root,
                std::move(lock).value(),
                std::move(masterPath).value()});
}

struct OpenedMaster final {
    bool exists{};
    UniqueHandle handle;
    std::uint64_t bytes{};
};

[[nodiscard]] Domain::Result<OpenedMaster> openMaster(
    DiagnosticReadTransaction& transaction) noexcept
{
    RelativeOpenOptions options{};
    options.desiredAccess = GENERIC_READ | FILE_READ_ATTRIBUTES;
    options.shareAccess = FILE_SHARE_READ;
    options.disposition = RelativeOpenDisposition::OpenExisting;
    options.objectType = RelativeObjectType::File;
    auto opened = Detail::openRelative(
        transaction.root.handles.back().get(), DiagnosticLogName, options);
    if (!opened) {
        if (opened.win32Error == ERROR_FILE_NOT_FOUND ||
            opened.win32Error == ERROR_PATH_NOT_FOUND) {
            return Domain::Result<OpenedMaster>::success(OpenedMaster{});
        }
        return Domain::Result<OpenedMaster>::failure(Detail::makeWin32Error(
            "open the handle-relative diagnostic tail file",
            opened.win32Error, Domain::ErrorCodes::InternalFailure,
            opened.win32Error == ERROR_SHARING_VIOLATION));
    }

    auto verified = verifyOpenedFile(
        opened.handle.get(), transaction.masterPath);
    if (!verified) {
        return Domain::Result<OpenedMaster>::failure(
            std::move(verified).error());
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(opened.handle.get(), &size) == FALSE ||
        size.QuadPart < 0) {
        return Domain::Result<OpenedMaster>::failure(Detail::makeWin32Error(
            "measure the diagnostic tail file", ::GetLastError()));
    }
    return Domain::Result<OpenedMaster>::success(OpenedMaster{
        true, std::move(opened.handle),
        static_cast<std::uint64_t>(size.QuadPart)});
}

[[nodiscard]] Domain::Result<void> readExactAt(
    const HANDLE file,
    const std::uint64_t offset,
    const std::span<char> destination,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (offset > static_cast<std::uint64_t>(
                         (std::numeric_limits<LONGLONG>::max)())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The diagnostic tail file offset exceeds the Windows bound."));
        }
        auto valid = validateContext(context, "read the diagnostic log");
        if (!valid) {
            return valid;
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) == FALSE) {
            return Domain::Result<void>::failure(Detail::makeWin32Error(
                "seek the diagnostic tail file", ::GetLastError()));
        }

        std::size_t completed{};
        while (completed < destination.size()) {
            valid = validateContext(context, "read the diagnostic log");
            if (!valid) {
                return valid;
            }
            const auto remaining = destination.size() - completed;
            const DWORD requested = static_cast<DWORD>((std::min)(
                remaining,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read{};
            if (::ReadFile(
                    file, destination.data() + completed, requested, &read,
                    nullptr) == FALSE) {
                return Domain::Result<void>::failure(Detail::makeWin32Error(
                    "read the diagnostic tail file", ::GetLastError()));
            }
            if (read == 0U) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The diagnostic tail file changed during its locked read."));
            }
            completed += static_cast<std::size_t>(read);
        }
        return validateContext(context, "finish reading the diagnostic log");
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The diagnostic tail file could not be read."));
    }
}

struct PreviousLineFeed final {
    bool found{};
    std::uint64_t offset{};
};

[[nodiscard]] Domain::Result<PreviousLineFeed> findPreviousLineFeed(
    const HANDLE file,
    const std::uint64_t before,
    const std::size_t maximumRawLineBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        std::array<char, ScanBlockBytes> block{};
        std::uint64_t cursor = before;
        std::uint64_t scanned{};
        while (cursor > 0U) {
            const auto take = static_cast<std::size_t>((std::min)(
                cursor, static_cast<std::uint64_t>(block.size())));
            const std::uint64_t start = cursor - take;
            auto read = readExactAt(
                file, start, std::span<char>{block.data(), take}, context);
            if (!read) {
                return Domain::Result<PreviousLineFeed>::failure(
                    std::move(read).error());
            }
            for (std::size_t index = take; index > 0U; --index) {
                if (block[index - 1U] == '\n') {
                    return Domain::Result<PreviousLineFeed>::success(
                        PreviousLineFeed{
                            true, start + static_cast<std::uint64_t>(index - 1U)});
                }
            }
            scanned += static_cast<std::uint64_t>(take);
            if (scanned > maximumRawLineBytes) {
                return Domain::Result<PreviousLineFeed>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "A selected diagnostic line exceeds its byte bound."));
            }
            cursor = start;
        }
        return Domain::Result<PreviousLineFeed>::success(PreviousLineFeed{});
    } catch (...) {
        return Domain::Result<PreviousLineFeed>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The diagnostic tail line boundary could not be found."));
    }
}

[[nodiscard]] Domain::Result<std::string> readValidatedLine(
    const HANDLE file,
    const std::uint64_t start,
    const std::uint64_t end,
    const std::size_t maximumLineBytes,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (end < start) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The diagnostic tail line has an invalid file range."));
        }
        const std::uint64_t rawLength64 = end - start;
        const std::uint64_t maximumRaw =
            static_cast<std::uint64_t>(maximumLineBytes) + 1U;
        if (rawLength64 > maximumRaw) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A selected diagnostic line exceeds its byte bound."));
        }
        std::string line(static_cast<std::size_t>(rawLength64), '\0');
        if (!line.empty()) {
            auto read = readExactAt(
                file, start, std::span<char>{line.data(), line.size()}, context);
            if (!read) {
                return Domain::Result<std::string>::failure(
                    std::move(read).error());
            }
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() > maximumLineBytes) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A selected diagnostic line exceeds its byte bound."));
        }
        if (line.find('\r') != std::string::npos) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A selected diagnostic line contains a bare carriage return."));
        }
        if (line.find('\0') != std::string::npos) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A selected diagnostic line contains an embedded NUL."));
        }
        if (!Domain::isValidUtf8(line)) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A selected diagnostic line is not valid UTF-8."));
        }
        return Domain::Result<std::string>::success(std::move(line));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A selected diagnostic line could not be allocated."));
    }
}

[[nodiscard]] Domain::Result<void> validateStableSize(
    const OpenedMaster& master,
    const std::uint64_t expectedBytes) noexcept
{
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(master.handle.get(), &size) == FALSE ||
        size.QuadPart < 0) {
        return Domain::Result<void>::failure(Detail::makeWin32Error(
            "remeasure the diagnostic tail file", ::GetLastError()));
    }
    if (static_cast<std::uint64_t>(size.QuadPart) != expectedBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The diagnostic tail file changed during its locked read."));
    }
    return Domain::Result<void>::success();
}

} // namespace

WindowsDiagnosticLogTailReader::WindowsDiagnosticLogTailReader(
    Domain::PathText diagnosticsRoot)
    : diagnosticsRoot_{std::move(diagnosticsRoot)}
{
}

Domain::Result<std::vector<std::string>>
WindowsDiagnosticLogTailReader::newestLines(
    const std::size_t maximumLines,
    const std::size_t maximumLineBytes,
    const std::size_t maximumAggregateBytes,
    const Domain::OperationContext& context) const noexcept
{
    try {
        auto valid = validateRequest(
            maximumLines, maximumLineBytes, maximumAggregateBytes);
        if (!valid) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(valid).error());
        }
        valid = validateContext(context, "read diagnostic tail lines");
        if (!valid) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(valid).error());
        }
        if (maximumLines == 0U) {
            return Domain::Result<std::vector<std::string>>::success({});
        }

        auto transaction = prepareTransaction(diagnosticsRoot_, context);
        if (!transaction) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(transaction).error());
        }
        if (!transaction.value().has_value()) {
            return Domain::Result<std::vector<std::string>>::success({});
        }
        auto master = openMaster(transaction.value().value());
        if (!master) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(master).error());
        }
        if (!master.value().exists || master.value().bytes == 0U) {
            return Domain::Result<std::vector<std::string>>::success({});
        }

        char finalByte{};
        auto read = readExactAt(
            master.value().handle.get(), master.value().bytes - 1U,
            std::span<char>{&finalByte, 1U}, context);
        if (!read) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(read).error());
        }
        if (finalByte != '\n') {
            return Domain::Result<std::vector<std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The diagnostic tail file ends with an incomplete append."));
        }

        std::vector<std::string> newestFirst;
        newestFirst.reserve(maximumLines);
        std::size_t aggregateBytes{};
        std::uint64_t lineEnd = master.value().bytes - 1U;
        const std::size_t maximumRawLineBytes = maximumLineBytes + 1U;
        while (newestFirst.size() < maximumLines) {
            auto previous = findPreviousLineFeed(
                master.value().handle.get(), lineEnd, maximumRawLineBytes,
                context);
            if (!previous) {
                return Domain::Result<std::vector<std::string>>::failure(
                    std::move(previous).error());
            }
            const std::uint64_t lineStart = previous.value().found
                                                ? previous.value().offset + 1U
                                                : 0U;
            auto line = readValidatedLine(
                master.value().handle.get(), lineStart, lineEnd,
                maximumLineBytes, context);
            if (!line) {
                return Domain::Result<std::vector<std::string>>::failure(
                    std::move(line).error());
            }
            if (line.value().size() > maximumAggregateBytes - aggregateBytes) {
                return Domain::Result<std::vector<std::string>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Selected diagnostic lines exceed their aggregate byte "
                        "bound."));
            }
            aggregateBytes += line.value().size();
            newestFirst.push_back(std::move(line).value());
            if (!previous.value().found) {
                break;
            }
            lineEnd = previous.value().offset;
        }

        valid = validateStableSize(master.value(), master.value().bytes);
        if (!valid) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(valid).error());
        }
        valid = validateContext(context, "finish reading diagnostic tail lines");
        if (!valid) {
            return Domain::Result<std::vector<std::string>>::failure(
                std::move(valid).error());
        }
        std::reverse(newestFirst.begin(), newestFirst.end());
        return Domain::Result<std::vector<std::string>>::success(
            std::move(newestFirst));
    } catch (...) {
        return Domain::Result<std::vector<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Diagnostic tail lines could not be read."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
