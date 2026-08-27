#ifndef NOMINMAX
#define NOMINMAX
#endif
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
constexpr std::size_t ExpectedToolCount = 53U;

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
    REQUIRE(result.at("content").size() == 1U);
    REQUIRE(result.at("content").front().at("type") == "text");
    const auto serialized = result.at("content").front().at("text").get<std::string>();
    REQUIRE(Json::parse(serialized) == result.at("structuredContent"));
    return result.at("structuredContent");
}

void validateToolArray(const Json& tools)
{
    REQUIRE(tools.is_array());
    REQUIRE(tools.size() == ExpectedToolCount);
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
};

[[nodiscard]] RoleObservation observeRole(
    McpProcessSession& session)
{
    const auto frames = session.awaitFrames(2U);

    const auto& initialize = responseFor(frames, 1);
    REQUIRE(initialize.size() == 3U);
    REQUIRE(initialize.at("jsonrpc") == "2.0");
    REQUIRE(initialize.contains("result"));
    REQUIRE(!initialize.contains("error"));
    const auto& initializeResult = initialize.at("result");
    REQUIRE(initializeResult.at("protocolVersion") == "2025-11-25");
    REQUIRE(initializeResult.at("serverInfo").at("version") == "0.9.0");
    REQUIRE(initializeResult.at("capabilities").at("tools").at("listChanged") == false);

    const auto& listed = responseFor(frames, 2);
    REQUIRE(listed.size() == 3U);
    REQUIRE(listed.at("jsonrpc") == "2.0");
    REQUIRE(listed.contains("result"));
    REQUIRE(!listed.contains("error"));
    const auto& tools = listed.at("result").at("tools");
    validateToolArray(tools);
    return RoleObservation{
        initializeResult.at("serverInfo").at("name").get<std::string>(),
        tools};
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

    verifier.send(toolRequest(6, "context_get", Json::object()));
    const auto recovered = successfulToolPayload(
        verifier.awaitFrames(6U), 6);
    REQUIRE(recovered.at("ok") == true);
    REQUIRE(recovered.at("found") == true);
    REQUIRE(recovered.at("workspace_project_id") == projectId);
    REQUIRE(normalizedPathKey(
                recovered.at("workspace_adopted").get<std::string>()) ==
            normalizedPathKey(utf8Path(std::filesystem::canonical(workspaceB))));

    constexpr std::string_view dynamicFileName{"dynamic-project-proof.txt"};
    constexpr std::string_view dynamicContent{"project B resolved in-process"};
    verifier.send(toolRequest(
        7,
        "fs_write",
        Json{{"path", std::string{dynamicFileName}},
             {"content", std::string{dynamicContent}}}));
    const auto written = successfulToolPayload(verifier.awaitFrames(7U), 7);
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
    verifier.finish(7U);

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
