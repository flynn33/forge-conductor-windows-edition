#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticLogTailReader.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::WindowsDiagnosticLogTailReader;
using Infrastructure::Windows::Detail::UniqueHandle;
using Infrastructure::Windows::Detail::strictUtf16ToUtf8;
using namespace std::chrono_literals;

static_assert(WindowsDiagnosticLogTailReader::MaximumRequestedLines == 100U);
static_assert(
    WindowsDiagnosticLogTailReader::MaximumRequestedLineBytes == 16U * 1024U);
static_assert(
    WindowsDiagnosticLogTailReader::MaximumRequestedAggregateBytes ==
    512U * 1024U);

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path& path)
{
    return take(Domain::PathText::create(
        take(strictUtf16ToUtf8(path.native()))));
}

[[nodiscard]] Domain::OperationContext activeContext(
    const std::stop_token cancellation = {},
    const std::chrono::milliseconds duration = 5min)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("81000000-0000-4000-8000-000000000001"),
        std::chrono::steady_clock::now() + duration,
        cancellation,
        parse<Domain::CorrelationId>("p16-diagnostic-tail-reader")};
}

[[nodiscard]] Domain::OperationContext expiredContext()
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("81000000-0000-4000-8000-000000000002"),
        std::chrono::steady_clock::now() - 1ms,
        {},
        parse<Domain::CorrelationId>("p16-diagnostic-tail-expired")};
}

class ScopedTestTree final {
public:
    ScopedTestTree()
    {
        std::wstring temporary(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(),
                "GetTempPathW failed for diagnostic tail tests");
        temporary.resize(length);
        base_ = std::filesystem::path{temporary} /
            (L"ForgeConductor.P16.DiagnosticTail." +
             std::to_wstring(::GetCurrentProcessId()) + L"." +
             std::to_wstring(::GetCurrentThreadId()) + L"." +
             std::to_wstring(::GetTickCount64()));
        root_ = base_ / L"logs";
        outside_ = base_ / L"outside";
        require(
            std::filesystem::create_directories(root_) &&
                std::filesystem::create_directories(outside_),
            "diagnostic tail fixture directories could not be created");
    }

    ~ScopedTestTree() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(base_, ignored));
    }

    ScopedTestTree(const ScopedTestTree&) = delete;
    ScopedTestTree& operator=(const ScopedTestTree&) = delete;

    [[nodiscard]] const std::filesystem::path& base() const noexcept
    {
        return base_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& outside() const noexcept
    {
        return outside_;
    }

    [[nodiscard]] std::filesystem::path master() const
    {
        return root_ / L"forge-diagnostics.jsonl";
    }

private:
    std::filesystem::path base_;
    std::filesystem::path root_;
    std::filesystem::path outside_;
};

void writeBytes(
    const std::filesystem::path& path,
    const std::string_view bytes)
{
    UniqueHandle file{::CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    require(static_cast<bool>(file), "diagnostic tail fixture file did not open");
    std::size_t completed{};
    while (completed < bytes.size()) {
        const DWORD requested = static_cast<DWORD>((std::min)(
            bytes.size() - completed,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        require(
            ::WriteFile(
                file.get(), bytes.data() + completed, requested, &written,
                nullptr) != FALSE &&
                written != 0U,
            "diagnostic tail fixture write failed");
        completed += static_cast<std::size_t>(written);
    }
    require(::FlushFileBuffers(file.get()) != FALSE,
            "diagnostic tail fixture flush failed");
}

class HeldDiagnosticLock final {
public:
    explicit HeldDiagnosticLock(const std::filesystem::path& root)
    {
        const auto path = root / L".forge-diagnostics.lock";
        handle_ = UniqueHandle{::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(handle_),
                "held diagnostic tail lock did not open");
        OVERLAPPED operation{};
        require(
            ::LockFileEx(
                handle_.get(),
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U,
                1U, 0U, &operation) != FALSE,
            "held diagnostic tail lock was not acquired");
        locked_ = true;
    }

    ~HeldDiagnosticLock() noexcept
    {
        release();
    }

    HeldDiagnosticLock(const HeldDiagnosticLock&) = delete;
    HeldDiagnosticLock& operator=(const HeldDiagnosticLock&) = delete;

    void release() noexcept
    {
        if (locked_ && handle_) {
            OVERLAPPED operation{};
            static_cast<void>(
                ::UnlockFileEx(handle_.get(), 0U, 1U, 0U, &operation));
            locked_ = false;
        }
        handle_.reset();
    }

private:
    UniqueHandle handle_;
    bool locked_{};
};

struct MountPointData final {
    DWORD tag{};
    WORD dataLength{};
    WORD reserved{};
    WORD substituteOffset{};
    WORD substituteLength{};
    WORD printOffset{};
    WORD printLength{};
    wchar_t pathBuffer[1]{};
};

void createJunction(
    const std::filesystem::path& junction,
    const std::filesystem::path& target)
{
    require(std::filesystem::create_directory(junction),
            "diagnostic tail junction placeholder was not created");
    UniqueHandle handle{::CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(static_cast<bool>(handle),
            "diagnostic tail junction placeholder did not open");

    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring printName = target.native();
    const std::size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t pathBytes = substituteBytes + sizeof(wchar_t) +
                                  printBytes + sizeof(wchar_t);
    const std::size_t totalBytes = offsetof(MountPointData, pathBuffer) + pathBytes;
    require(
        totalBytes <=
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) &&
            pathBytes + 8U <=
                static_cast<std::size_t>((std::numeric_limits<WORD>::max)()),
        "diagnostic tail junction payload exceeded native bounds");

    std::vector<std::uint64_t> storage(
        (totalBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
    auto* const data = reinterpret_cast<MountPointData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(pathBytes + 8U);
    data->substituteLength = static_cast<WORD>(substituteBytes);
    data->printOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    data->printLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substitute.data(), substituteBytes);
    data->pathBuffer[substitute.size()] = L'\0';
    std::memcpy(
        reinterpret_cast<std::byte*>(data->pathBuffer) + data->printOffset,
        printName.data(), printBytes);
    data->pathBuffer[
        (data->printOffset / sizeof(wchar_t)) + printName.size()] = L'\0';

    DWORD returned{};
    require(
        ::DeviceIoControl(
            handle.get(), FSCTL_SET_REPARSE_POINT, data,
            static_cast<DWORD>(totalBytes), nullptr, 0U, &returned,
            nullptr) != FALSE,
        "diagnostic tail junction reparse metadata was not installed");
}

void returnsNewestLinesInOriginalOrderAndNormalizesCrLf()
{
    ScopedTestTree tree;
    writeBytes(
        tree.master(),
        "first\r\nsnowman \xE2\x98\x83\nthird\r\nfourth\n");
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};

    const auto lines = take(reader.newestLines(3U, 64U, 128U, activeContext()));
    require(
        lines == std::vector<std::string>{
                     "snowman \xE2\x98\x83", "third", "fourth"},
        "diagnostic tail selection changed order or CRLF normalization");
    require(
        take(reader.newestLines(0U, 1U, 1U, activeContext())).empty(),
        "zero requested diagnostic lines did not return an empty result");
}

void returnsEmptyForMissingOrEmptyMaster()
{
    ScopedTestTree tree;
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};
    require(take(reader.newestLines(10U, 64U, 128U, activeContext())).empty(),
            "a missing diagnostic master did not return an empty result");

    writeBytes(tree.master(), {});
    require(take(reader.newestLines(10U, 64U, 128U, activeContext())).empty(),
            "an empty diagnostic master did not return an empty result");
}

void rejectsInvalidBoundsAndOversizedSelectedRecordsWithoutTruncation()
{
    ScopedTestTree tree;
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};
    requireError(
        reader.newestLines(
            WindowsDiagnosticLogTailReader::MaximumRequestedLines + 1U,
            64U, 128U, activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "an over-bound diagnostic line request was admitted");
    requireError(
        reader.newestLines(1U, 0U, 128U, activeContext()),
        Domain::ErrorCodes::InvalidRequest,
        "a zero diagnostic line byte bound was admitted");

    writeBytes(tree.master(), "oversized\nok\n");
    require(
        take(reader.newestLines(1U, 2U, 2U, activeContext())) ==
            std::vector<std::string>{"ok"},
        "an unselected older line affected the bounded tail result");
    requireError(
        reader.newestLines(2U, 2U, 64U, activeContext()),
        Domain::ErrorCodes::PayloadTooLarge,
        "an oversized selected diagnostic line was truncated or admitted");

    writeBytes(tree.master(), "abc\ndef\n");
    requireError(
        reader.newestLines(2U, 8U, 5U, activeContext()),
        Domain::ErrorCodes::PayloadTooLarge,
        "an over-bound diagnostic aggregate was truncated or admitted");
}

void handlesTheBackwardScanBoundaryAndExactLineLimit()
{
    ScopedTestTree tree;
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};
    const std::string exact(
        WindowsDiagnosticLogTailReader::MaximumRequestedLineBytes, 'x');
    writeBytes(tree.master(), exact + "\r\n");
    const auto lines = take(reader.newestLines(
        1U, WindowsDiagnosticLogTailReader::MaximumRequestedLineBytes,
        WindowsDiagnosticLogTailReader::MaximumRequestedAggregateBytes,
        activeContext()));
    require(lines == std::vector<std::string>{exact},
            "an exact-limit CRLF line spanning the backward scan boundary "
            "was changed");

    writeBytes(tree.master(), exact + "x\n");
    requireError(
        reader.newestLines(
            1U, WindowsDiagnosticLogTailReader::MaximumRequestedLineBytes,
            WindowsDiagnosticLogTailReader::MaximumRequestedAggregateBytes,
            activeContext()),
        Domain::ErrorCodes::PayloadTooLarge,
        "an over-limit line spanning the backward scan boundary was admitted");
}

void rejectsIncompleteMalformedAndNulSelectedLines()
{
    ScopedTestTree tree;
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};

    writeBytes(tree.master(), "partial");
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "an incomplete diagnostic append was returned");

    const std::string invalidUtf8{"bad \xC3\x28\n", 7U};
    writeBytes(tree.master(), invalidUtf8);
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "invalid UTF-8 diagnostic text was returned");

    const std::string embeddedNul{"a\0b\n", 4U};
    writeBytes(tree.master(), embeddedNul);
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "diagnostic text with an embedded NUL was returned");

    writeBytes(tree.master(), "a\rb\n");
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "a bare carriage return was admitted as diagnostic text");

    HeldDiagnosticLock reacquired{tree.root()};
}

void honorsPreflightAndInFlightCancellationAndDeadline()
{
    ScopedTestTree tree;
    writeBytes(tree.master(), "complete\n");
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};

    std::stop_source preCancelled;
    preCancelled.request_stop();
    requireError(
        reader.newestLines(
            1U, 64U, 64U, activeContext(preCancelled.get_token())),
        Domain::ErrorCodes::Cancelled,
        "a pre-cancelled diagnostic tail read was admitted");
    requireError(
        reader.newestLines(1U, 64U, 64U, expiredContext()),
        Domain::ErrorCodes::DeadlineExceeded,
        "an expired diagnostic tail read was admitted");

    HeldDiagnosticLock held{tree.root()};
    std::stop_source cancellation;
    std::atomic_bool entered{};
    std::optional<Domain::Result<std::vector<std::string>>> result;
    std::jthread worker{[&] {
        entered.store(true, std::memory_order_release);
        result.emplace(reader.newestLines(
            1U, 64U, 64U, activeContext(cancellation.get_token())));
    }};
    const auto entryDeadline = std::chrono::steady_clock::now() + 1s;
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entryDeadline) {
        std::this_thread::yield();
    }
    require(entered.load(std::memory_order_acquire),
            "the in-flight diagnostic tail read did not start");
    std::this_thread::sleep_for(50ms);
    const auto cancelledAt = std::chrono::steady_clock::now();
    cancellation.request_stop();
    worker.join();
    require(result.has_value(),
            "the cancelled diagnostic tail read did not return");
    requireError(
        result.value(), Domain::ErrorCodes::Cancelled,
        "the diagnostic tail lock wait ignored cancellation");
    require(std::chrono::steady_clock::now() - cancelledAt < 1s,
            "the cancelled diagnostic tail lock wait was not bounded");

    const auto deadlineStarted = std::chrono::steady_clock::now();
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext({}, 100ms)),
        Domain::ErrorCodes::DeadlineExceeded,
        "the diagnostic tail lock wait ignored its deadline");
    require(std::chrono::steady_clock::now() - deadlineStarted < 1s,
            "the diagnostic tail deadline wait was not bounded");
}

void rejectsReparseRootAndHardLinkedMaster()
{
    ScopedTestTree tree;
    const auto redirected = tree.base() / L"redirected-logs";
    createJunction(redirected, tree.outside());
    WindowsDiagnosticLogTailReader redirectedReader{pathText(redirected)};
    requireError(
        redirectedReader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "a reparse-point diagnostic root escaped its app-owned anchor");
    require(::RemoveDirectoryW(redirected.c_str()) != FALSE,
            "diagnostic tail junction fixture was not removed safely");

    const auto outsideFile = tree.outside() / L"foreign.jsonl";
    writeBytes(outsideFile, "foreign\n");
    require(
        ::CreateHardLinkW(tree.master().c_str(), outsideFile.c_str(), nullptr) !=
            FALSE,
        "diagnostic tail hard-link fixture was not created");
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::PathOutsideAuthority,
        "a hard-linked diagnostic master escaped its app-owned identity");
}

void waitsForTheSinkLockBeforeInspectingAnIncompleteAppend()
{
    ScopedTestTree tree;
    writeBytes(tree.master(), "incomplete");
    WindowsDiagnosticLogTailReader reader{pathText(tree.root())};
    HeldDiagnosticLock held{tree.root()};

    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext({}, 100ms)),
        Domain::ErrorCodes::DeadlineExceeded,
        "the diagnostic tail reader inspected a partial append without the "
        "sink lock");
    held.release();
    requireError(
        reader.newestLines(1U, 64U, 64U, activeContext()),
        Domain::ErrorCodes::IntegrityFailure,
        "an unlocked incomplete diagnostic append was returned");
}

} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    TestRegistry tests;
    addTest(
        tests, "diagnostic_tail.order_crlf",
        returnsNewestLinesInOriginalOrderAndNormalizesCrLf);
    addTest(
        tests, "diagnostic_tail.missing_empty",
        returnsEmptyForMissingOrEmptyMaster);
    addTest(
        tests, "diagnostic_tail.bounds",
        rejectsInvalidBoundsAndOversizedSelectedRecordsWithoutTruncation);
    addTest(
        tests, "diagnostic_tail.scan_boundary",
        handlesTheBackwardScanBoundaryAndExactLineLimit);
    addTest(
        tests, "diagnostic_tail.encoding_integrity",
        rejectsIncompleteMalformedAndNulSelectedLines);
    addTest(
        tests, "diagnostic_tail.context_lock_wait",
        honorsPreflightAndInFlightCancellationAndDeadline);
    addTest(
        tests, "diagnostic_tail.reparse_hardlink",
        rejectsReparseRootAndHardLinkedMaster);
    addTest(
        tests, "diagnostic_tail.partial_append_lock",
        waitsForTheSinkLockBeforeInspectingAnIncompleteAppend);

    std::size_t passed{};
    for (const auto& [name, run] : tests) {
        try {
            run();
            ++passed;
            std::printf("PASS %s\n", name.c_str());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", name.c_str(), error.what());
            return EXIT_FAILURE;
        }
    }
    std::printf("SUMMARY passed=%zu failed=0\n", passed);
    return EXIT_SUCCESS;
}
