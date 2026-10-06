#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr auto ChildTimeout = 30s;
constexpr auto ForcedCleanupTimeout = 5s;
constexpr auto DrainCancelRetryInterval = 25ms;
constexpr std::size_t MaximumCapturedBytes = 2U * 1024U * 1024U;
constexpr std::size_t ExpectedToolCount = 80U;

std::size_t assertions{};

void require(const bool condition, const std::string_view expression)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

[[nodiscard]] std::runtime_error win32Failure(
    const std::string_view operation,
    const DWORD error = ::GetLastError())
{
    return std::runtime_error{
        std::string{operation} + " failed with Win32 error " +
        std::to_string(error) + '.'};
}

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_{handle} {}
    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(const HANDLE replacement = nullptr) noexcept
    {
        const HANDLE previous = std::exchange(handle_, replacement);
        if (previous != nullptr && previous != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(previous));
        }
    }

private:
    HANDLE handle_{};
};

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto temporaryRoot = std::filesystem::temp_directory_path();
        for (std::uint32_t attempt{}; attempt < 32U; ++attempt) {
            const auto name =
                L"ForgeConductor-McpServeSnapshot-\u6e2c\u8a66-" +
                std::to_wstring(::GetCurrentProcessId()) + L'-' +
                std::to_wstring(::GetTickCount64()) + L'-' +
                std::to_wstring(attempt);
            const auto candidate = temporaryRoot / name;
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root_ = candidate;
                break;
            }
            if (error) {
                throw std::runtime_error{
                    "The isolated MCP process-test directory could not be created."};
            }
        }
        if (root_.empty()) {
            throw std::runtime_error{
                "A unique MCP process-test directory could not be allocated."};
        }
    }

    ~TemporaryDirectory() noexcept
    {
        if (!root_.empty()) {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove_all(root_, ignored));
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

private:
    std::filesystem::path root_;
};

[[nodiscard]] std::optional<std::wstring> environmentValue(
    const std::wstring_view name)
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD required = ::GetEnvironmentVariableW(
        std::wstring{name}.c_str(), nullptr, 0U);
    if (required == 0U) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        if (error == ERROR_SUCCESS) {
            return std::wstring{};
        }
        throw win32Failure("GetEnvironmentVariableW", error);
    }

    std::wstring value(required, L'\0');
    ::SetLastError(ERROR_SUCCESS);
    const DWORD written = ::GetEnvironmentVariableW(
        std::wstring{name}.c_str(), value.data(), required);
    const DWORD readError = ::GetLastError();
    if (written >= required || (written == 0U && readError != ERROR_SUCCESS)) {
        throw win32Failure("GetEnvironmentVariableW", readError);
    }
    value.resize(written);
    return value;
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(std::wstring name, const std::wstring_view value)
        : name_{std::move(name)}, previous_{environmentValue(name_)}
    {
        if (::SetEnvironmentVariableW(
                name_.c_str(), std::wstring{value}.c_str()) == FALSE) {
            throw win32Failure("SetEnvironmentVariableW");
        }
    }

    ~ScopedEnvironmentVariable() noexcept
    {
        static_cast<void>(::SetEnvironmentVariableW(
            name_.c_str(), previous_ ? previous_->c_str() : nullptr));
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

struct PipeEnds final {
    UniqueHandle reader;
    UniqueHandle writer;
};

[[nodiscard]] PipeEnds createPipe(const bool parentOwnsReader)
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readerRaw{};
    HANDLE writerRaw{};
    if (::CreatePipe(
            &readerRaw,
            &writerRaw,
            &attributes,
            65'536U) == FALSE) {
        throw win32Failure("CreatePipe");
    }
    PipeEnds pipe{UniqueHandle{readerRaw}, UniqueHandle{writerRaw}};
    const HANDLE parentHandle =
        parentOwnsReader ? pipe.reader.get() : pipe.writer.get();
    if (::SetHandleInformation(
            parentHandle,
            HANDLE_FLAG_INHERIT,
            0U) == FALSE) {
        throw win32Failure("SetHandleInformation");
    }
    return pipe;
}

[[nodiscard]] std::wstring quoteWindowsArgument(const std::wstring_view value)
{
    if (!value.empty() &&
        value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{value};
    }

    std::wstring quoted{L"\""};
    std::size_t backslashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0U;
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

struct ChildProcess final {
    UniqueHandle process;
    UniqueHandle inputWriter;
    UniqueHandle outputReader;
    UniqueHandle errorReader;
};

[[nodiscard]] ChildProcess launch(
    const std::filesystem::path& executable,
    const std::filesystem::path& home,
    const std::filesystem::path& workspace,
    const std::wstring_view role,
    const std::wstring_view deploymentId,
    const bool homeFromEnvironment)
{
    auto input = createPipe(false);
    auto output = createPipe(true);
    auto error = createPipe(true);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.reader.get();
    startup.hStdOutput = output.writer.get();
    startup.hStdError = error.writer.get();

    const auto executableText = executable.native();
    std::wstring commandLine = quoteWindowsArgument(executableText) + L" serve";
    if (!homeFromEnvironment) {
        commandLine.append(L" --home ");
        commandLine.append(quoteWindowsArgument(home.native()));
    }
    std::vector<wchar_t> mutableCommand{
        commandLine.begin(), commandLine.end()};
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    {
        const ScopedEnvironmentVariable roleEnvironment{
            L"FORGE_MCP_ROLE", role};
        const ScopedEnvironmentVariable deploymentEnvironment{
            L"FORGE_DEPLOYMENT_ID", deploymentId};
        std::optional<ScopedEnvironmentVariable> homeEnvironment;
        if (homeFromEnvironment) {
            homeEnvironment.emplace(L"FORGE_CONDUCTOR_HOME", home.native());
        }
        if (::CreateProcessW(
                executableText.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                workspace.native().c_str(),
                &startup,
                &process) == FALSE) {
            throw win32Failure("CreateProcessW");
        }
    }

    UniqueHandle processHandle{process.hProcess};
    UniqueHandle threadHandle{process.hThread};
    input.reader.reset();
    output.writer.reset();
    error.writer.reset();
    return ChildProcess{
        std::move(processHandle),
        std::move(input.writer),
        std::move(output.reader),
        std::move(error.reader)};
}

void writeAll(const HANDLE handle, const std::string_view bytes)
{
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        if (::WriteFile(
                handle,
                bytes.data() + offset,
                request,
                &written,
                nullptr) == FALSE ||
            written == 0U) {
            throw win32Failure("WriteFile");
        }
        offset += written;
    }
}

struct PipeCapture final {
    std::mutex mutex;
    std::condition_variable changed;
    std::string bytes;
    bool overflow{};
    std::optional<DWORD> failure;
    bool closed{};
};

struct PipeDrainState final {
    explicit PipeDrainState(UniqueHandle ownedReader) noexcept
        : reader{std::move(ownedReader)}
    {
    }

    UniqueHandle reader;
    PipeCapture capture;
};

void drainPipe(
    const HANDLE handle,
    PipeCapture& capture,
    const std::stop_token cancellation) noexcept
{
    try {
        std::array<char, 4U * 1024U> buffer{};
        for (;;) {
            if (cancellation.stop_requested()) {
                break;
            }
            DWORD read{};
            if (::ReadFile(
                    handle,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr) == FALSE) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF &&
                    !(error == ERROR_OPERATION_ABORTED &&
                      cancellation.stop_requested())) {
                    std::lock_guard lock{capture.mutex};
                    capture.failure = error;
                }
                break;
            }
            if (read == 0U) {
                break;
            }
            {
                std::lock_guard lock{capture.mutex};
                if (capture.bytes.size() + read <= MaximumCapturedBytes) {
                    capture.bytes.append(buffer.data(), read);
                } else {
                    capture.overflow = true;
                }
            }
            capture.changed.notify_all();
        }
    } catch (...) {
        try {
            std::lock_guard lock{capture.mutex};
            capture.failure = ERROR_OUTOFMEMORY;
        } catch (...) {
        }
    }
    try {
        {
            std::lock_guard lock{capture.mutex};
            capture.closed = true;
        }
        capture.changed.notify_all();
    } catch (...) {
    }
}

[[nodiscard]] DWORD waitMilliseconds(
    const std::chrono::milliseconds timeout) noexcept
{
    if (timeout <= std::chrono::milliseconds::zero()) {
        return 0U;
    }
    constexpr auto maximum = static_cast<std::chrono::milliseconds::rep>(
        INFINITE - 1U);
    return static_cast<DWORD>((std::min)(timeout.count(), maximum));
}

[[nodiscard]] bool terminateAndWait(
    const HANDLE process,
    const std::chrono::milliseconds timeout) noexcept
{
    if (process == nullptr || process == INVALID_HANDLE_VALUE) {
        return true;
    }
    const DWORD initial = ::WaitForSingleObject(process, 0U);
    if (initial == WAIT_OBJECT_0) {
        return true;
    }
    if (initial != WAIT_TIMEOUT) {
        return false;
    }

    static_cast<void>(::TerminateProcess(process, 124U));
    return ::WaitForSingleObject(process, waitMilliseconds(timeout)) ==
        WAIT_OBJECT_0;
}

void detachNoexcept(std::jthread& thread) noexcept
{
    try {
        if (thread.joinable()) {
            thread.detach();
        }
    } catch (...) {
    }
}

[[nodiscard]] bool joinSignaledThread(std::jthread& thread) noexcept
{
    try {
        thread.join();
        return true;
    } catch (...) {
        detachNoexcept(thread);
        return false;
    }
}

[[nodiscard]] bool waitAndJoinDrain(
    std::jthread& thread,
    const std::chrono::milliseconds timeout) noexcept
{
    if (!thread.joinable()) {
        return true;
    }
    const HANDLE threadHandle = thread.native_handle();
    if (::WaitForSingleObject(threadHandle, waitMilliseconds(timeout)) !=
        WAIT_OBJECT_0) {
        return false;
    }
    return joinSignaledThread(thread);
}

[[nodiscard]] bool cancelAndJoinDrain(
    std::jthread& thread,
    const std::chrono::milliseconds timeout) noexcept
{
    if (!thread.joinable()) {
        return true;
    }

    thread.request_stop();
    const HANDLE threadHandle = thread.native_handle();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        // Repeating cancellation closes the small race between the worker's
        // stop check and entry into its next synchronous ReadFile call.
        static_cast<void>(::CancelSynchronousIo(threadHandle));
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto waitSlice = (std::min)(
            remaining, std::chrono::duration_cast<std::chrono::milliseconds>(
                           DrainCancelRetryInterval));
        const DWORD wait = ::WaitForSingleObject(
            threadHandle,
            (std::max)(DWORD{1U}, waitMilliseconds(waitSlice)));
        if (wait == WAIT_OBJECT_0) {
            return joinSignaledThread(thread);
        }
        if (wait != WAIT_TIMEOUT) {
            break;
        }
    }

    // Drain state and its reader handle are shared with the worker, so a
    // detached last-resort reader cannot access this session after destruction.
    detachNoexcept(thread);
    return false;
}

[[nodiscard]] std::string handshakeStream()
{
    const Json initialize{
        {"id", 1},
        {"jsonrpc", "2.0"},
        {"method", "initialize"},
        {"params",
         Json{
             {"capabilities", Json::object()},
             {"clientInfo",
              Json{{"name", "forge-conductor-g14-process-test"},
                   {"version", "1.0"}}},
             {"protocolVersion", "2025-11-25"}}}};
    const Json initialized{
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", Json::object()}};
    const Json toolsList{
        {"id", 2},
        {"jsonrpc", "2.0"},
        {"method", "tools/list"},
        {"params", Json::object()}};
    return initialize.dump() + '\n' + initialized.dump() + '\n' +
        toolsList.dump() + '\n';
}

[[nodiscard]] std::string statusRequest(const std::int64_t id)
{
    const Json forgeStatus{
        {"id", id},
        {"jsonrpc", "2.0"},
        {"method", "tools/call"},
        {"params",
         Json{{"arguments", Json::object()}, {"name", "forge_status"}}}};
    return forgeStatus.dump() + '\n';
}

[[nodiscard]] std::string toolRequest(
    const std::int64_t id,
    const std::string_view name,
    Json arguments)
{
    const Json request{
        {"id", id},
        {"jsonrpc", "2.0"},
        {"method", "tools/call"},
        {"params",
         Json{{"arguments", std::move(arguments)}, {"name", std::string{name}}}}};
    return request.dump() + '\n';
}

[[nodiscard]] std::vector<Json> parseProtocolFrames(
    const std::string_view bytes)
{
    std::vector<Json> frames;
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto end = bytes.find('\n', offset);
        if (end == std::string_view::npos) {
            throw std::runtime_error{
                "MCP stdout ended with an unterminated protocol frame."};
        }
        auto line = bytes.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1U);
        }
        if (line.empty()) {
            throw std::runtime_error{
                "MCP stdout contained a blank protocol frame."};
        }
        auto frame = Json::parse(line.begin(), line.end());
        REQUIRE(frame.is_object());
        frames.push_back(std::move(frame));
        offset = end + 1U;
    }
    return frames;
}

[[nodiscard]] std::size_t completeFrameCount(
    const std::string_view bytes) noexcept
{
    return static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), '\n'));
}

struct PipeCaptureSnapshot final {
    std::string bytes;
    bool overflow{};
    std::optional<DWORD> failure;
    bool closed{};
};

[[nodiscard]] PipeCaptureSnapshot snapshotCapture(PipeCapture& capture)
{
    std::lock_guard lock{capture.mutex};
    return PipeCaptureSnapshot{
        capture.bytes,
        capture.overflow,
        capture.failure,
        capture.closed};
}

class McpProcessSession final {
public:
    McpProcessSession(
        const std::filesystem::path& executable,
        const std::filesystem::path& home,
        const std::filesystem::path& workspace,
        const std::wstring_view role,
        const std::wstring_view deploymentId,
        const bool homeFromEnvironment = false)
        : child_{launch(
              executable, home, workspace, role, deploymentId,
              homeFromEnvironment)}
    {
        try {
            output_ = std::make_shared<PipeDrainState>(
                std::move(child_.outputReader));
            error_ = std::make_shared<PipeDrainState>(
                std::move(child_.errorReader));

            const auto outputState = output_;
            outputDrain_ = std::jthread{
                [outputState](const std::stop_token cancellation) noexcept {
                    drainPipe(
                        outputState->reader.get(), outputState->capture,
                        cancellation);
                }};
            const auto errorState = error_;
            errorDrain_ = std::jthread{
                [errorState](const std::stop_token cancellation) noexcept {
                    drainPipe(
                        errorState->reader.get(), errorState->capture,
                        cancellation);
                }};
        } catch (...) {
            forceCleanup();
            throw;
        }
    }

    ~McpProcessSession() noexcept
    {
        forceCleanup();
    }

    McpProcessSession(const McpProcessSession&) = delete;
    McpProcessSession& operator=(const McpProcessSession&) = delete;
    McpProcessSession(McpProcessSession&&) = delete;
    McpProcessSession& operator=(McpProcessSession&&) = delete;

    void send(const std::string_view bytes)
    {
        if (finished_ || !child_.inputWriter) {
            throw std::runtime_error{
                "The stdio MCP child input is already closed."};
        }
        writeAll(child_.inputWriter.get(), bytes);
    }

    [[nodiscard]] std::vector<Json> awaitFrames(
        const std::size_t expectedCount)
    {
        std::string bytes;
        bool closedEarly{};
        auto& outputCapture = output_->capture;
        {
            std::unique_lock lock{outputCapture.mutex};
            const auto deadline = std::chrono::steady_clock::now() + ChildTimeout;
            const bool ready = outputCapture.changed.wait_until(
                lock,
                deadline,
                [&] {
                    return outputCapture.overflow ||
                        outputCapture.failure.has_value() ||
                        outputCapture.closed ||
                        completeFrameCount(outputCapture.bytes) >= expectedCount;
                });
            if (!ready) {
                throw std::runtime_error{
                    "The stdio MCP child did not produce its bounded response set."};
            }
            if (outputCapture.overflow) {
                throw std::runtime_error{
                    "The stdio MCP child exceeded the stdout capture bound."};
            }
            if (outputCapture.failure) {
                throw win32Failure(
                    "ReadFile(stdout)", outputCapture.failure.value());
            }
            if (completeFrameCount(outputCapture.bytes) < expectedCount) {
                closedEarly = true;
            } else {
                bytes = outputCapture.bytes;
            }
        }
        if (closedEarly) {
            DWORD exitCode{STILL_ACTIVE};
            static_cast<void>(::GetExitCodeProcess(
                child_.process.get(), &exitCode));
            const auto error = snapshotCapture(error_->capture);
            throw std::runtime_error{
                "The stdio MCP child closed stdout before completing its response set; "
                "exit code " + std::to_string(exitCode) + "; stderr: " +
                error.bytes};
        }
        auto frames = parseProtocolFrames(bytes);
        REQUIRE(frames.size() == expectedCount);
        return frames;
    }

    void finish(const std::size_t expectedFrameCount)
    {
        if (finished_) {
            throw std::runtime_error{
                "The stdio MCP child was already collected."};
        }
        child_.inputWriter.reset();

        const DWORD wait = ::WaitForSingleObject(
            child_.process.get(),
            waitMilliseconds(std::chrono::duration_cast<
                             std::chrono::milliseconds>(ChildTimeout)));
        if (wait == WAIT_TIMEOUT) {
            const bool terminationConfirmed = terminateAndWait(
                child_.process.get(),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    ForcedCleanupTimeout));
            throw std::runtime_error{
                std::string{
                    "The stdio MCP child did not exit after end-of-file; "
                    "forced termination was "} +
                (terminationConfirmed ? "confirmed." : "not confirmed.")};
        }
        if (wait != WAIT_OBJECT_0) {
            throw win32Failure("WaitForSingleObject");
        }
        DWORD exitCode{};
        if (::GetExitCodeProcess(child_.process.get(), &exitCode) == FALSE) {
            throw win32Failure("GetExitCodeProcess");
        }

        if (!collectDrainsExactly()) {
            throw std::runtime_error{
                "The stdio MCP pipe drains did not finish within their bounds."};
        }
        const auto output = snapshotCapture(output_->capture);
        const auto error = snapshotCapture(error_->capture);
        REQUIRE(output.closed);
        REQUIRE(error.closed);
        REQUIRE(!output.overflow);
        REQUIRE(!error.overflow);
        REQUIRE(!output.failure.has_value());
        REQUIRE(!error.failure.has_value());
        if (exitCode != 0U) {
            throw std::runtime_error{
                "The stdio MCP child exited with code " +
                std::to_string(exitCode) + "; stderr: " + error.bytes};
        }
        REQUIRE(error.bytes.empty());
        auto frames = parseProtocolFrames(output.bytes);
        REQUIRE(frames.size() == expectedFrameCount);
        finished_ = true;
    }

private:
    [[nodiscard]] bool collectDrainExactly(std::jthread& thread) noexcept
    {
        const auto timeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                ForcedCleanupTimeout);
        if (waitAndJoinDrain(thread, timeout)) {
            return true;
        }
        static_cast<void>(cancelAndJoinDrain(thread, timeout));
        return false;
    }

    [[nodiscard]] bool collectDrainsExactly() noexcept
    {
        const bool outputCollected = collectDrainExactly(outputDrain_);
        const bool errorCollected = collectDrainExactly(errorDrain_);
        return outputCollected && errorCollected;
    }

    void forceCleanup() noexcept
    {
        child_.inputWriter.reset();
        const auto timeout =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                ForcedCleanupTimeout);
        const bool childStopped =
            terminateAndWait(child_.process.get(), timeout);
        const bool outputStopped = cancelAndJoinDrain(outputDrain_, timeout);
        const bool errorStopped = cancelAndJoinDrain(errorDrain_, timeout);
        static_cast<void>(childStopped);
        static_cast<void>(outputStopped);
        static_cast<void>(errorStopped);
    }

    ChildProcess child_;
    std::shared_ptr<PipeDrainState> output_;
    std::shared_ptr<PipeDrainState> error_;
    std::jthread outputDrain_;
    std::jthread errorDrain_;
    bool finished_{};
};

[[nodiscard]] const Json& responseFor(
    const std::vector<Json>& frames,
    const std::int64_t id)
{
    const auto response = std::find_if(
        frames.begin(), frames.end(), [id](const Json& candidate) {
            return candidate.contains("id") &&
                candidate.at("id").is_number_integer() &&
                candidate.at("id").get<std::int64_t>() == id;
        });
    REQUIRE(response != frames.end());
    return *response;
}

[[nodiscard]] Json successfulToolPayload(
    const std::vector<Json>& frames,
    const std::int64_t id)
{
    const auto& response = responseFor(frames, id);
    REQUIRE(response.size() == 3U);
    REQUIRE(response.at("jsonrpc") == "2.0");
    REQUIRE(response.contains("result"));
    REQUIRE(!response.contains("error"));
    const auto& result = response.at("result");
    REQUIRE(result.is_object());
    if (!result.contains("isError") || result.at("isError") != false) {
        throw std::runtime_error{
            "Tool request " + std::to_string(id) +
            " returned an error result: " + result.dump()};
    }
    REQUIRE(result.at("isError") == false);
    REQUIRE(result.at("structuredContent").is_object());
    REQUIRE(result.at("content").is_array());
    const auto& content = result.at("content");
    REQUIRE(!content.empty());
    std::string serialized;
    std::size_t totalBytes{};
    for (std::size_t index{}; index < content.size(); ++index) {
        REQUIRE(content.at(index).at("type") == "text");
        const auto text = content.at(index).at("text").get<std::string>();
        REQUIRE(text.size() <= 32U * 1024U);
        const auto value = Json::parse(text);
        if (content.size() == 1U) {
            serialized = text;
            break;
        }
        REQUIRE(value.is_object());
        REQUIRE(value.size() == 7U);
        REQUIRE(value.at("kind") == "forge_tool_result_fragment");
        REQUIRE(value.at("version") == 1U);
        REQUIRE(value.at("index") == index);
        REQUIRE(value.at("count") == content.size());
        REQUIRE(value.at("total_bytes").is_number_unsigned());
        const auto expectedBytes = value.at("total_bytes").get<std::size_t>();
        REQUIRE(expectedBytes > 32U * 1024U);
        REQUIRE(index == 0U || expectedBytes == totalBytes);
        totalBytes = expectedBytes;
        REQUIRE(value.at("part").is_string());
        const auto part = value.at("part").get<std::string>();
        REQUIRE(!part.empty());
        REQUIRE(part.size() <= 12U * 1024U);
        REQUIRE(value.at("instruction") == "Concatenate part from every fragment in index order, then parse the complete JSON tool result. Do not repeat the tool call.");
        serialized += part;
    }
    REQUIRE(content.size() == 1U || serialized.size() == totalBytes);
    REQUIRE(Json::parse(serialized) == result.at("structuredContent"));
    REQUIRE(serialized == result.at("structuredContent").dump());
    return result.at("structuredContent");
}

void validateToolArray(
    const Json& tools,
    const std::size_t expectedCount = ExpectedToolCount)
{
    REQUIRE(tools.is_array());
    REQUIRE(tools.size() == expectedCount);
    std::set<std::string, std::less<>> names;
    std::string previous;
    for (const auto& tool : tools) {
        REQUIRE(tool.is_object());
        REQUIRE(tool.size() == 3U);
        REQUIRE(tool.contains("name"));
        REQUIRE(tool.contains("description"));
        REQUIRE(tool.contains("inputSchema"));
        REQUIRE(tool.at("name").is_string());
        REQUIRE(tool.at("description").is_string());
        REQUIRE(!tool.at("description").get_ref<const std::string&>().empty());
        REQUIRE(tool.at("inputSchema").is_object());
        const auto& name = tool.at("name").get_ref<const std::string&>();
        REQUIRE(previous.empty() || previous < name);
        REQUIRE(names.insert(name).second);
        previous = name;
    }
}

[[nodiscard]] Json loadGolden(const std::filesystem::path& path)
{
    REQUIRE(std::filesystem::is_regular_file(path));
    const auto bytes = std::filesystem::file_size(path);
    REQUIRE(bytes > 0U);
    REQUIRE(bytes <= MaximumCapturedBytes);
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    std::string content(static_cast<std::size_t>(bytes), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(static_cast<std::size_t>(input.gcount()) == content.size());
    const auto golden = Json::parse(content.begin(), content.end());
    REQUIRE(golden.is_object());
    REQUIRE(golden.size() == 2U);
    REQUIRE(golden.at("schemaVersion") == 1);
    validateToolArray(golden.at("tools"));
    return golden;
}

struct RoleObservation final {
    std::string serverName;
    Json tools;
    std::string instructions;
};

[[nodiscard]] RoleObservation observeRole(
    McpProcessSession& session,
    const std::size_t expectedToolCount = ExpectedToolCount)
{
    const auto frames = session.awaitFrames(2U);

    const auto& initialize = responseFor(frames, 1);
    REQUIRE(initialize.size() == 3U);
    REQUIRE(initialize.at("jsonrpc") == "2.0");
    REQUIRE(initialize.contains("result"));
    REQUIRE(!initialize.contains("error"));
    const auto& initializeResult = initialize.at("result");
    REQUIRE(initializeResult.at("protocolVersion") == "2025-11-25");
    REQUIRE(initializeResult.at("serverInfo").at("version") == "1.3.13");
    REQUIRE(initializeResult.at("capabilities").at("tools").at("listChanged") == false);
    const auto& instructions =
        initializeResult.at("instructions").get_ref<const std::string&>();
    REQUIRE(instructions.find("Project folder: ") != std::string::npos);
    REQUIRE(instructions.find("Instruction package folders (ordered):") !=
            std::string::npos);
    REQUIRE(instructions.find("Development policy source: ") !=
            std::string::npos);

    const auto& listed = responseFor(frames, 2);
    REQUIRE(listed.size() == 3U);
    REQUIRE(listed.at("jsonrpc") == "2.0");
    REQUIRE(listed.contains("result"));
    REQUIRE(!listed.contains("error"));
    const auto& tools = listed.at("result").at("tools");
    validateToolArray(tools, expectedToolCount);
    return RoleObservation{
        initializeResult.at("serverInfo").at("name").get<std::string>(),
        tools,
        instructions};
}

void validateStatus(
    const std::vector<Json>& frames,
    const std::int64_t id,
    const std::size_t expectedPresenceCount)
{
    const auto& status = responseFor(frames, id);
    REQUIRE(status.size() == 3U);
    REQUIRE(status.at("jsonrpc") == "2.0");
    REQUIRE(status.contains("result"));
    REQUIRE(!status.contains("error"));
    const auto& statusResult = status.at("result");
    REQUIRE(statusResult.at("isError") == false);
    const auto& structuredStatus = statusResult.at("structuredContent");
    REQUIRE(structuredStatus.is_object());
    REQUIRE(structuredStatus.at("presence_count") == expectedPresenceCount);
    REQUIRE(structuredStatus.at("workspace").at("project_root").is_string());
    REQUIRE(structuredStatus.at("workspace").at("project_id").is_string());
    REQUIRE(structuredStatus.at("workspace").at("binding_source") ==
            "registered_project");
    REQUIRE(structuredStatus.at("home_kind") == "application_data");
    REQUIRE(structuredStatus.at("home_is_project") == false);
    REQUIRE(structuredStatus.at("instruction_packages").at("packages").is_array());
    REQUIRE(structuredStatus.at("instruction_packages").at("available") == true);
    REQUIRE(structuredStatus.at("instruction_packages").at("error").is_null());
    REQUIRE(structuredStatus.at("instruction_packages").at("read_in_order") ==
            true);
    REQUIRE(structuredStatus.at("development_policy").at("read_tool") ==
            "project_policy.read");
    const auto& content = statusResult.at("content");
    REQUIRE(content.is_array());
    REQUIRE(content.size() == 1U);
    REQUIRE(content.front().at("type") == "text");
    const auto textStatus = Json::parse(
        content.front().at("text").get_ref<const std::string&>());
    REQUIRE(textStatus.at("presence_count") == expectedPresenceCount);
}

[[nodiscard]] bool canonicalUuid(const std::string_view value) noexcept
{
    if (value.size() != 36U) {
        return false;
    }
    for (std::size_t index{}; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const char character = value[index];
        const bool hexadecimal =
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
        if (!hexadecimal) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string utf8Path(const std::filesystem::path& path)
{
    const auto encoded = path.u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t character : encoded) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] std::string normalizedPathKey(std::string value)
{
    for (char& character : value) {
        if (character == '/') {
            character = '\\';
        } else if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    while (value.size() > 3U && value.back() == '\\') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] Json loadRegistry(const std::filesystem::path& home)
{
    const auto registryPath = home / "projects" / "registry.json";
    REQUIRE(std::filesystem::is_regular_file(registryPath));
    const auto bytes = std::filesystem::file_size(registryPath);
    REQUIRE(bytes > 0U);
    REQUIRE(bytes <= MaximumCapturedBytes);
    std::ifstream input{registryPath, std::ios::binary};
    REQUIRE(input.is_open());
    std::string content(static_cast<std::size_t>(bytes), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(static_cast<std::size_t>(input.gcount()) == content.size());

    return Json::parse(content.begin(), content.end());
}

void storeRegistry(
    const std::filesystem::path& home,
    const Json& registry)
{
    const auto registryPath = home / "projects" / "registry.json";
    const auto replacementPath = home / "projects" / "registry.process-test.json";
    {
        std::ofstream output{replacementPath, std::ios::binary | std::ios::trunc};
        REQUIRE(output.is_open());
        const auto serialized = registry.dump();
        output.write(
            serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        REQUIRE(output.good());
    }
    if (::MoveFileExW(
            replacementPath.c_str(),
            registryPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        throw win32Failure("MoveFileExW");
    }
}

[[nodiscard]] std::string registerWorkspaceProject(
    const std::filesystem::path& home,
    const std::filesystem::path& workspace)
{
    constexpr std::string_view projectId{
        "70000000-0000-4000-8000-000000000002"};
    auto registry = loadRegistry(home);
    REQUIRE(registry.is_object());
    REQUIRE(registry.at("projects").is_array());
    REQUIRE(registry.at("projects").size() == 1U);
    auto project = registry.at("projects").front();
    project["id"] = projectId;
    project["displayName"] = "Dynamic Project B";
    project["repositoryIdentity"] = nullptr;
    project["aliases"] = Json::array(
        {utf8Path(std::filesystem::canonical(workspace))});
    registry.at("projects").push_back(std::move(project));
    storeRegistry(home, registry);
    return std::string{projectId};
}

void validateRegistry(
    const std::filesystem::path& home,
    const std::vector<std::filesystem::path>& workspaces)
{
    const auto registry = loadRegistry(home);
    REQUIRE(registry.is_object());
    REQUIRE(registry.size() == 2U);
    REQUIRE(registry.at("schemaVersion") == 1U);
    const auto& projects = registry.at("projects");
    REQUIRE(projects.is_array());
    REQUIRE(projects.size() == workspaces.size());
    std::set<std::string, std::less<>> expectedAliases;
    for (const auto& workspace : workspaces) {
        expectedAliases.insert(normalizedPathKey(
            utf8Path(std::filesystem::canonical(workspace))));
    }
    std::set<std::string, std::less<>> observedIds;
    std::set<std::string, std::less<>> observedAliases;
    for (const auto& project : projects) {
        REQUIRE(project.is_object());
        REQUIRE(project.size() == 6U);
        REQUIRE(project.at("id").is_string());
        const auto& id = project.at("id").get_ref<const std::string&>();
        REQUIRE(canonicalUuid(id));
        REQUIRE(observedIds.insert(id).second);
        REQUIRE(project.at("displayName").is_string());
        REQUIRE(!project.at("displayName").get_ref<const std::string&>().empty());
        REQUIRE(project.at("repositoryIdentity").is_null() ||
                project.at("repositoryIdentity").is_string());
        REQUIRE(project.at("createdAt").is_string());
        REQUIRE(!project.at("createdAt").get_ref<const std::string&>().empty());
        REQUIRE(project.at("updatedAt").is_string());
        REQUIRE(!project.at("updatedAt").get_ref<const std::string&>().empty());
        const auto& aliases = project.at("aliases");
        REQUIRE(aliases.is_array());
        REQUIRE(aliases.size() == 1U);
        REQUIRE(aliases.front().is_string());
        const auto& alias = aliases.front().get_ref<const std::string&>();
        REQUIRE(!alias.empty());
        REQUIRE(observedAliases.insert(normalizedPathKey(alias)).second);
    }
    REQUIRE(observedIds.size() == workspaces.size());
    REQUIRE(observedAliases == expectedAliases);
}

void runPopulatedWorkspaceRegression(
    const std::filesystem::path& executable,
    const std::filesystem::path& root)
{
    const auto home = root / L"home";
    const auto workspace = root / L"workspace";
    const auto packageSource = root / L"unavailable-legacy-source";
    const auto policySource = root / L"policy-source";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(policySource);
    const std::string packageContent =
        "Follow the stored project instructions.\n" + std::string(17U * 1024U, 'x') +
        "\nThe final instruction survives content paging.";
    constexpr std::string_view policyContent{
        "Read the current code before changing behavior.\n"};
    const std::string revision(64U, 'd');
    const auto handshake = handshakeStream();
    std::string projectId;
    {
        McpProcessSession writer{
            executable, home, workspace, L"primary", L"populated-state-writer"};
        writer.send(handshake);
        static_cast<void>(observeRole(writer));
        writer.send(statusRequest(3));
        const auto status = successfulToolPayload(writer.awaitFrames(3U), 3);
        projectId = status.at("workspace").at("project_id").get<std::string>();
        writer.send(toolRequest(4, "project_memory.remember", Json{
            {"project_id", projectId}, {"kind", "project_instruction"},
            {"title", "START-HERE.md"}, {"summary", "Pinned legacy instruction text"},
            {"body", packageContent}, {"source_kind", "manager_instruction_package"}}));
        const auto file = successfulToolPayload(writer.awaitFrames(4U), 4);
        const auto manifest = Json{
            {"schema", "forge-instruction-package-v1"},
            {"package_name", "Persisted legacy instructions"},
            {"package_path", utf8Path(packageSource)}, {"revision", revision},
            {"files", Json::array({{{"path", "START-HERE.md"},
                {"record_id", file.at("record_id")}}})}};
        writer.send(toolRequest(5, "project_memory.remember", Json{
            {"project_id", projectId}, {"kind", "instruction_package"},
            {"title", "Persisted legacy instructions"},
            {"summary", "Legacy package saved before a queue existed"},
            {"body", manifest.dump()}, {"source_kind", "manager_instruction_package"}}));
        REQUIRE(successfulToolPayload(writer.awaitFrames(5U), 5).at("ok") == true);
        writer.finish(5U);
    }
    REQUIRE(!std::filesystem::exists(packageSource));

    // This is the durable schema produced by ProjectPolicyService::bind. The
    // separate serve processes must resolve the same project and profile.
    const auto policy = Json{{"schema", 2}, {"project", projectId},
        {"binding", {{"source", utf8Path(policySource)}, {"commit", nullptr},
            {"revision", revision}, {"entry_count", 1U}, {"coverage_gap_count", 0U},
            {"bound_at_utc_ms", 1'700'000'000'000LL}}},
        {"entries", Json::array({{{"path", "POLICY.md"}, {"kind", "file"},
            {"byte_length", policyContent.size()}, {"content_hash", nullptr},
            {"content", policyContent}, {"interpretation", "interpreted"},
            {"coverage_detail", nullptr}}})},
        {"rules", Json::array()}, {"findings", Json::array()},
        {"notifications", Json::array()}, {"history", Json::array()}};
    {
        std::ofstream output{home / L"memory" /
            std::filesystem::path{"project-policy-" + projectId + ".json"},
            std::ios::binary | std::ios::trunc};
        REQUIRE(output.is_open());
        output << policy.dump();
        output.close();
        REQUIRE(!output.fail());
    }

    std::string queueRowId;
    for (const auto role : {std::wstring_view{L"primary"}, std::wstring_view{L"fallback"}}) {
        McpProcessSession reader{
            executable, home, workspace, role, L"populated-state-reader"};
        reader.send(handshake);
        const auto observed = observeRole(reader);
        REQUIRE(observed.instructions.find(utf8Path(packageSource)) != std::string::npos);
        REQUIRE(observed.instructions.find(utf8Path(policySource)) != std::string::npos);
        reader.send(statusRequest(3));
        const auto status = successfulToolPayload(reader.awaitFrames(3U), 3);
        REQUIRE(status.at("workspace").at("project_id") == projectId);
        const auto& packages = status.at("instruction_packages");
        REQUIRE(packages.at("available") == true);
        REQUIRE(packages.at("count") == 1U);
        REQUIRE(packages.at("packages").at(0).at("path") == utf8Path(packageSource));
        const auto currentRowId = packages.at("packages").at(0)
            .at("queue_row_id").get<std::string>();
        REQUIRE(queueRowId.empty() || queueRowId == currentRowId);
        queueRowId = currentRowId;
        REQUIRE(status.at("development_policy").at("active") == true);
        REQUIRE(status.at("development_policy").at("source") == utf8Path(policySource));
        REQUIRE(status.at("development_policy").at("revision") == revision);

        reader.send(toolRequest(4, "instruction_package.read",
            Json{{"queue_row_id", queueRowId}}));
        const auto firstPage = successfulToolPayload(reader.awaitFrames(4U), 4);
        REQUIRE(firstPage.at("package").at("revision") == revision);
        REQUIRE(firstPage.at("entries").size() == 1U);
        const auto& firstEntry = firstPage.at("entries").at(0);
        REQUIRE(firstEntry.at("relative_path") == "START-HERE.md");
        REQUIRE(firstEntry.at("complete") == false);
        reader.send(toolRequest(5, "instruction_package.read", Json{
            {"queue_row_id", queueRowId}, {"path", "START-HERE.md"},
            {"offset", firstEntry.at("next_offset")}}));
        const auto secondPage = successfulToolPayload(reader.awaitFrames(5U), 5);
        REQUIRE(secondPage.at("entries").size() == 1U);
        const auto& secondEntry = secondPage.at("entries").at(0);
        REQUIRE(secondEntry.at("complete") == true);
        REQUIRE(firstEntry.at("content").get<std::string>() +
            secondEntry.at("content").get<std::string>() == packageContent);
        reader.send(toolRequest(6, "project_policy.read", Json::object()));
        const auto index = successfulToolPayload(reader.awaitFrames(6U), 6);
        REQUIRE(index.at("active") == true);
        REQUIRE(index.at("coverage").size() == 1U);
        reader.send(toolRequest(7, "project_policy.read", Json{{"path", "POLICY.md"}}));
        const auto document = successfulToolPayload(reader.awaitFrames(7U), 7);
        REQUIRE(document.at("content").get<std::string>() == policyContent);
        REQUIRE(document.at("complete") == true);
        reader.finish(7U);
    }

    McpProcessSession clu{
        executable, home, workspace, L"clu", L"populated-state-clu"};
    clu.send(handshake);
    const auto cluObserved = observeRole(clu, 5U);
    REQUIRE(cluObserved.instructions.find(utf8Path(packageSource)) != std::string::npos);
    REQUIRE(cluObserved.instructions.find(utf8Path(policySource)) != std::string::npos);
    clu.send(toolRequest(3, "project_policy.read", Json{{"path", "POLICY.md"}}));
    REQUIRE(successfulToolPayload(clu.awaitFrames(3U), 3)
        .at("content").get<std::string>() == policyContent);
    clu.finish(3U);

    {
        McpProcessSession deletion{
            executable, home, workspace, L"primary", L"populated-state-deletion"};
        deletion.send(handshake);
        static_cast<void>(observeRole(deletion));
        deletion.send(toolRequest(3, "project_memory.list_recent", Json{
            {"project_id", projectId}, {"kinds", Json::array({"instruction_package_queue"})},
            {"include_body", true}}));
        const auto queue = successfulToolPayload(deletion.awaitFrames(3U), 3);
        REQUIRE(queue.at("records").size() == 1U);
        deletion.send(toolRequest(4, "project_memory.remember", Json{
            {"project_id", projectId}, {"kind", "instruction_package_queue_order"},
            {"title", "Explicitly empty queue"}, {"summary", "Operator removed the last package"},
            {"body", Json{{"schema", "forge-instruction-package-order-v1"},
                {"project_id", projectId}, {"rows", Json::array()}}.dump()}}));
        REQUIRE(successfulToolPayload(deletion.awaitFrames(4U), 4).at("ok") == true);
        deletion.send(toolRequest(5, "project_memory.forget", Json{
            {"project_id", projectId}, {"id", queue.at("records").at(0).at("id")}}));
        REQUIRE(successfulToolPayload(deletion.awaitFrames(5U), 5).at("ok") == true);
        deletion.finish(5U);
    }
    {
        McpProcessSession empty{
            executable, home, workspace, L"primary", L"populated-state-empty"};
        empty.send(handshake);
        const auto observed = observeRole(empty);
        REQUIRE(observed.instructions.find(utf8Path(packageSource)) == std::string::npos);
        REQUIRE(observed.instructions.find(utf8Path(policySource)) != std::string::npos);
        empty.send(statusRequest(3));
        const auto status = successfulToolPayload(empty.awaitFrames(3U), 3);
        REQUIRE(status.at("instruction_packages").at("count") == 0U);
        REQUIRE(status.at("development_policy").at("active") == true);
        empty.send(toolRequest(4, "project_memory.list_recent", Json{
            {"project_id", projectId}, {"kinds", Json::array({"instruction_package"})}}));
        REQUIRE(successfulToolPayload(empty.awaitFrames(4U), 4).at("records").size() == 1U);
        empty.finish(4U);
    }

    const auto otherWorkspace = root / L"other-workspace";
    std::filesystem::create_directories(otherWorkspace);
    McpProcessSession unrelated{
        executable, home, otherWorkspace, L"primary", L"populated-state-unrelated"};
    unrelated.send(handshake);
    const auto unrelatedObserved = observeRole(unrelated);
    REQUIRE(unrelatedObserved.instructions.find(utf8Path(packageSource)) == std::string::npos);
    REQUIRE(unrelatedObserved.instructions.find(utf8Path(policySource)) == std::string::npos);
    unrelated.send(statusRequest(3));
    const auto otherStatus = successfulToolPayload(unrelated.awaitFrames(3U), 3);
    REQUIRE(otherStatus.at("workspace").at("project_id") != projectId);
    REQUIRE(otherStatus.at("instruction_packages").at("count") == 0U);
    REQUIRE(otherStatus.at("development_policy").at("active") == false);
    unrelated.finish(3U);
}

void runIsolatedManagerReviewerRegression(
    const std::filesystem::path& executable,
    const std::filesystem::path& root)
{
    const auto home = root / L"home";
    const auto workspace = root / L"workspace";
    std::filesystem::create_directories(home / L"config");
    std::filesystem::create_directories(workspace);
    std::uint16_t dashboardPort{};
    {
        WSADATA data{};
        REQUIRE(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
        struct WinsockCleanup final { ~WinsockCleanup() { static_cast<void>(::WSACleanup()); } } cleanup;
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(socket != INVALID_SOCKET);
        struct SocketCleanup final { SOCKET value; ~SocketCleanup() { static_cast<void>(::closesocket(value)); } } close{socket};
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        int size = sizeof(address);
        REQUIRE(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        dashboardPort = ntohs(address.sin_port);
    }
    {
        std::ofstream config{home / L"config" / L"config.json"};
        // Use an unavailable local endpoint so this process regression cannot
        // submit inference to the user's loaded model.
        config << Json{{"schema_version", 1}, {"dashboard", {{"port", dashboardPort}}}, {"local_model", {{"port", 1},
            {"model", "isolated-regression-model"}}}}.dump();
    }
    const auto handshake = handshakeStream();
    {
        McpProcessSession bootstrap{executable, home, workspace, L"fallback", L"isolated-reviewer-bootstrap"};
        bootstrap.send(handshake);
        static_cast<void>(bootstrap.awaitFrames(2U));
        bootstrap.finish(2U);
    }
    const auto managerExecutable = executable.parent_path() / L"ForgeConductor.Manager.exe";
    REQUIRE(std::filesystem::is_regular_file(managerExecutable));
    auto command = quoteWindowsArgument(managerExecutable.native()) + L" --alpha-root " + quoteWindowsArgument(home.native());
    std::vector<wchar_t> mutableCommand{command.begin(), command.end()};
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto managerLogPath = root / L"manager-process.log";
    UniqueHandle managerLog{::CreateFileW(managerLogPath.native().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    UniqueHandle managerInput{::CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(managerLog && managerInput);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = managerInput.get();
    startup.hStdOutput = managerLog.get();
    startup.hStdError = managerLog.get();
    PROCESS_INFORMATION process{};
    REQUIRE(::CreateProcessW(managerExecutable.native().c_str(), mutableCommand.data(), nullptr,
        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workspace.native().c_str(), &startup, &process));
    UniqueHandle managerProcess{process.hProcess};
    UniqueHandle managerThread{process.hThread};
    struct ManagerCleanup final {
        HANDLE process;
        ~ManagerCleanup() { static_cast<void>(terminateAndWait(process,
            std::chrono::duration_cast<std::chrono::milliseconds>(ForcedCleanupTimeout))); }
    } cleanup{managerProcess.get()};
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    std::unique_ptr<McpProcessSession> connected;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD exitCode{};
        REQUIRE(::GetExitCodeProcess(managerProcess.get(), &exitCode));
        if (exitCode != STILL_ACTIVE) {
            std::ifstream log{managerLogPath, std::ios::binary};
            const std::string details{std::istreambuf_iterator<char>{log}, std::istreambuf_iterator<char>{}};
            throw std::runtime_error{"The isolated Manager exited with " + std::to_string(exitCode) + ": " + details};
        }
        auto candidate = std::make_unique<McpProcessSession>(executable, home, workspace,
            L"fallback", L"isolated-reviewer-connect");
        candidate->send(handshake);
        static_cast<void>(candidate->awaitFrames(2U));
        candidate->send(statusRequest(3));
        const auto status = successfulToolPayload(candidate->awaitFrames(3U), 3);
        if (status.at("shell_execution").at("durable_across_mcp_reconnect").get<bool>()) {
            connected = std::move(candidate);
            break;
        }
        candidate->finish(3U);
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(connected);
    connected->send(toolRequest(4, "reviewer_start", Json{
        {"opening_message", "Review only this disposable process regression text."},
        {"authorization", "Authorized isolated process regression"}, {"mode", "text_only"},
        {"receive_timeout_sec", 1}}));
    const auto started = successfulToolPayload(connected->awaitFrames(4U), 4);
    REQUIRE(started.at("broker") == "persistent_manager");
    REQUIRE(started.at("manager_owned") == true);
    REQUIRE(started.at("read_only") == true);
    REQUIRE(started.at("receive_timeout_sec") == 1);
    const auto runId = started.at("run_id").get<std::string>();
    connected->finish(4U);
    McpProcessSession resumed{executable, home, workspace, L"fallback", L"isolated-reviewer-reconnect"};
    resumed.send(handshake);
    static_cast<void>(resumed.awaitFrames(2U));
    resumed.send(toolRequest(3, "reviewer_status", Json{{"run_id", runId}}));
    const auto restored = successfulToolPayload(resumed.awaitFrames(3U), 3);
    REQUIRE(restored.at("broker") == "persistent_manager");
    REQUIRE(restored.at("run_id") == runId);
    REQUIRE(restored.at("receive_timeout_sec") == 1);
    REQUIRE(restored.at("gate_approved") == false);
    resumed.send(toolRequest(4, "reviewer_cancel", Json{{"run_id", runId}}));
    REQUIRE(successfulToolPayload(resumed.awaitFrames(4U), 4).at("manager_owned") == true);
    resumed.finish(4U);
}

void runPolicyPagingRegression(
    const std::filesystem::path& executable,
    const std::filesystem::path& root)
{
    constexpr std::size_t SerializedPageLimit = 32U * 1024U;
    const auto home = root / L"home";
    const auto workspace = root / L"workspace";
    const auto otherWorkspace = root / L"other-workspace";
    const auto packageSource = root / L"saved-package-source";
    const auto policySource = root / L"policy-source";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(otherWorkspace);
    std::filesystem::create_directories(policySource);
    const auto handshake = handshakeStream();
    const std::string revision(64U, 'e');
    const std::string unit = std::string{"\x01\"\\\n"} +
        "\xE6\xB8\xAC\xE8\xA9\xA6\xF0\x9F\xA7\xAA";
    std::string documentContent;
    std::string packageContent;
    for (std::size_t index{}; index < 4096U; ++index) documentContent += unit;
    for (std::size_t index{}; index < 2300U; ++index) packageContent += unit;
    documentContent += "the complete policy ending";
    packageContent += "the complete package ending";
    std::string projectId;
    std::string queueRowId;
    {
        McpProcessSession writer{
            executable, home, workspace, L"fallback", L"policy-paging-writer"};
        writer.send(handshake);
        static_cast<void>(observeRole(writer));
        writer.send(statusRequest(3));
        projectId = successfulToolPayload(writer.awaitFrames(3U), 3)
            .at("workspace").at("project_id").get<std::string>();
        writer.send(toolRequest(4, "project_memory.remember", Json{
            {"project_id", projectId}, {"kind", "project_instruction"},
            {"title", "ESCAPED.md"}, {"summary", "Pinned escaped Unicode text"},
            {"body", packageContent}, {"source_kind", "manager_instruction_package"}}));
        const auto file = successfulToolPayload(writer.awaitFrames(4U), 4);
        const auto manifest = Json{{"schema", "forge-instruction-package-v1"},
            {"package_name", "Escaped instruction package"},
            {"package_path", utf8Path(packageSource)}, {"revision", revision},
            {"files", Json::array({{{"path", "ESCAPED.md"},
                {"record_id", file.at("record_id")}}})}};
        writer.send(toolRequest(5, "project_memory.remember", Json{
            {"project_id", projectId}, {"kind", "instruction_package"},
            {"title", "Escaped instruction package"}, {"summary", "Native envelope regression"},
            {"body", manifest.dump()}, {"source_kind", "manager_instruction_package"}}));
        REQUIRE(successfulToolPayload(writer.awaitFrames(5U), 5).at("ok") == true);
        writer.send(statusRequest(6));
        const auto savedPackageStatus = successfulToolPayload(writer.awaitFrames(6U), 6);
        queueRowId = savedPackageStatus.at("instruction_packages").at("packages").at(0)
            .at("queue_row_id").get<std::string>();
        writer.finish(6U);
    }
    Json entries = Json::array();
    Json expectedCoverage = Json::array();
    for (std::size_t index{}; index < 270U; ++index) {
        const auto path = index == 0U ? std::string{"ESCAPED-POLICY.md"} :
            "policy/" + std::to_string(index) + "-" + std::string(160U, 'p') + ".md";
        const auto byteLength = index == 0U ? documentContent.size() : 0U;
        auto coverage = Json{{"path", path}, {"kind", "file"},
            {"byte_length", byteLength}, {"content_hash", nullptr},
            {"interpretation", "interpreted"}, {"coverage_detail", nullptr}};
        expectedCoverage.push_back(coverage);
        coverage["content"] = index == 0U ? documentContent : std::string{};
        entries.push_back(std::move(coverage));
    }
    Json findings = Json::array();
    Json expectedGuidance = Json::array();
    std::string guidanceContent;
    for (std::size_t index{}; index < 35U; ++index) guidanceContent += unit;
    for (std::size_t index{}; index < 120U; ++index) {
        const auto findingId = "fixture-finding-" + std::to_string(index);
        const auto ruleId = "fixture-rule-" + std::to_string(index);
        findings.push_back({{"finding_id", findingId}, {"rule_id", ruleId},
            {"state", "open"}, {"severity", "warning"}, {"evidence", guidanceContent},
            {"requested_correction", "Read the complete retained guidance."}});
        expectedGuidance.push_back({{"finding_id", findingId}, {"rule_id", ruleId},
            {"evidence", guidanceContent},
            {"required_correction", "Read the complete retained guidance."}});
    }
    auto policy = Json{{"schema", 2}, {"project", projectId},
        {"binding", {{"source", utf8Path(policySource)}, {"commit", nullptr},
            {"revision", revision}, {"entry_count", entries.size()},
            {"coverage_gap_count", 0U}, {"bound_at_utc_ms", 1'700'000'000'000LL}}},
        {"entries", entries}, {"rules", Json::array()}, {"findings", findings},
        {"notifications", Json::array({{{"notification_id", "fixture-pending-notification"},
            {"finding_id", "fixture-finding-0"}, {"state", "pending"},
            {"rule_id", "fixture-rule-0"}, {"evidence", guidanceContent},
            {"required_correction", "Retain this pending notification."},
            {"created_at_utc_ms", 1'700'000'000'000LL}}})},
        {"history", Json::array()}};
    const auto policyPath = home / L"memory" /
        std::filesystem::path{"project-policy-" + projectId + ".json"};
    const auto savePolicy = [&] {
        std::ofstream output{policyPath, std::ios::binary | std::ios::trunc};
        REQUIRE(output.is_open());
        output << policy.dump();
        output.close();
        REQUIRE(!output.fail());
    };
    savePolicy();
    const auto reject = [](const std::vector<Json>& frames, const std::int64_t id,
                           const std::string_view code) {
        const auto& result = responseFor(frames, id).at("result");
        if (result.at("isError") != true) {
            throw std::runtime_error{"Policy paging rejection id=" + std::to_string(id) +
                " expected=" + std::string{code} + " actual=" + result.dump().substr(0U, 2048U)};
        }
        REQUIRE(result.at("isError") == true);
        REQUIRE(result.at("structuredContent").at("code") == std::string{code});
    };
    Json receivedCoverage = Json::array();
    Json receivedGuidance = Json::array();
    const auto retainPage = [&](const Json& page) {
        REQUIRE(page.dump().size() <= SerializedPageLimit);
        REQUIRE(page.at("clu_governance_notifications_deferred") == true);
        REQUIRE(page.at("clu_governance_notifications_read_tool") == "clu.findings");
        REQUIRE(page.at("active") == true);
        REQUIRE(page.at("entry_count") == 270U);
        REQUIRE(page.at("open_findings") == 120U);
        REQUIRE(page.at("coverage_offset") == receivedCoverage.size());
        REQUIRE(page.at("guidance_offset") == receivedGuidance.size());
        for (const auto& entry : page.at("coverage")) receivedCoverage.push_back(entry);
        for (const auto& guidance : page.at("agent_guidance")) receivedGuidance.push_back(guidance);
        REQUIRE(page.at("complete").get<bool>() == page.at("next_cursor").is_null());
    };
    std::string firstCursor;
    {
        McpProcessSession predecessor{
            executable, home, workspace, L"fallback", L"policy-paging-predecessor"};
        predecessor.send(handshake);
        static_cast<void>(observeRole(predecessor));
        predecessor.send(toolRequest(3, "project_policy.read", Json::object()));
        const auto first = successfulToolPayload(predecessor.awaitFrames(3U), 3);
        retainPage(first);
        REQUIRE(first.at("complete") == false);
        REQUIRE(!first.at("coverage").empty());
        REQUIRE(first.at("coverage").size() < expectedCoverage.size());
        firstCursor = first.at("next_cursor").get<std::string>();
        predecessor.finish(3U);
    }
    {
        McpProcessSession successor{
            executable, home, workspace, L"fallback", L"policy-paging-successor"};
        successor.send(handshake);
        static_cast<void>(observeRole(successor));
        std::int64_t id = 2;
        std::string cursor = firstCursor;
        bool complete{};
        std::size_t pages = 1U;
        while (!complete) {
            REQUIRE(pages < 24U);
            successor.send(toolRequest(++id, "project_policy.read", Json{{"cursor", cursor}}));
            const auto page = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
            retainPage(page);
            complete = page.at("complete").get<bool>();
            if (!complete) {
                const auto next = page.at("next_cursor").get<std::string>();
                REQUIRE(next != cursor);
                cursor = next;
            }
            ++pages;
        }
        REQUIRE(pages > 2U);
        REQUIRE(receivedCoverage == expectedCoverage);
        REQUIRE(receivedGuidance == expectedGuidance);

        std::string reconstructed;
        std::size_t documentPages{};
        while (reconstructed.size() < documentContent.size()) {
            REQUIRE(documentPages < 16U);
            successor.send(toolRequest(++id, "project_policy.read", Json{
                {"path", "ESCAPED-POLICY.md"}, {"offset", reconstructed.size()}}));
            const auto page = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
            REQUIRE(page.dump().size() <= SerializedPageLimit);
            REQUIRE(page.at("clu_governance_notifications_deferred") == true);
            REQUIRE(page.at("clu_governance_notifications_read_tool") == "clu.findings");
            const auto content = page.at("content").get<std::string>();
            REQUIRE(!content.empty());
            reconstructed += content;
            REQUIRE(page.at("next_offset") == reconstructed.size());
            REQUIRE(page.at("complete").get<bool>() == (reconstructed.size() == documentContent.size()));
            ++documentPages;
        }
        REQUIRE(documentPages > 1U);
        REQUIRE(reconstructed == documentContent);

        const auto& rowId = queueRowId;
        std::string reconstructedPackage;
        std::size_t packagePages{};
        while (reconstructedPackage.size() < packageContent.size()) {
            REQUIRE(packagePages < 16U);
            successor.send(toolRequest(++id, "instruction_package.read", Json{
                {"queue_row_id", rowId}, {"path", "ESCAPED.md"},
                {"offset", reconstructedPackage.size()}}));
            const auto page = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
            REQUIRE(page.dump().size() <= SerializedPageLimit);
            REQUIRE(page.at("clu_governance_notifications_deferred") == true);
            REQUIRE(page.at("clu_governance_notifications_read_tool") == "clu.findings");
            REQUIRE(page.at("entries").size() == 1U);
            const auto& entry = page.at("entries").at(0);
            const auto content = entry.at("content").get<std::string>();
            REQUIRE(!content.empty());
            reconstructedPackage += content;
            REQUIRE(entry.at("next_offset") == reconstructedPackage.size());
            REQUIRE(entry.at("complete").get<bool>() == (reconstructedPackage.size() == packageContent.size()));
            ++packagePages;
        }
        REQUIRE(packagePages > 1U);
        REQUIRE(reconstructedPackage == packageContent);

        const std::string escapedLineUnit = std::string{"\x01\"\\\x02"} +
            "\xE6\xB8\xAC\xE8\xA9\xA6\xF0\x9F\xA7\xAA";
        std::string largeEscapedLine;
        for (std::size_t index{}; index < 4096U; ++index) largeEscapedLine += escapedLineUnit;
        largeEscapedLine += "\nshort line\nfinal line";
        const std::string largeAsciiLine = std::string(96U * 1024U, 'a') +
            "\nshort line\nfinal line";
        std::string shortLines;
        for (std::size_t index{}; index < 500U; ++index) {
            shortLines += std::to_string(index) + ":";
            for (std::size_t column{}; column < 12U; ++column) shortLines += escapedLineUnit;
            shortLines += '\n';
        }
        const auto readFileCompletely = [&](const std::string& name,
                                            const std::string& source,
                                            const bool expectBytePaging) {
            {
                std::ofstream file{workspace / name, std::ios::binary};
                REQUIRE(file.is_open());
                file << source;
                file.close();
                REQUIRE(!file.fail());
            }
            std::string reconstructedFile;
            Json arguments{{"path", name}};
            const auto totalLines = static_cast<std::size_t>(
                std::count(source.begin(), source.end(), '\n')) + 1U;
            std::size_t filePages{};
            bool sawBytePage{};
            bool sawLinePage{};
            bool hasMore = true;
            while (hasMore) {
                REQUIRE(filePages < 32U);
                const auto sourceStart = reconstructedFile.size();
                successor.send(toolRequest(++id, "fs_read", arguments));
                const auto page = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
                REQUIRE(page.dump().size() <= SerializedPageLimit);
                REQUIRE(page.at("clu_governance_notifications_deferred") == true);
                REQUIRE(page.at("clu_governance_notifications_read_tool") == "clu.findings");
                REQUIRE(page.at("size") == source.size());
                REQUIRE(page.at("total_lines") == totalLines);
                const auto content = page.at("content").get<std::string>();
                REQUIRE(!content.empty());
                REQUIRE(source.compare(sourceStart, content.size(), content) == 0);
                const auto expectedStartLine = static_cast<std::size_t>(std::count(
                    source.begin(), source.begin() + static_cast<std::ptrdiff_t>(sourceStart), '\n')) + 1U;
                REQUIRE(page.at("start_line") == expectedStartLine);
                reconstructedFile += content;
                hasMore = page.at("has_more").get<bool>();
                if (!page.at("byte_offset").is_null()) {
                    sawBytePage = true;
                    REQUIRE(page.at("byte_offset") == sourceStart);
                    const auto pageNewlines = static_cast<std::size_t>(
                        std::count(content.begin(), content.end(), '\n'));
                    const auto expectedEndLine = expectedStartLine + pageNewlines -
                        (content.ends_with('\n') ? 1U : 0U);
                    REQUIRE(page.at("end_line") == expectedEndLine);
                    REQUIRE(page.at("line_count") == expectedEndLine - expectedStartLine + 1U);
                    REQUIRE(page.at("next_offset").is_null());
                    if (hasMore) {
                        REQUIRE(page.at("next_byte_offset") == reconstructedFile.size());
                        arguments = Json{{"path", name}, {"byte_offset", page.at("next_byte_offset")}};
                    } else {
                        REQUIRE(page.at("next_byte_offset").is_null());
                    }
                } else {
                    sawLinePage = true;
                    REQUIRE(page.at("next_byte_offset").is_null());
                    const auto lineCount = static_cast<std::size_t>(
                        std::count(content.begin(), content.end(), '\n')) + 1U;
                    REQUIRE(page.at("line_count") == lineCount);
                    REQUIRE(page.at("end_line") == expectedStartLine + lineCount - 1U);
                    if (hasMore) {
                        REQUIRE(page.at("next_offset") == expectedStartLine + lineCount);
                        REQUIRE(source.at(reconstructedFile.size()) == '\n');
                        reconstructedFile += '\n';
                        arguments = Json{{"path", name}, {"offset", page.at("next_offset")},
                            {"length", totalLines}};
                    }
                }
                ++filePages;
            }
            REQUIRE(filePages > 1U);
            REQUIRE(reconstructedFile == source);
            REQUIRE(sawBytePage == expectBytePaging);
            if (!expectBytePaging) REQUIRE(sawLinePage);
        };
        readFileCompletely("large-ascii-line.txt", largeAsciiLine, true);
        readFileCompletely("large-escaped-unicode-line.txt", largeEscapedLine, true);
        readFileCompletely("bounded-whole-lines.txt", shortLines, false);

        const auto nearEndPath = workspace / "near-limit-eof.txt";
        std::size_t nearEndBytes = SerializedPageLimit;
        Json beforeProjection{{"ok", true}, {"path", utf8Path(nearEndPath)},
            {"content", std::string(nearEndBytes, 'a')}, {"size", nearEndBytes},
            {"total_lines", 1U}, {"start_line", 1U}, {"end_line", 1U}, {"line_count", 1U},
            {"has_more", false}, {"next_offset", nullptr}, {"byte_offset", nullptr},
            {"next_byte_offset", nullptr},
            {"note", "Complete file contents (1 lines). Do not re-read this path unless the file changes."},
            {"clu_governance_notifications_deferred", true},
            {"clu_governance_notifications_read_tool", "clu.findings"}};
        while (beforeProjection.dump().size() > SerializedPageLimit + 1U) {
            REQUIRE(nearEndBytes > 0U);
            --nearEndBytes;
            beforeProjection["content"] = std::string(nearEndBytes, 'a');
            beforeProjection["size"] = nearEndBytes;
        }
        REQUIRE(beforeProjection.dump().size() == SerializedPageLimit + 1U);
        const std::string nearEndContent(nearEndBytes, 'a');
        {
            std::ofstream file{nearEndPath, std::ios::binary};
            REQUIRE(file.is_open());
            file << nearEndContent;
            file.close();
            REQUIRE(!file.fail());
        }
        successor.send(toolRequest(++id, "fs_read", Json{{"path", "near-limit-eof.txt"}}));
        const auto nearEnd = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
        REQUIRE(nearEnd.dump().size() <= SerializedPageLimit);
        REQUIRE(nearEnd.at("content").get<std::string>() == nearEndContent);
        REQUIRE(nearEnd.at("has_more") == false);
        REQUIRE(nearEnd.at("next_offset").is_null());
        REQUIRE(nearEnd.at("next_byte_offset").is_null());
        REQUIRE(nearEnd.at("total_lines") == 1U);
        REQUIRE(nearEnd.at("end_line") == 1U);
        REQUIRE(nearEnd.at("line_count") == 1U);

        const auto rejectedRequest = [&](const Json& arguments, const std::string_view code) {
            successor.send(toolRequest(++id, "project_policy.read", arguments));
            reject(successor.awaitFrames(static_cast<std::size_t>(id)), id, code);
        };
        rejectedRequest(Json{{"cursor", "not a cursor"}}, "invalid_request");
        rejectedRequest(Json{{"cursor", firstCursor}, {"path", "ESCAPED-POLICY.md"}}, "invalid_request");
        auto invalidOffset = Json::parse(firstCursor);
        invalidOffset["coverage_offset"] = expectedCoverage.size() + 1U;
        rejectedRequest(Json{{"cursor", invalidOffset.dump()}}, "invalid_request");
        const auto continuationByte = documentContent.find("\xE6\xB8\xAC") + 1U;
        rejectedRequest(Json{{"path", "ESCAPED-POLICY.md"}, {"offset", continuationByte}}, "invalid_request");
        auto endCursor = Json::parse(firstCursor);
        endCursor["coverage_offset"] = expectedCoverage.size();
        endCursor["guidance_offset"] = expectedGuidance.size();
        successor.send(toolRequest(++id, "project_policy.read", Json{{"cursor", endCursor.dump()}}));
        const auto end = successfulToolPayload(successor.awaitFrames(static_cast<std::size_t>(id)), id);
        REQUIRE(end.at("complete") == true);
        REQUIRE(end.at("coverage").empty());
        REQUIRE(end.at("agent_guidance").empty());
        REQUIRE(end.at("next_cursor").is_null());
        successor.finish(static_cast<std::size_t>(id));
    }
    {
        McpProcessSession unrelated{
            executable, home, otherWorkspace, L"fallback", L"policy-paging-unrelated"};
        unrelated.send(handshake);
        static_cast<void>(observeRole(unrelated));
        unrelated.send(toolRequest(3, "project_policy.read", Json{{"cursor", firstCursor}}));
        reject(unrelated.awaitFrames(3U), 3, "project_scope_mismatch");
        unrelated.finish(3U);
    }
    {
        std::ifstream saved{policyPath, std::ios::binary};
        REQUIRE(saved.is_open());
        const auto durable = Json::parse(saved);
        REQUIRE(durable.at("notifications").size() == 1U);
        REQUIRE(durable.at("notifications").at(0).at("state") == "pending");
        REQUIRE(!durable.at("notifications").at(0).contains("delivered_at_utc_ms"));
    }
    policy["binding"]["revision"] = std::string(64U, 'f');
    savePolicy();
    {
        McpProcessSession stale{
            executable, home, workspace, L"fallback", L"policy-paging-stale"};
        stale.send(handshake);
        static_cast<void>(observeRole(stale));
        stale.send(toolRequest(3, "project_policy.read", Json{{"cursor", firstCursor}}));
        reject(stale.awaitFrames(3U), 3, "conflict");
        stale.finish(3U);
    }
    policy["binding"]["revision"] = revision;
    policy["entries"][1]["path"] = "same-revision-changed-inventory.md";
    savePolicy();
    {
        McpProcessSession changed{
            executable, home, workspace, L"fallback", L"policy-paging-changed-index"};
        changed.send(handshake);
        static_cast<void>(observeRole(changed));
        changed.send(toolRequest(3, "project_policy.read", Json{{"cursor", firstCursor}}));
        reject(changed.awaitFrames(3U), 3, "conflict");
        changed.finish(3U);
    }
    policy["entries"][1] = entries.at(1);
    policy["findings"][0]["requested_correction"] = "Changed guidance with the same policy revision.";
    savePolicy();
    {
        McpProcessSession changedGuidance{
            executable, home, workspace, L"fallback", L"policy-paging-changed-guidance"};
        changedGuidance.send(handshake);
        static_cast<void>(observeRole(changedGuidance));
        changedGuidance.send(toolRequest(3, "project_policy.read", Json{{"cursor", firstCursor}}));
        reject(changedGuidance.awaitFrames(3U), 3, "conflict");
        changedGuidance.finish(3U);
    }
    policy["entries"][0]["path"] = std::string(40U * 1024U, 'p');
    savePolicy();
    {
        McpProcessSession oversized{
            executable, home, workspace, L"fallback", L"policy-paging-oversized-entry"};
        oversized.send(handshake);
        static_cast<void>(observeRole(oversized));
        oversized.send(toolRequest(3, "project_policy.read", Json::object()));
        reject(oversized.awaitFrames(3U), 3, "payload_too_large");
        oversized.finish(3U);
    }
    policy["entries"] = Json::array();
    policy["findings"] = Json::array();
    policy["binding"]["entry_count"] = 0U;
    savePolicy();
    {
        McpProcessSession empty{
            executable, home, workspace, L"fallback", L"policy-paging-empty"};
        empty.send(handshake);
        static_cast<void>(observeRole(empty));
        empty.send(toolRequest(3, "project_policy.read", Json::object()));
        const auto index = successfulToolPayload(empty.awaitFrames(3U), 3);
        REQUIRE(index.at("complete") == true);
        REQUIRE(index.at("coverage").empty());
        REQUIRE(index.at("agent_guidance").empty());
        REQUIRE(index.at("next_cursor").is_null());
        REQUIRE(index.at("entry_count") == 0U);
        REQUIRE(index.dump().size() <= SerializedPageLimit);
        empty.finish(3U);
    }
}

void runFragmentedToolResultRegression(
    const std::filesystem::path& executable,
    const std::filesystem::path& root)
{
    const auto home = root / L"home";
    const auto workspace = root / L"workspace";
    const auto outside = root / L"outside";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(outside);
    const auto slashVariants = [](std::string native) {
        auto forward = native;
        std::replace(forward.begin(), forward.end(), '\\', '/');
        auto mixed = forward;
        bool alternate{};
        for (auto& character : mixed) {
            if (character == '/') {
                alternate = !alternate;
                if (alternate) character = '\\';
            }
        }
        return std::vector<std::string>{std::move(native), std::move(forward), std::move(mixed)};
    };
    const auto cwdVariants = slashVariants(utf8Path(workspace));
    const auto readPath = workspace / L"slash-read.txt";
    const std::string readText = "Exact content through separator variants.\n";
    {
        std::ofstream file{readPath, std::ios::binary};
        REQUIRE(file.is_open());
        file << readText;
        REQUIRE(file.good());
    }
    McpProcessSession reader{
        executable, home, workspace, L"fallback", L"fragmented-tool-result"};
    reader.send(handshakeStream());
    static_cast<void>(observeRole(reader));
    std::int64_t id = 3;
    for (const auto& cwd : cwdVariants) {
        reader.send(toolRequest(id, "shell_exec", Json{
            {"command", "Write-Output ('Z' * 60000)"}, {"timeout_sec", 15U}, {"cwd", cwd}}));
        const auto frames = reader.awaitFrames(static_cast<std::size_t>(id));
        const auto result = successfulToolPayload(frames, id);
        if (!result.value("ok", false)) {
            throw std::runtime_error{"Explicit shell cwd rejected: " + cwd + " => " + result.dump()};
        }
        REQUIRE(responseFor(frames, id).at("result").at("content").size() > 1U);
        REQUIRE(result.at("ok") == true);
        REQUIRE(result.at("exit_code") == 0);
        REQUIRE(result.at("stdout_truncated") == false);
        REQUIRE(normalizedPathKey(result.at("cwd").get<std::string>()) ==
                normalizedPathKey(utf8Path(workspace)));
        const auto output = result.at("stdout").get<std::string>();
        REQUIRE(output == std::string(60000U, 'Z') + "\r\n" ||
                output == std::string(60000U, 'Z') + "\n");
        REQUIRE(result.dump().size() > 50000U);
        ++id;
    }
    for (const auto& path : slashVariants(utf8Path(readPath))) {
        reader.send(toolRequest(id, "fs_read", Json{{"path", path}}));
        const auto result = successfulToolPayload(
            reader.awaitFrames(static_cast<std::size_t>(id)), id);
        REQUIRE(result.at("content") == readText);
        REQUIRE(result.at("has_more") == false);
        REQUIRE(normalizedPathKey(result.at("path").get<std::string>()) ==
                normalizedPathKey(utf8Path(readPath)));
        ++id;
    }
    const std::vector<std::pair<std::string, std::string>> unsafe{
        {slashVariants(utf8Path(outside)).at(1), "path_outside_authority"},
        {"//?/" + cwdVariants.at(1), "path_outside_authority"},
        {cwdVariants.at(1) + "/./", "invalid_request"}};
    for (const auto& [cwd, code] : unsafe) {
        reader.send(toolRequest(id, "shell_exec", Json{
            {"command", "Write-Output 'UNSAFE-CWD-EXECUTED'"}, {"cwd", cwd}}));
        const auto frames = reader.awaitFrames(static_cast<std::size_t>(id));
        const auto& rejected = responseFor(frames, id).at("result").at("structuredContent");
        REQUIRE(rejected.at("ok") == false);
        REQUIRE(rejected.at("code") == code);
        REQUIRE(rejected.value("stdout", std::string{}).find("UNSAFE-CWD-EXECUTED") == std::string::npos);
        ++id;
    }
    reader.finish(static_cast<std::size_t>(id - 1));
}

void runAgentLifecycleRegression(
    const std::filesystem::path& executable,
    const std::filesystem::path& root)
{
    const auto home = root / L"home";
    const auto workspace = root / L"workspace";
    const auto otherWorkspace = root / L"other-workspace";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(otherWorkspace);
    const auto handshake = handshakeStream();
    std::string sessionId;
    std::string projectId;
    std::string originalClientId;
    {
        McpProcessSession owner{
            executable, home, workspace, L"fallback", L"agent-lifecycle-owner"};
        owner.send(handshake);
        static_cast<void>(observeRole(owner));
        owner.send(statusRequest(3));
        const auto initial = successfulToolPayload(owner.awaitFrames(3U), 3);
        projectId = initial.at("workspace").at("project_id").get<std::string>();
        originalClientId = initial.at("client_id").get<std::string>();
        owner.send(toolRequest(4, "agent_run_start", Json{
            {"agent_id", "debug"}, {"goal", "Verify durable agent lifecycle"},
            {"cwd", utf8Path(workspace)}}));
        const auto started = successfulToolPayload(owner.awaitFrames(4U), 4);
        sessionId = started.at("session_id").get<std::string>();
        REQUIRE(canonicalUuid(sessionId));
        REQUIRE(started.at("session").at("client_id") == originalClientId);
        REQUIRE(started.at("session").at("status") == "open");
        owner.send(toolRequest(5, "agent_run_status", Json{{"session_id", sessionId}}));
        const auto open = successfulToolPayload(owner.awaitFrames(5U), 5);
        REQUIRE(open.at("session").at("id") == sessionId);
        REQUIRE(open.at("must_complete") == true);
        REQUIRE(open.at("reattached") == false);
        REQUIRE(open.at("active_binding").at("session_id") == sessionId);

        {
            McpProcessSession unrelated{
                executable, home, otherWorkspace, L"fallback", L"agent-lifecycle-unrelated"};
            unrelated.send(handshake);
            static_cast<void>(observeRole(unrelated));
            unrelated.send(statusRequest(3));
            const auto other = successfulToolPayload(unrelated.awaitFrames(3U), 3);
            REQUIRE(other.at("workspace").at("project_id") != projectId);
            unrelated.send(toolRequest(4, "agent_run_status", Json{{"session_id", sessionId}}));
            const auto frames = unrelated.awaitFrames(4U);
            const auto& rejected = responseFor(frames, 4).at("result");
            REQUIRE(rejected.at("isError") == true);
            REQUIRE(rejected.at("structuredContent").at("code") == "unauthorized");
            REQUIRE(rejected.at("structuredContent").at("message").get<std::string>()
                .find("durable run project") != std::string::npos);
            unrelated.finish(4U);
        }
        owner.send(toolRequest(6, "agent_run_status", Json{{"session_id", sessionId}}));
        const auto retained = successfulToolPayload(owner.awaitFrames(6U), 6);
        REQUIRE(retained.at("session").at("client_id") == originalClientId);
        REQUIRE(retained.at("must_complete") == true);
        owner.finish(6U);
    }
    {
        McpProcessSession successor{
            executable, home, workspace, L"fallback", L"agent-lifecycle-successor"};
        successor.send(handshake);
        static_cast<void>(observeRole(successor));
        successor.send(statusRequest(3));
        const auto initial = successfulToolPayload(successor.awaitFrames(3U), 3);
        REQUIRE(initial.at("workspace").at("project_id") == projectId);
        const auto clientId = initial.at("client_id").get<std::string>();
        REQUIRE(clientId != originalClientId);
        successor.send(toolRequest(4, "agent_run_status", Json{{"session_id", sessionId}}));
        const auto reattached = successfulToolPayload(successor.awaitFrames(4U), 4);
        REQUIRE(reattached.at("session").at("id") == sessionId);
        REQUIRE(reattached.at("session").at("client_id") == clientId);
        REQUIRE(reattached.at("reattached") == true);
        REQUIRE(reattached.at("must_complete") == true);
        REQUIRE(reattached.at("active_binding").at("session_id") == sessionId);
        successor.send(toolRequest(5, "agent_run_complete", Json{
            {"session_id", sessionId}, {"report", {
                {"symptom", "Status previously rejected a just-started run"},
                {"repro", "Actual MCP start, status, restart, reattach, completion"},
                {"root_cause", "Lifecycle capabilities omitted resolved project scope"},
                {"fix", "Start and status require registered project scope"},
                {"verify", "Cross-project status rejected; same-project lifecycle succeeds"}}}}));
        const auto completed = successfulToolPayload(successor.awaitFrames(5U), 5);
        REQUIRE(completed.at("schema_complete") == true);
        REQUIRE(completed.at("missing_schema_keys").empty());
        REQUIRE(completed.at("session").at("status") == "closed");
        successor.send(toolRequest(6, "agent_run_status", Json{{"session_id", sessionId}}));
        const auto closed = successfulToolPayload(successor.awaitFrames(6U), 6);
        REQUIRE(closed.at("session").at("id") == sessionId);
        REQUIRE(closed.at("session").at("status") == "closed");
        REQUIRE(closed.at("must_complete") == false);
        REQUIRE(closed.at("active_binding").is_null());
        successor.finish(6U);
    }
}

void run(
    const std::filesystem::path& executable,
    const std::filesystem::path& goldenPath)
{
    REQUIRE(std::filesystem::is_regular_file(executable));
    const auto golden = loadGolden(goldenPath);

    TemporaryDirectory temporary;
    const auto sharedRoot = temporary.root() / L"shared-\u5171\u6709";
    const auto home = sharedRoot / L"home-\u4e3b";
    const auto workspace = sharedRoot / L"workspace-\u4f5c\u696d";
    std::filesystem::create_directories(home);
    std::filesystem::create_directories(workspace);

    McpProcessSession primary{
        executable,
        home,
        workspace,
        L"primary",
        L"p14-shared-root-primary"};
    McpProcessSession fallback{
        executable,
        home,
        workspace,
        L"fallback",
        L"p14-shared-root-fallback"};

    const auto handshake = handshakeStream();
    primary.send(handshake);
    fallback.send(handshake);
    const auto primaryObservation = observeRole(primary);
    const auto fallbackObservation = observeRole(fallback);

    REQUIRE(primaryObservation.serverName == "forge-conductor");
    REQUIRE(fallbackObservation.serverName == "forge-conductor-fallback");
    REQUIRE(primaryObservation.serverName != fallbackObservation.serverName);
    REQUIRE(primaryObservation.tools == fallbackObservation.tools);
    REQUIRE(primaryObservation.tools == golden.at("tools"));

    McpProcessSession clu{
        executable,
        home,
        workspace,
        L"clu",
        L"p14-shared-root-clu"};
    clu.send(handshake);
    const auto cluObservation = observeRole(clu, 5U);
    REQUIRE(cluObservation.serverName == "forge-conductor-clu");
    std::vector<std::string> cluNames;
    for (const auto& tool : cluObservation.tools) {
        cluNames.push_back(tool.at("name").get<std::string>());
    }
    REQUIRE((cluNames ==
        std::vector<std::string>{
            "clu.evaluate", "clu.export_log", "clu.findings", "clu.resolve",
            "project_policy.read"}));
    clu.send(toolRequest(3, "clu.findings", Json::object()));
    const auto cluFrames = clu.awaitFrames(3U);
    const auto& cluResult = responseFor(cluFrames, 3).at("result");
    REQUIRE(cluResult.at("isError") == false);
    REQUIRE(cluResult.at("structuredContent").at("active") == false);
    REQUIRE(cluResult.at("structuredContent").at("findings").empty());
    clu.finish(3U);

    primary.send(statusRequest(3));
    fallback.send(statusRequest(3));
    validateStatus(primary.awaitFrames(3U), 3, 2U);
    validateStatus(fallback.awaitFrames(3U), 3, 2U);

    primary.finish(3U);

    fallback.send(statusRequest(4));
    validateStatus(fallback.awaitFrames(4U), 4, 1U);
    fallback.finish(4U);

    McpProcessSession verifier{
        executable,
        home,
        workspace,
        L"primary",
        L"p14-shared-root-verifier"};
    verifier.send(handshake);
    const auto verifierObservation = observeRole(verifier);
    REQUIRE(verifierObservation.serverName == primaryObservation.serverName);
    REQUIRE(verifierObservation.tools == golden.at("tools"));
    verifier.send(statusRequest(3));
    validateStatus(verifier.awaitFrames(3U), 3, 1U);

    validateRegistry(home, {workspace});

    // Keep this child alive while a second canonical project is registered and
    // initialized. The subsequent context recovery must therefore force the
    // live server to refresh its registry-backed authority rather than relying
    // on a process-start snapshot.
    const auto workspaceB = sharedRoot / L"workspace-b-\u52d5\u7684";
    std::filesystem::create_directories(workspaceB);

    const auto projectId = registerWorkspaceProject(home, workspaceB);
    REQUIRE(canonicalUuid(projectId));
    verifier.send(toolRequest(
        4,
        "project_memory.initialize",
        Json{{"project_id", projectId},
             {"project_path", utf8Path(workspaceB)},
             {"idempotency_key", "p14-dynamic-project-initialize"}}));
    const auto initialized = successfulToolPayload(
        verifier.awaitFrames(4U), 4);
    REQUIRE(initialized.at("ok") == true);
    REQUIRE(initialized.at("project_id") == projectId);
    REQUIRE(initialized.at("project").at("aliases").is_array());
    REQUIRE(initialized.at("project").at("aliases").size() == 1U);

    verifier.send(toolRequest(
        5,
        "session_checkpoint",
        Json{{"project_id", projectId},
             {"goal", "Recover the live dynamic workspace"},
             {"status", "in_progress"},
             {"cwd", utf8Path(workspaceB)},
             {"narrative", "Project B was registered after launch."}}));
    const auto checkpoint = successfulToolPayload(
        verifier.awaitFrames(5U), 5);
    REQUIRE(checkpoint.at("ok") == true);
    REQUIRE(checkpoint.at("action") == "checkpoint");
    REQUIRE(checkpoint.at("handoff_id").is_string());

    // An explicit project on the checkpoint scopes that call only. Default
    // recovery must retain A instead of adopting the globally latest B packet.
    verifier.send(toolRequest(6, "context_get", Json::object()));
    const auto defaultRecovery = successfulToolPayload(
        verifier.awaitFrames(6U), 6);
    REQUIRE(defaultRecovery.at("ok") == true);
    REQUIRE(defaultRecovery.at("found") == false);
    verifier.send(statusRequest(7));
    const auto unchangedWorkspace = successfulToolPayload(
        verifier.awaitFrames(7U), 7);
    REQUIRE(unchangedWorkspace.at("workspace").at("project_id") != projectId);
    REQUIRE(normalizedPathKey(unchangedWorkspace.at("workspace")
                .at("project_root").get<std::string>()) ==
            normalizedPathKey(utf8Path(std::filesystem::canonical(workspace))));

    // Explicit recovery still refreshes live registry-backed authority and
    // adopts the registered project that was added after server launch.
    verifier.send(toolRequest(8, "context_get",
        Json{{"handoff_id", checkpoint.at("handoff_id")}}));
    const auto recovered = successfulToolPayload(
        verifier.awaitFrames(8U), 8);
    REQUIRE(recovered.at("ok") == true);
    REQUIRE(recovered.at("found") == true);
    REQUIRE(recovered.at("workspace_project_id") == projectId);
    REQUIRE(normalizedPathKey(
                recovered.at("workspace_adopted").get<std::string>()) ==
            normalizedPathKey(utf8Path(std::filesystem::canonical(workspaceB))));

    constexpr std::string_view dynamicFileName{"dynamic-project-proof.txt"};
    constexpr std::string_view dynamicContent{"project B resolved in-process"};
    verifier.send(toolRequest(
        9,
        "fs_write",
        Json{{"path", std::string{dynamicFileName}},
             {"content", std::string{dynamicContent}}}));
    const auto written = successfulToolPayload(verifier.awaitFrames(9U), 9);
    REQUIRE(written.at("ok") == true);
    REQUIRE(written.at("bytes_written") == dynamicContent.size());
    const auto dynamicFile = workspaceB / std::string{dynamicFileName};
    REQUIRE(normalizedPathKey(written.at("path").get<std::string>()) ==
            normalizedPathKey(utf8Path(dynamicFile)));
    REQUIRE(std::filesystem::is_regular_file(dynamicFile));
    REQUIRE(!std::filesystem::exists(workspace / std::string{dynamicFileName}));
    std::ifstream dynamicInput{dynamicFile, std::ios::binary};
    REQUIRE(dynamicInput.is_open());
    std::string dynamicBytes{
        std::istreambuf_iterator<char>{dynamicInput}, std::istreambuf_iterator<char>{}};
    REQUIRE(dynamicBytes == dynamicContent);
    verifier.finish(9U);

    // The deployment preflight intentionally supplies a present-but-empty
    // deployment ID so the child cannot inherit an ambient installed revision.
    // The serve root must treat that value as empty and generate its isolated
    // process identity instead of rejecting the successful zero-character read.
    const auto localAppData = environmentValue(L"LOCALAPPDATA");
    REQUIRE(localAppData.has_value());
    REQUIRE(!localAppData->empty());
    const auto isolatedHome = std::filesystem::path{*localAppData} /
        (L"ForgeConductor-LMStudioPreflight-" +
         std::to_wstring(::GetCurrentProcessId()) + L'-' +
         std::to_wstring(::GetTickCount64()));
    std::error_code isolatedDirectoryError;
    REQUIRE(std::filesystem::create_directories(
        isolatedHome, isolatedDirectoryError));
    REQUIRE(!isolatedDirectoryError);
    struct IsolatedHomeCleanup final {
        std::filesystem::path path;
        ~IsolatedHomeCleanup() noexcept
        {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove_all(path, ignored));
        }
    } isolatedCleanup{isolatedHome};
    McpProcessSession isolatedPreflight{
        executable,
        isolatedHome,
        isolatedHome,
        L"primary",
        L"",
        true};
    isolatedPreflight.send(handshake);
    const auto isolatedObservation = observeRole(isolatedPreflight);
    REQUIRE(isolatedObservation.serverName == primaryObservation.serverName);
    REQUIRE(isolatedObservation.tools == golden.at("tools"));
    isolatedPreflight.finish(2U);
    REQUIRE(std::filesystem::is_regular_file(
        isolatedHome / L"projects" / L"registry.json"));
    REQUIRE(std::filesystem::is_regular_file(isolatedHome / L"store.sqlite"));

    validateRegistry(home, {workspace, workspaceB});
    runPopulatedWorkspaceRegression(executable, sharedRoot / L"populated-state");
    runAgentLifecycleRegression(executable, sharedRoot / L"agent-lifecycle");
    runPolicyPagingRegression(executable, sharedRoot / L"policy-paging");
    runFragmentedToolResultRegression(executable, sharedRoot / L"fragmented-result");
    runIsolatedManagerReviewerRegression(executable, sharedRoot / L"isolated-manager-reviewer");
}

} // namespace

int wmain(const int argumentCount, wchar_t* const arguments[])
{
    try {
        if (argumentCount != 3) {
            throw std::runtime_error{
                "Expected the CLI executable and MCP semantic golden paths."};
        }
        run(
            std::filesystem::path{arguments[1]},
            std::filesystem::path{arguments[2]});
        std::cout << "MCP serve process snapshot tests passed: "
                  << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP serve process snapshot tests failed after "
                  << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
