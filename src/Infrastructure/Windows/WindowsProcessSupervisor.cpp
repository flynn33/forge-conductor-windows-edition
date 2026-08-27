#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"

#include "Detail/CommandLineBuilder.h"
#include "Detail/JobObject.h"
#include "Detail/OperationGuard.h"
#include "Detail/OverlappedPipeReader.h"
#include "Detail/ProcessLaunchObserver.h"
#include "Detail/RelativeFileOperations.h"
#include "Detail/UniqueHandle.h"
#include "Detail/WindowsPathResolver.h"
#include "ForgeConductor/Domain/Error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Detail::CommandLineBuilder;
using Detail::EnvironmentEntry;
using Detail::IoCompletionPort;
using Detail::JobObject;
using Detail::OperationGuard;
using Detail::OperationRegistry;
using Detail::OperationState;
using Detail::OverlappedPipeReader;
using Detail::PipeEndpoints;
using Detail::RelativeObjectType;
using Detail::RelativeOpenDisposition;
using Detail::RelativeOpenOptions;
using Detail::TerminationReason;
using Detail::UniqueHandle;
using namespace std::chrono_literals;

constexpr auto TerminationConfirmationTimeout = 5s;
constexpr auto ReaderShutdownTimeout = 2s;
constexpr auto SupervisorShutdownTimeout = 15s;
constexpr auto StdinWriterShutdownTimeout = 2s;
constexpr DWORD StdinWriterShutdownTimeoutMilliseconds = 2'000U;
constexpr std::size_t StdinWriteChunkBytes = 16U * 1024U;

[[nodiscard]] bool containsAccess(const std::vector<Domain::FileAccess>& accesses,
                                  const Domain::FileAccess candidate) noexcept
{
    return std::find(accesses.begin(), accesses.end(), candidate) != accesses.end();
}

[[nodiscard]] Domain::Error win32Failure(const std::string_view code, const std::string_view action,
                                         const DWORD error)
{
    return Domain::makeError(code, std::string{action} + " failed with Win32 error " +
                                       std::to_string(error) + ".");
}

[[nodiscard]] bool isLocalDriveAbsolutePath(const std::wstring_view value) noexcept
{
    if (value.size() < 3U || value.starts_with(L"\\\\.\\") || value.starts_with(L"\\\\?\\")) {
        return false;
    }
    if (!((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
        value[1] != L':' || (value[2] != L'\\' && value[2] != L'/')) {
        return false;
    }
    std::array<wchar_t, 4> driveRoot{value[0], L':', L'\\', L'\0'};
    switch (::GetDriveTypeW(driveRoot.data())) {
    case DRIVE_REMOVABLE:
    case DRIVE_FIXED:
    case DRIVE_CDROM:
    case DRIVE_RAMDISK:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] Domain::Result<std::wstring> localPathText(const Domain::PathText& path)
{
    auto converted = CommandLineBuilder::utf8ToUtf16(path.value());
    if (!converted) {
        return converted;
    }
    if (!isLocalDriveAbsolutePath(converted.value())) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Process executable and working-directory paths must be absolute local-drive paths; "
            "UNC, device-namespace, and remote-drive paths are forbidden."));
    }
    return converted;
}

[[nodiscard]] Domain::Result<void> validateLocalRequestPaths(const Domain::ProcessRequest& request)
{
    auto executable = localPathText(request.executable);
    if (!executable) {
        return Domain::Result<void>::failure(std::move(executable).error());
    }
    if (request.workingDirectory) {
        auto workingDirectory = localPathText(request.workingDirectory.value());
        if (!workingDirectory) {
            return Domain::Result<void>::failure(std::move(workingDirectory).error());
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::wstring> absolutePath(const Domain::PathText& path)
{
    auto converted = localPathText(path);
    if (!converted) {
        return converted;
    }

    const auto required = ::GetFullPathNameW(converted.value().c_str(), 0, nullptr, nullptr);
    if (required == 0U) {
        return Domain::Result<std::wstring>::failure(
            win32Failure(Domain::ErrorCodes::InvalidRequest, "GetFullPathNameW", ::GetLastError()));
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const auto written =
        ::GetFullPathNameW(converted.value().c_str(), required, buffer.data(), nullptr);
    if (written == 0U || written >= required) {
        return Domain::Result<std::wstring>::failure(
            win32Failure(Domain::ErrorCodes::InvalidRequest, "GetFullPathNameW", ::GetLastError()));
    }
    std::wstring normalized{buffer.data(), static_cast<std::size_t>(written)};
    if (!isLocalDriveAbsolutePath(normalized)) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The normalized process path did not resolve to an absolute local-drive path."));
    }
    return Domain::Result<std::wstring>::success(std::move(normalized));
}

[[nodiscard]] std::wstring removeExtendedPrefix(std::wstring value)
{
    if (value.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + value.substr(8U);
    }
    if (value.starts_with(L"\\\\?\\")) {
        return value.substr(4U);
    }
    return value;
}

[[nodiscard]] Domain::Result<std::wstring> finalPathForHandle(const HANDLE handle,
                                                              const std::string_view failureCode)
{
    const auto required =
        ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0U) {
        return Domain::Result<std::wstring>::failure(
            win32Failure(failureCode, "GetFinalPathNameByHandleW", ::GetLastError()));
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const auto written =
        ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= buffer.size()) {
        return Domain::Result<std::wstring>::failure(
            win32Failure(failureCode, "GetFinalPathNameByHandleW", ::GetLastError()));
    }
    return Domain::Result<std::wstring>::success(
        removeExtendedPrefix(std::wstring{buffer.data(), static_cast<std::size_t>(written)}));
}

struct OpenedPath final {
    std::wstring finalPath;
    std::vector<UniqueHandle> anchors;
    std::vector<std::wstring> expectedPaths;
    bool finalIsDirectory{};
};

[[nodiscard]] bool ordinalEqual(std::wstring_view left, std::wstring_view right) noexcept;

[[nodiscard]] Domain::Result<void> verifyLaunchPathHandle(const HANDLE handle,
                                                          const std::wstring_view expectedPath,
                                                          const bool requireDirectory,
                                                          const std::string_view failureCode)
{
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                       sizeof(attributes)) == FALSE) {
        return Domain::Result<void>::failure(
            win32Failure(failureCode, "Inspect an anchored process path", ::GetLastError()));
    }
    const bool openedDirectory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (requireDirectory != openedDirectory ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            failureCode, "An anchored process path is a reparse point or has the wrong type."));
    }
    FILE_STANDARD_INFO standard{};
    if (::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
        FALSE) {
        return Domain::Result<void>::failure(
            win32Failure(failureCode, "Inspect anchored process path state", ::GetLastError()));
    }
    if (standard.DeletePending != FALSE) {
        return Domain::Result<void>::failure(
            Domain::makeError(failureCode, "An anchored process path is pending deletion."));
    }
    if (!requireDirectory && standard.NumberOfLinks != 1U) {
        return Domain::Result<void>::failure(Domain::makeError(
            failureCode,
            "A process executable with multiple hard links is outside authority policy."));
    }
    if (requireDirectory) {
        FILE_CASE_SENSITIVE_INFO caseSensitivity{};
        if (::GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &caseSensitivity,
                                           sizeof(caseSensitivity)) == FALSE) {
            return Domain::Result<void>::failure(win32Failure(
                failureCode, "Inspect anchored process-directory case policy", ::GetLastError()));
        }
        if ((caseSensitivity.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0U) {
            return Domain::Result<void>::failure(Domain::makeError(
                failureCode,
                "Case-sensitive directories are outside the Windows process authority policy."));
        }
    }
    auto finalPath = finalPathForHandle(handle, failureCode);
    if (!finalPath) {
        return Domain::Result<void>::failure(std::move(finalPath).error());
    }
    if (!ordinalEqual(finalPath.value(), expectedPath) &&
        !Detail::WindowsPathResolver::isExpectedPackagedLocalAppDataRedirect(
            expectedPath, finalPath.value())) {
        return Domain::Result<void>::failure(Domain::makeError(
            failureCode, "An anchored process path differs from its authorized canonical path."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> revalidateAnchoredPath(const OpenedPath& path,
                                                          const std::string_view failureCode)
{
    if (path.anchors.empty() || path.anchors.size() != path.expectedPaths.size()) {
        return Domain::Result<void>::failure(
            Domain::makeError(failureCode, "The process path anchor set is incomplete."));
    }
    for (std::size_t index = 0; index < path.anchors.size(); ++index) {
        const bool directory = index + 1U < path.anchors.size() || path.finalIsDirectory;
        auto verified = verifyLaunchPathHandle(path.anchors[index].get(), path.expectedPaths[index],
                                               directory, failureCode);
        if (!verified) {
            return verified;
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<OpenedPath> openAnchoredPath(std::wstring normalized,
                                                          const bool requireDirectory,
                                                          const std::string_view failureCode)
{
    try {
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        while (normalized.size() > 3U && normalized.ends_with(L'\\')) {
            normalized.pop_back();
        }
        if (!isLocalDriveAbsolutePath(normalized)) {
            return Domain::Result<OpenedPath>::failure(Domain::makeError(
                failureCode, "The process path is not an absolute local-drive path."));
        }

        std::wstring driveRoot{normalized.substr(0U, 3U)};
        UniqueHandle drive{::CreateFileW(
            driveRoot.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!drive) {
            return Domain::Result<OpenedPath>::failure(
                win32Failure(failureCode, "Anchor the process drive root", ::GetLastError()));
        }
        auto verified = verifyLaunchPathHandle(drive.get(), driveRoot, true, failureCode);
        if (!verified) {
            return Domain::Result<OpenedPath>::failure(std::move(verified).error());
        }

        std::vector<UniqueHandle> anchors;
        std::vector<std::wstring> expectedPaths;
        anchors.push_back(std::move(drive));
        expectedPaths.push_back(driveRoot);
        std::size_t componentStart = 3U;
        while (componentStart < normalized.size()) {
            const std::size_t separator = normalized.find(L'\\', componentStart);
            const bool finalComponent = separator == std::wstring::npos;
            const std::size_t componentEnd = finalComponent ? normalized.size() : separator;
            const std::wstring_view component{normalized.data() + componentStart,
                                              componentEnd - componentStart};
            const bool componentIsDirectory = !finalComponent || requireDirectory;

            RelativeOpenOptions options{};
            options.desiredAccess =
                FILE_READ_ATTRIBUTES |
                (componentIsDirectory ? FILE_LIST_DIRECTORY | FILE_TRAVERSE : FILE_EXECUTE);
            options.shareAccess = FILE_SHARE_READ;
            options.disposition = RelativeOpenDisposition::OpenExisting;
            options.fileAttributes = FILE_ATTRIBUTE_NORMAL;
            options.objectType =
                componentIsDirectory ? RelativeObjectType::Directory : RelativeObjectType::File;
            auto opened = Detail::openRelative(anchors.back().get(), component, options);
            if (!opened) {
                return Domain::Result<OpenedPath>::failure(win32Failure(
                    failureCode, "Open a process path component relative to its retained parent",
                    opened.win32Error));
            }
            const std::wstring_view expected{normalized.data(), componentEnd};
            verified = verifyLaunchPathHandle(opened.handle.get(), expected, componentIsDirectory,
                                              failureCode);
            if (!verified) {
                return Domain::Result<OpenedPath>::failure(std::move(verified).error());
            }
            anchors.push_back(std::move(opened.handle));
            expectedPaths.emplace_back(expected);
            if (finalComponent) {
                break;
            }
            componentStart = separator + 1U;
        }
        if (!requireDirectory && anchors.size() == 1U) {
            return Domain::Result<OpenedPath>::failure(
                Domain::makeError(failureCode, "The process executable path has no file name."));
        }
        return Domain::Result<OpenedPath>::success(OpenedPath{
            std::move(normalized), std::move(anchors), std::move(expectedPaths), requireDirectory});
    } catch (...) {
        return Domain::Result<OpenedPath>::failure(Domain::makeError(
            failureCode, "The process path could not be anchored without allocation failure."));
    }
}

[[nodiscard]] bool ordinalEqual(const std::wstring_view left,
                                const std::wstring_view right) noexcept
{
    if (left.size() > static_cast<std::size_t>(INT_MAX) ||
        right.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isWithinRoot(const std::wstring_view candidate,
                                const std::wstring_view root) noexcept
{
    if (ordinalEqual(candidate, root)) {
        return true;
    }
    if (candidate.size() <= root.size()) {
        return false;
    }
    const auto prefix = candidate.substr(0U, root.size());
    if (!ordinalEqual(prefix, root)) {
        return false;
    }
    if (root.ends_with(L'\\') || root.ends_with(L'/')) {
        return true;
    }
    return candidate[root.size()] == L'\\' || candidate[root.size()] == L'/';
}

[[nodiscard]] Domain::Result<std::wstring>
authorizedNormalizedPath(const Domain::PathText& requested,
                         const Contracts::WorkspaceAuthority& authority)
{
    auto candidate = absolutePath(requested);
    if (!candidate) {
        return Domain::Result<std::wstring>::failure(std::move(candidate).error());
    }
    for (const auto& root : authority.trustedRoots()) {
        auto normalizedRoot = absolutePath(root);
        if (normalizedRoot && isWithinRoot(candidate.value(), normalizedRoot.value())) {
            return Domain::Result<std::wstring>::success(std::move(candidate).value());
        }
    }
    return Domain::Result<std::wstring>::failure(
        Domain::makeError(Domain::ErrorCodes::Unauthorized,
                          "The requested process path is outside every explicit authority root."));
}

struct AuthorizedLaunchPaths final {
    OpenedPath application;
    std::optional<OpenedPath> workingDirectory;
};

[[nodiscard]] Domain::Result<AuthorizedLaunchPaths>
authorizeLaunchPaths(const Domain::ProcessRequest& request,
                     const Contracts::WorkspaceAuthority& authority)
{
    if (!authority.shellEnabled()) {
        return Domain::Result<AuthorizedLaunchPaths>::failure(
            Domain::makeError(Domain::ErrorCodes::ShellDisabled,
                              "Process execution is disabled by workspace authority."));
    }
    if (!containsAccess(authority.grants(), Domain::FileAccess::Execute) ||
        containsAccess(authority.denials(), Domain::FileAccess::Execute)) {
        return Domain::Result<AuthorizedLaunchPaths>::failure(
            Domain::makeError(Domain::ErrorCodes::Unauthorized,
                              "Workspace authority does not grant process execution."));
    }

    auto applicationPath = authorizedNormalizedPath(request.executable, authority);
    if (!applicationPath) {
        return Domain::Result<AuthorizedLaunchPaths>::failure(std::move(applicationPath).error());
    }
    auto application = openAnchoredPath(std::move(applicationPath).value(), false,
                                        Domain::ErrorCodes::ProcessLaunchFailed);
    if (!application) {
        return Domain::Result<AuthorizedLaunchPaths>::failure(std::move(application).error());
    }

    std::optional<OpenedPath> workingDirectory;
    if (request.workingDirectory) {
        auto directoryPath = authorizedNormalizedPath(request.workingDirectory.value(), authority);
        if (!directoryPath) {
            return Domain::Result<AuthorizedLaunchPaths>::failure(std::move(directoryPath).error());
        }
        auto directory = openAnchoredPath(std::move(directoryPath).value(), true,
                                          Domain::ErrorCodes::ProcessLaunchFailed);
        if (!directory) {
            return Domain::Result<AuthorizedLaunchPaths>::failure(std::move(directory).error());
        }
        workingDirectory.emplace(std::move(directory).value());
    }

    return Domain::Result<AuthorizedLaunchPaths>::success(
        AuthorizedLaunchPaths{std::move(application).value(), std::move(workingDirectory)});
}

class StartupAttributeList final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<StartupAttributeList>>
    create(const std::span<HANDLE> handles)
    {
        SIZE_T bytes{};
        static_cast<void>(::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes));
        if (bytes == 0U) {
            return Domain::Result<std::unique_ptr<StartupAttributeList>>::failure(
                win32Failure(Domain::ErrorCodes::ProcessLaunchFailed,
                             "InitializeProcThreadAttributeList sizing", ::GetLastError()));
        }

        auto result = std::unique_ptr<StartupAttributeList>{new StartupAttributeList{bytes}};
        if (!::InitializeProcThreadAttributeList(result->list_, 1, 0, &bytes)) {
            return Domain::Result<std::unique_ptr<StartupAttributeList>>::failure(
                win32Failure(Domain::ErrorCodes::ProcessLaunchFailed,
                             "InitializeProcThreadAttributeList", ::GetLastError()));
        }
        result->initialized_ = true;
        if (!::UpdateProcThreadAttribute(result->list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         handles.data(), handles.size_bytes(), nullptr, nullptr)) {
            return Domain::Result<std::unique_ptr<StartupAttributeList>>::failure(
                win32Failure(Domain::ErrorCodes::ProcessLaunchFailed, "UpdateProcThreadAttribute",
                             ::GetLastError()));
        }
        return Domain::Result<std::unique_ptr<StartupAttributeList>>::success(std::move(result));
    }

    ~StartupAttributeList() noexcept
    {
        if (initialized_) {
            ::DeleteProcThreadAttributeList(list_);
        }
    }

    StartupAttributeList(const StartupAttributeList&) = delete;
    StartupAttributeList& operator=(const StartupAttributeList&) = delete;

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    explicit StartupAttributeList(const SIZE_T bytes)
        : storage_((static_cast<std::size_t>(bytes) + sizeof(std::max_align_t) - 1U) /
                   sizeof(std::max_align_t)),
          list_{reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data())}
    {}

    std::vector<std::max_align_t> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_{};
    bool initialized_{};
};

class PipeNameFactory final {
public:
    PipeNameFactory() noexcept
    {
        LARGE_INTEGER counter{};
        static_cast<void>(::QueryPerformanceCounter(&counter));
        nonce_ = static_cast<std::uint64_t>(counter.QuadPart) ^
                 static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
    }

    [[nodiscard]] std::wstring make(const std::wstring_view stream)
    {
        const auto sequence = sequence_.fetch_add(1U, std::memory_order_relaxed);
        return L"\\\\.\\pipe\\ForgeConductor.Process." + std::to_wstring(::GetCurrentProcessId()) +
               L"." + std::to_wstring(nonce_) + L"." + std::to_wstring(sequence) + L"." +
               std::wstring{stream};
    }

private:
    std::atomic<std::uint64_t> sequence_{};
    std::uint64_t nonce_{};
};

void closeWritersAndStop(PipeEndpoints& stdoutPipe, PipeEndpoints& stderrPipe) noexcept
{
    stdoutPipe.childWriter.reset();
    stderrPipe.childWriter.reset();
    stdoutPipe.reader->finishAvailable();
    stderrPipe.reader->finishAvailable();
    if (!stdoutPipe.reader->waitUntilIdle(ReaderShutdownTimeout)) {
        stdoutPipe.reader->cancelAndWait();
    }
    if (!stderrPipe.reader->waitUntilIdle(ReaderShutdownTimeout)) {
        stderrPipe.reader->cancelAndWait();
    }
}

class StdinWriteState final {
public:
    StdinWriteState(UniqueHandle writer, UniqueHandle completionEvent) noexcept
        : writer_{std::move(writer)}, completionEvent_{std::move(completionEvent)}
    {
    }

    ~StdinWriteState() = default;
    StdinWriteState(const StdinWriteState&) = delete;
    StdinWriteState& operator=(const StdinWriteState&) = delete;
    StdinWriteState(StdinWriteState&&) = delete;
    StdinWriteState& operator=(StdinWriteState&&) = delete;

    void write(const std::string_view payload, const std::stop_token stopToken) noexcept
    {
        std::size_t offset{};
        DWORD finalError{ERROR_SUCCESS};
        while (offset < payload.size()) {
            {
                std::scoped_lock lock{mutex_};
                if (cancelRequested_ || stopToken.stop_requested()) {
                    finalError = ERROR_OPERATION_ABORTED;
                    break;
                }
                if (::ResetEvent(completionEvent_.get()) == FALSE) {
                    finalError = ::GetLastError();
                    break;
                }
                overlapped_ = {};
                overlapped_.hEvent = completionEvent_.get();
                pending_ = true;
                const auto count = static_cast<DWORD>(
                    (std::min)(StdinWriteChunkBytes, payload.size() - offset));
                const auto started = ::WriteFile(writer_.get(), payload.data() + offset, count,
                                                 nullptr, &overlapped_);
                const auto writeError = started ? ERROR_SUCCESS : ::GetLastError();
                if (started == FALSE && writeError != ERROR_IO_PENDING) {
                    pending_ = false;
                    finalError = writeError;
                    break;
                }
            }

            // The event is owned for the full thread lifetime. Cancellation uses CancelIoEx on
            // this exact OVERLAPPED, so a blocked named-pipe write always has a kernel wake path.
            if (::WaitForSingleObject(completionEvent_.get(), INFINITE) != WAIT_OBJECT_0) {
                std::terminate();
            }

            DWORD written{};
            {
                std::scoped_lock lock{mutex_};
                const auto completed =
                    ::GetOverlappedResult(writer_.get(), &overlapped_, &written, FALSE);
                const auto completionError = completed ? ERROR_SUCCESS : ::GetLastError();
                pending_ = false;
                if (completed == FALSE) {
                    finalError = completionError;
                    break;
                }
            }
            if (written == 0U) {
                finalError = ERROR_WRITE_FAULT;
                break;
            }
            offset += static_cast<std::size_t>(written);
            bytesWritten_.store(offset, std::memory_order_release);
        }

        {
            std::scoped_lock lock{mutex_};
            if (finalError == ERROR_SUCCESS && offset < payload.size()) {
                finalError = cancelRequested_ || stopToken.stop_requested()
                                 ? ERROR_OPERATION_ABORTED
                                 : ERROR_WRITE_FAULT;
            }
            if (finalError != ERROR_SUCCESS) {
                error_.store(finalError, std::memory_order_release);
            }
            writer_.reset();
            done_ = true;
        }
        doneCondition_.notify_all();
    }

    void cancel() noexcept
    {
        try {
            std::scoped_lock lock{mutex_};
            cancelRequested_ = true;
            if (pending_ && ::CancelIoEx(writer_.get(), &overlapped_) == FALSE) {
                const auto cancelError = ::GetLastError();
                if (cancelError != ERROR_NOT_FOUND) {
                    error_.store(cancelError, std::memory_order_release);
                }
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitUntilDone(const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return doneCondition_.wait_for(lock, timeout, [this] { return done_; });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t bytesWritten() const noexcept
    {
        return bytesWritten_.load(std::memory_order_acquire);
    }

    [[nodiscard]] DWORD error() const noexcept
    {
        return error_.load(std::memory_order_acquire);
    }

private:
    UniqueHandle writer_;
    UniqueHandle completionEvent_;
    mutable std::mutex mutex_;
    std::condition_variable doneCondition_;
    OVERLAPPED overlapped_{};
    std::atomic<std::size_t> bytesWritten_{};
    std::atomic<DWORD> error_{ERROR_SUCCESS};
    bool pending_{};
    bool cancelRequested_{};
    bool done_{};
};

struct StdinPipe final {
    UniqueHandle childReader;
    std::shared_ptr<StdinWriteState> writer;
};

[[nodiscard]] Domain::Result<void> cancelAndReapStdinConnection(
    const HANDLE server, OVERLAPPED& connection)
{
    const auto cancelled = ::CancelIoEx(server, &connection);
    const auto cancelError = cancelled ? ERROR_SUCCESS : ::GetLastError();
    if (::WaitForSingleObject(connection.hEvent, StdinWriterShutdownTimeoutMilliseconds) !=
        WAIT_OBJECT_0) {
        // The stack-owned OVERLAPPED cannot be released while the kernel can still reference it.
        std::terminate();
    }
    DWORD ignored{};
    if (::GetOverlappedResult(server, &connection, &ignored, FALSE) == FALSE) {
        const auto completionError = ::GetLastError();
        if (completionError == ERROR_IO_INCOMPLETE) {
            std::terminate();
        }
        if (completionError != ERROR_OPERATION_ABORTED && completionError != ERROR_PIPE_CONNECTED) {
            return Domain::Result<void>::failure(win32Failure(
                Domain::ErrorCodes::ProcessLaunchFailed,
                "GetOverlappedResult while reaping child standard-input connection",
                completionError));
        }
    }
    if (cancelled == FALSE && cancelError != ERROR_NOT_FOUND) {
        return Domain::Result<void>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "CancelIoEx for child standard-input connection", cancelError));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<StdinPipe> createStdinPipe(const std::wstring& pipeName)
{
    UniqueHandle writer{::CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(StdinWriteChunkBytes), static_cast<DWORD>(StdinWriteChunkBytes), 0,
        nullptr)};
    if (!writer) {
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "CreateNamedPipeW for child standard input", ::GetLastError()));
    }
    if (::SetHandleInformation(writer.get(), HANDLE_FLAG_INHERIT, 0U) == FALSE) {
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "SetHandleInformation for child standard-input writer", ::GetLastError()));
    }

    UniqueHandle connectionEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!connectionEvent) {
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "CreateEventW for child standard-input connection", ::GetLastError()));
    }
    OVERLAPPED connection{};
    connection.hEvent = connectionEvent.get();
    const auto connectionStarted = ::ConnectNamedPipe(writer.get(), &connection);
    const auto connectionError = connectionStarted ? ERROR_SUCCESS : ::GetLastError();
    const bool connectionPending =
        connectionStarted == FALSE && connectionError == ERROR_IO_PENDING;
    if (connectionStarted == FALSE && !connectionPending &&
        connectionError != ERROR_PIPE_CONNECTED) {
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "ConnectNamedPipe for child standard input", connectionError));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    UniqueHandle childReader{::CreateFileW(pipeName.c_str(), GENERIC_READ, 0, &inheritable,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!childReader) {
        const auto readerError = ::GetLastError();
        if (connectionPending) {
            auto reaped = cancelAndReapStdinConnection(writer.get(), connection);
            if (!reaped) {
                return Domain::Result<StdinPipe>::failure(std::move(reaped).error());
            }
        }
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "CreateFileW for child standard-input reader", readerError));
    }

    if (connectionPending) {
        const auto waitResult =
            ::WaitForSingleObject(connectionEvent.get(), StdinWriterShutdownTimeoutMilliseconds);
        if (waitResult != WAIT_OBJECT_0) {
            const auto waitError = waitResult == WAIT_FAILED ? ::GetLastError() : ERROR_TIMEOUT;
            childReader.reset();
            auto reaped = cancelAndReapStdinConnection(writer.get(), connection);
            if (!reaped) {
                return Domain::Result<StdinPipe>::failure(std::move(reaped).error());
            }
            return Domain::Result<StdinPipe>::failure(win32Failure(
                Domain::ErrorCodes::ProcessLaunchFailed,
                "WaitForSingleObject for child standard-input connection", waitError));
        }
        DWORD ignored{};
        if (::GetOverlappedResult(writer.get(), &connection, &ignored, FALSE) == FALSE) {
            const auto completionError = ::GetLastError();
            if (completionError == ERROR_IO_INCOMPLETE) {
                std::terminate();
            }
            if (completionError != ERROR_PIPE_CONNECTED) {
                return Domain::Result<StdinPipe>::failure(win32Failure(
                    Domain::ErrorCodes::ProcessLaunchFailed,
                    "GetOverlappedResult for child standard-input connection",
                    completionError));
            }
        }
    }

    UniqueHandle writeEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!writeEvent) {
        return Domain::Result<StdinPipe>::failure(win32Failure(
            Domain::ErrorCodes::ProcessLaunchFailed,
            "CreateEventW for child standard-input writes", ::GetLastError()));
    }
    auto writeState = std::make_shared<StdinWriteState>(std::move(writer), std::move(writeEvent));
    return Domain::Result<StdinPipe>::success(
        StdinPipe{std::move(childReader), std::move(writeState)});
}

class StdinWriterThread final {
public:
    StdinWriterThread() noexcept = default;
    ~StdinWriterThread() noexcept { cancelAndJoin(); }
    StdinWriterThread(const StdinWriterThread&) = delete;
    StdinWriterThread& operator=(const StdinWriterThread&) = delete;
    StdinWriterThread(StdinWriterThread&&) = delete;
    StdinWriterThread& operator=(StdinWriterThread&&) = delete;

    [[nodiscard]] Domain::Result<void> start(std::shared_ptr<StdinWriteState> state,
                                             const std::string_view payload)
    {
        if (!state || thread_.joinable()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The process standard-input writer was started with invalid ownership."));
        }
        state_ = std::move(state);
        try {
            thread_ = std::jthread{[state = state_, payload](const std::stop_token stopToken) {
                state->write(payload, stopToken);
            }};
        } catch (...) {
            state_.reset();
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The process standard-input writer thread could not be created."));
        }
        return Domain::Result<void>::success();
    }

    void cancelAndJoin() noexcept
    {
        if (!thread_.joinable()) {
            state_.reset();
            return;
        }
        thread_.request_stop();
        state_->cancel();
        if (!state_->waitUntilDone(StdinWriterShutdownTimeout)) {
            // Joining without a completion acknowledgement could hang shutdown, while releasing
            // state could free an OVERLAPPED still owned by the kernel. Fail closed instead.
            std::terminate();
        }
        thread_.join();
        state_.reset();
    }

private:
    std::shared_ptr<StdinWriteState> state_;
    std::jthread thread_;
};

[[nodiscard]] std::string lossyUtf8(const std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::string{bytes};
    }
    const auto inputLength = static_cast<int>(bytes.size());
    const auto wideLength =
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), inputLength, nullptr, 0);
    if (wideLength <= 0) {
        return std::string{bytes};
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), inputLength, wide.data(), wideLength) !=
        wideLength) {
        return std::string{bytes};
    }
    const auto utf8Length =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return std::string{bytes};
    }
    std::string normalized(static_cast<std::size_t>(utf8Length), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, normalized.data(), utf8Length,
                              nullptr, nullptr) != utf8Length) {
        return std::string{bytes};
    }
    return normalized;
}

[[nodiscard]] DWORD waitMillisecondsUntil(const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining = deadline - now;
    const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    const auto count = rounded.count();
    return static_cast<DWORD>(
        (std::min)(count, static_cast<decltype(count)>((std::numeric_limits<DWORD>::max)() - 1U)));
}

[[nodiscard]] Domain::Result<Domain::ProcessResult> terminationUnconfirmed()
{
    return Domain::Result<Domain::ProcessResult>::failure(
        Domain::makeError(Domain::ErrorCodes::ProcessTerminationUnconfirmed,
                          "The process supervisor did not confirm process-tree "
                          "termination within five seconds."));
}

} // namespace

class WindowsProcessSupervisor::Impl final {
public:
    Impl(Domain::ResourceBudgets budgets,
         std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
         std::shared_ptr<Detail::IProcessLaunchObserver> launchObserver = {})
        : budgets_{std::move(budgets)}, runtimeDiagnostics_{std::move(runtimeDiagnostics)},
          launchObserver_{std::move(launchObserver)}
    {
        if (!runtimeDiagnostics_) {
            initializationError_ = Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Windows process supervisor requires runtime diagnostics ownership.");
            return;
        }
        auto created = IoCompletionPort::create();
        if (created) {
            completionPort_ = std::move(created).value();
        } else {
            initializationError_ = std::move(created).error();
        }
    }

    ~Impl() noexcept
    {
        shutdown();
        if (completionPort_) {
            completionPort_->shutdown();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProcessResult>
    run(const Domain::ProcessRequest& request, const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context)
    {
        if (initializationError_) {
            return Domain::Result<Domain::ProcessResult>::failure(initializationError_.value());
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "The process operation was cancelled before admission."));
        }
        const auto startedAt = std::chrono::steady_clock::now();
        if (context.isExpired(startedAt)) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                  "The process operation deadline expired before admission."));
        }

        auto validated = Domain::validateProcessRequest(request, budgets_);
        if (!validated) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(validated).error());
        }
        auto localPaths = validateLocalRequestPaths(request);
        if (!localPaths) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(localPaths).error());
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The process operation was cancelled during local-path preflight."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The process operation deadline expired during local-path preflight."));
        }

        const auto requestDeadline = startedAt + request.timeout;
        const auto effectiveDeadline = (std::min)(requestDeadline, context.deadline);
        const auto deadlineReason = requestDeadline <= context.deadline
                                        ? TerminationReason::ProcessTimeout
                                        : TerminationReason::ContextDeadline;
        auto admitted = operations_.admit(context.operationId);
        if (!admitted) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(admitted).error());
        }
        auto guard = std::move(admitted).value();
        const auto state = guard.state();
        std::stop_callback cancellationCallback{
            context.cancellation,
            [state] { state->requestTermination(TerminationReason::Cancelled); }};
        if (state->reason() != TerminationReason::None) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "The process operation was cancelled during admission."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The process operation deadline expired before path authorization."));
        }
        auto paths = authorizeLaunchPaths(request, authority);
        if (!paths) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(paths).error());
        }
        if (state->reason() != TerminationReason::None) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The process operation was cancelled during path authorization."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The process operation deadline expired during path authorization."));
        }

        auto launchPaths = std::move(paths).value();
        auto commandLine = CommandLineBuilder::buildCommandLine(launchPaths.application.finalPath,
                                                                request.arguments);
        if (!commandLine) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(commandLine).error());
        }

        std::vector<EnvironmentEntry> inheritedEnvironment;
        if (request.inheritEnvironment) {
            auto inherited = CommandLineBuilder::readCurrentEnvironment();
            if (!inherited) {
                return Domain::Result<Domain::ProcessResult>::failure(std::move(inherited).error());
            }
            inheritedEnvironment = std::move(inherited).value();
        }
        auto environmentBlock =
            CommandLineBuilder::buildEnvironmentBlock(request, inheritedEnvironment);
        if (!environmentBlock) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(environmentBlock).error());
        }
        if (state->reason() != TerminationReason::None) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "The process operation was cancelled before native launch."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                  "The process operation deadline expired before native launch."));
        }

        auto childProcessOwnership =
            runtimeDiagnostics_->acquire(Contracts::RuntimeOwnerKind::ChildProcess, context);
        if (!childProcessOwnership) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(childProcessOwnership).error());
        }
        auto stdoutReaderOwnership =
            runtimeDiagnostics_->acquire(Contracts::RuntimeOwnerKind::ProcessReader, context);
        if (!stdoutReaderOwnership) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(stdoutReaderOwnership).error());
        }
        auto stderrReaderOwnership =
            runtimeDiagnostics_->acquire(Contracts::RuntimeOwnerKind::ProcessReader, context);
        if (!stderrReaderOwnership) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(stderrReaderOwnership).error());
        }
        state->setRuntimeOwnership(std::move(childProcessOwnership).value(),
                                   std::move(stdoutReaderOwnership).value(),
                                   std::move(stderrReaderOwnership).value());

        auto job = JobObject::create();
        if (!job) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(job).error());
        }
        const auto operationJob = std::move(job).value();
        auto jobCompletionResult = completionPort_->associateJob(operationJob->nativeHandle());
        if (!jobCompletionResult) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(jobCompletionResult).error());
        }
        auto jobCompletion = std::move(jobCompletionResult).value();

        auto stdoutPipeResult = OverlappedPipeReader::create(
            *completionPort_, pipeNames_.make(L"stdout"), request.maximumStdoutBytes);
        if (!stdoutPipeResult) {
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(stdoutPipeResult).error());
        }
        auto stdoutPipe = std::move(stdoutPipeResult).value();

        auto stderrPipeResult = OverlappedPipeReader::create(
            *completionPort_, pipeNames_.make(L"stderr"), request.maximumStderrBytes);
        if (!stderrPipeResult) {
            stdoutPipe.childWriter.reset();
            stdoutPipe.reader->cancelAndWait();
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(stderrPipeResult).error());
        }
        auto stderrPipe = std::move(stderrPipeResult).value();

        auto stdoutStarted = stdoutPipe.reader->start();
        if (!stdoutStarted) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(std::move(stdoutStarted).error());
        }
        auto stderrStarted = stderrPipe.reader->start();
        if (!stderrStarted) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(std::move(stderrStarted).error());
        }
        state->setReaders(stdoutPipe.reader, stderrPipe.reader);

        UniqueHandle childStdin;
        std::shared_ptr<StdinWriteState> stdinWriteState;
        if (request.stdinUtf8.empty()) {
            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;
            childStdin.reset(::CreateFileW(L"NUL", GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!childStdin) {
                closeWritersAndStop(stdoutPipe, stderrPipe);
                return Domain::Result<Domain::ProcessResult>::failure(
                    win32Failure(Domain::ErrorCodes::ProcessLaunchFailed,
                                 "CreateFileW for child standard input", ::GetLastError()));
            }
        } else {
            auto inputPipe = createStdinPipe(pipeNames_.make(L"stdin"));
            if (!inputPipe) {
                closeWritersAndStop(stdoutPipe, stderrPipe);
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(inputPipe).error());
            }
            auto pipe = std::move(inputPipe).value();
            childStdin = std::move(pipe.childReader);
            stdinWriteState = std::move(pipe.writer);
        }

        std::array<HANDLE, 3> inheritedHandles{childStdin.get(), stdoutPipe.childWriter.get(),
                                               stderrPipe.childWriter.get()};
        auto attributes = StartupAttributeList::create(inheritedHandles);
        if (!attributes) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(std::move(attributes).error());
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = childStdin.get();
        startup.StartupInfo.hStdOutput = stdoutPipe.childWriter.get();
        startup.StartupInfo.hStdError = stderrPipe.childWriter.get();
        startup.lpAttributeList = attributes.value()->get();

        if (state->reason() != TerminationReason::None) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "The process operation was cancelled before CreateProcessW."));
        }
        if (context.isExpired(std::chrono::steady_clock::now())) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                  "The process operation deadline expired before CreateProcessW."));
        }

        auto executableStillAuthorized = revalidateAnchoredPath(
            launchPaths.application, Domain::ErrorCodes::ProcessLaunchFailed);
        if (!executableStillAuthorized) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(
                std::move(executableStillAuthorized).error());
        }
        if (launchPaths.workingDirectory) {
            auto workingDirectoryStillAuthorized = revalidateAnchoredPath(
                launchPaths.workingDirectory.value(), Domain::ErrorCodes::ProcessLaunchFailed);
            if (!workingDirectoryStillAuthorized) {
                closeWritersAndStop(stdoutPipe, stderrPipe);
                return Domain::Result<Domain::ProcessResult>::failure(
                    std::move(workingDirectoryStillAuthorized).error());
            }
        }

        PROCESS_INFORMATION processInformation{};
        constexpr DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW;
        if (launchObserver_) {
            launchObserver_->beforeCreateProcess();
        }
        const auto created = ::CreateProcessW(
            launchPaths.application.finalPath.c_str(), commandLine.value().data(), nullptr, nullptr,
            TRUE, creationFlags, environmentBlock.value().data(),
            launchPaths.workingDirectory ? launchPaths.workingDirectory->finalPath.c_str()
                                         : nullptr,
            &startup.StartupInfo, &processInformation);
        const auto launchError = created ? ERROR_SUCCESS : ::GetLastError();
        UniqueHandle process{processInformation.hProcess};
        UniqueHandle primaryThread{processInformation.hThread};

        launchPaths.application.anchors.clear();
        if (launchPaths.workingDirectory) {
            launchPaths.workingDirectory->anchors.clear();
        }
        if (launchObserver_) {
            launchObserver_->afterCreateProcess();
        }

        childStdin.reset();
        stdoutPipe.childWriter.reset();
        stderrPipe.childWriter.reset();
        if (!created) {
            closeWritersAndStop(stdoutPipe, stderrPipe);
            return Domain::Result<Domain::ProcessResult>::failure(win32Failure(
                Domain::ErrorCodes::ProcessLaunchFailed, "CreateProcessW", launchError));
        }

        auto assigned = operationJob->assign(process.get());
        if (!assigned) {
            static_cast<void>(::TerminateProcess(process.get(), ERROR_CANCELLED));
            const auto directChildStopped =
                ::WaitForSingleObject(process.get(),
                                      static_cast<DWORD>(TerminationConfirmationTimeout.count() *
                                                         1'000)) == WAIT_OBJECT_0;
            closeWritersAndStop(stdoutPipe, stderrPipe);
            if (!directChildStopped) {
                return terminationUnconfirmed();
            }
            return Domain::Result<Domain::ProcessResult>::failure(std::move(assigned).error());
        }
        state->setJob(operationJob);

        if (std::chrono::steady_clock::now() >= effectiveDeadline) {
            state->requestTermination(deadlineReason);
        }
        auto resumed = state->resumePrimaryThread(primaryThread.get());
        if (!resumed) {
            auto resumeFailure = std::move(resumed).error();
            state->requestTermination(TerminationReason::Cancelled);
            const auto directChildStopped =
                ::WaitForSingleObject(process.get(),
                                      static_cast<DWORD>(TerminationConfirmationTimeout.count() *
                                                         1'000)) == WAIT_OBJECT_0;
            closeWritersAndStop(stdoutPipe, stderrPipe);
            const auto processTreeStopped =
                jobCompletion->waitUntilEmpty(TerminationConfirmationTimeout);
            if (!directChildStopped || !processTreeStopped) {
                return terminationUnconfirmed();
            }
            return Domain::Result<Domain::ProcessResult>::failure(std::move(resumeFailure));
        }
        primaryThread.reset();

        StdinWriterThread stdinWriterThread;
        if (stdinWriteState) {
            auto writerStarted =
                stdinWriterThread.start(stdinWriteState, std::string_view{request.stdinUtf8});
            if (!writerStarted) {
                auto startFailure = std::move(writerStarted).error();
                state->requestTermination(TerminationReason::Cancelled);
                const auto directChildStopped =
                    ::WaitForSingleObject(process.get(),
                                          static_cast<DWORD>(TerminationConfirmationTimeout.count() *
                                                             1'000)) == WAIT_OBJECT_0;
                closeWritersAndStop(stdoutPipe, stderrPipe);
                const auto processTreeStopped =
                    jobCompletion->waitUntilEmpty(TerminationConfirmationTimeout);
                if (!directChildStopped || !processTreeStopped) {
                    return terminationUnconfirmed();
                }
                return Domain::Result<Domain::ProcessResult>::failure(std::move(startFailure));
            }
        }

        std::array<HANDLE, 2> waits{process.get(), state->cancellationEvent()};
        const auto waitResult =
            ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE,
                                     waitMillisecondsUntil(effectiveDeadline));

        std::optional<Domain::Error> waitFailure;
        if (waitResult == WAIT_TIMEOUT) {
            state->requestTermination(deadlineReason);
        } else if (waitResult == WAIT_OBJECT_0 + 1U) {
            if (state->reason() == TerminationReason::None) {
                state->requestTermination(TerminationReason::Cancelled);
            }
        } else if (waitResult != WAIT_OBJECT_0) {
            const auto waitError = ::GetLastError();
            state->requestTermination(TerminationReason::Cancelled);
            waitFailure = win32Failure(Domain::ErrorCodes::InternalFailure,
                                       "WaitForMultipleObjects for process", waitError);
        }

        if (::WaitForSingleObject(process.get(),
                                  static_cast<DWORD>(TerminationConfirmationTimeout.count() *
                                                     1'000)) != WAIT_OBJECT_0) {
            state->stopReadersImmediately();
            stdoutPipe.reader->cancelAndWait();
            stderrPipe.reader->cancelAndWait();
            stdinWriterThread.cancelAndJoin();
            return terminationUnconfirmed();
        }

        DWORD nativeExitCode{};
        if (!::GetExitCodeProcess(process.get(), &nativeExitCode)) {
            const auto exitCodeError = ::GetLastError();
            static_cast<void>(operationJob->terminate(ERROR_CANCELLED));
            stdinWriterThread.cancelAndJoin();
            state->stopReadersImmediately();
            closeWritersAndStop(stdoutPipe, stderrPipe);
            if (!jobCompletion->waitUntilEmpty(TerminationConfirmationTimeout)) {
                return terminationUnconfirmed();
            }
            return Domain::Result<Domain::ProcessResult>::failure(win32Failure(
                Domain::ErrorCodes::InternalFailure, "GetExitCodeProcess", exitCodeError));
        }

        // A direct child may exit after spawning a descendant that inherited stdin. Terminate the
        // job tree before reaping the cancellable write so no descendant can retain the read end.
        static_cast<void>(operationJob->terminate(nativeExitCode));
        stdinWriterThread.cancelAndJoin();
        state->finishReadersAfterDirectChildExit();
        if (!stdoutPipe.reader->waitUntilIdle(ReaderShutdownTimeout)) {
            stdoutPipe.reader->cancelAndWait();
        }
        if (!stderrPipe.reader->waitUntilIdle(ReaderShutdownTimeout)) {
            stderrPipe.reader->cancelAndWait();
        }
        if (!jobCompletion->waitUntilEmpty(TerminationConfirmationTimeout)) {
            return terminationUnconfirmed();
        }

        if (waitFailure) {
            return Domain::Result<Domain::ProcessResult>::failure(std::move(waitFailure).value());
        }
        if (state->reason() == TerminationReason::ContextDeadline) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                                  "The process operation deadline expired during execution."));
        }

        const auto stdoutCapture = stdoutPipe.reader->capture();
        const auto stderrCapture = stderrPipe.reader->capture();
        if (stdoutCapture.readError != ERROR_SUCCESS) {
            return Domain::Result<Domain::ProcessResult>::failure(
                win32Failure(Domain::ErrorCodes::InternalFailure, "ReadFile for process stdout",
                             stdoutCapture.readError));
        }
        if (stderrCapture.readError != ERROR_SUCCESS) {
            return Domain::Result<Domain::ProcessResult>::failure(
                win32Failure(Domain::ErrorCodes::InternalFailure, "ReadFile for process stderr",
                             stderrCapture.readError));
        }

        const auto reason = state->reason();
        if (reason == TerminationReason::None && stdinWriteState &&
            stdinWriteState->bytesWritten() != request.stdinUtf8.size()) {
            const auto writeError = stdinWriteState->error();
            return Domain::Result<Domain::ProcessResult>::failure(win32Failure(
                Domain::ErrorCodes::TransportClosed, "WriteFile for process stdin",
                writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError));
        }
        return Domain::Result<Domain::ProcessResult>::success(Domain::ProcessResult{
            std::bit_cast<std::int32_t>(nativeExitCode), lossyUtf8(stdoutCapture.bytes),
            lossyUtf8(stderrCapture.bytes), reason == TerminationReason::ProcessTimeout,
            reason == TerminationReason::Cancelled || reason == TerminationReason::Shutdown,
            stdoutCapture.truncated, stderrCapture.truncated, true,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  startedAt)});
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        operations_.cancel(operationId);
    }

    void cancelAll() noexcept { operations_.cancelAll(); }

    void shutdown() noexcept
    {
        std::scoped_lock lock{shutdownMutex_};
        operations_.beginShutdown();
        if (operations_.waitUntilEmpty(SupervisorShutdownTimeout) && completionPort_ &&
            !completionPortStopped_) {
            completionPort_->shutdown();
            completionPortStopped_ = true;
        }
    }

private:
    const Domain::ResourceBudgets budgets_;
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics_;
    std::shared_ptr<Detail::IProcessLaunchObserver> launchObserver_;
    OperationRegistry operations_;
    std::unique_ptr<IoCompletionPort> completionPort_;
    std::optional<Domain::Error> initializationError_;
    PipeNameFactory pipeNames_;
    std::mutex shutdownMutex_;
    bool completionPortStopped_{};
};

WindowsProcessSupervisor::WindowsProcessSupervisor(
    Domain::ResourceBudgets budgets,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics)
    : implementation_{std::make_shared<Impl>(std::move(budgets), std::move(runtimeDiagnostics))}
{}

WindowsProcessSupervisor::WindowsProcessSupervisor(
    Domain::ResourceBudgets budgets,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    std::shared_ptr<Detail::IProcessLaunchObserver> launchObserver)
    : implementation_{std::make_shared<Impl>(std::move(budgets), std::move(runtimeDiagnostics),
                                             std::move(launchObserver))}
{}

WindowsProcessSupervisor::~WindowsProcessSupervisor()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<Domain::ProcessResult>
WindowsProcessSupervisor::run(const Domain::ProcessRequest& request,
                              const Contracts::WorkspaceAuthority& authority,
                              const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return Domain::Result<Domain::ProcessResult>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled,
                                  "The Windows process supervisor is no longer available."));
        }
        return implementation->run(request, authority, context);
    } catch (const std::exception& exception) {
        return Domain::Result<Domain::ProcessResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{"The Windows process supervisor failed: "} + exception.what()));
    } catch (...) {
        return Domain::Result<Domain::ProcessResult>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "The Windows process supervisor failed with an unknown exception."));
    }
}

void WindowsProcessSupervisor::cancel(const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancel(operationId);
    }
}

void WindowsProcessSupervisor::cancelAll() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancelAll();
    }
}

void WindowsProcessSupervisor::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
