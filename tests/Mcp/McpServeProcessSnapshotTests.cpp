#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
    const DWORD written = ::GetEnvironmentVariableW(
        std::wstring{name}.c_str(), value.data(), required);
    if (written == 0U || written >= required) {
        throw win32Failure("GetEnvironmentVariableW");
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
    const std::wstring_view deploymentId)
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
    std::wstring commandLine =
        quoteWindowsArgument(executableText) + L" serve --home " +
        quoteWindowsArgument(home.native());
    std::vector<wchar_t> mutableCommand{
        commandLine.begin(), commandLine.end()};
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    {
        const ScopedEnvironmentVariable roleEnvironment{
            L"FORGE_MCP_ROLE", role};
        const ScopedEnvironmentVariable deploymentEnvironment{
            L"FORGE_DEPLOYMENT_ID", deploymentId};
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
    std::string bytes;
    bool overflow{};
    std::optional<DWORD> failure;
};

void drainPipe(const HANDLE handle, PipeCapture& capture) noexcept
{
    std::array<char, 4U * 1024U> buffer{};
    for (;;) {
        DWORD read{};
        if (::ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr) == FALSE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) {
                return;
            }
            capture.failure = error;
            return;
        }
        if (read == 0U) {
            return;
        }
        if (capture.bytes.size() + read <= MaximumCapturedBytes) {
            capture.bytes.append(buffer.data(), read);
        } else {
            capture.overflow = true;
        }
    }
}

void terminateIfRunning(const HANDLE process) noexcept
{
    if (::WaitForSingleObject(process, 0U) == WAIT_TIMEOUT) {
        static_cast<void>(::TerminateProcess(process, 124U));
        static_cast<void>(::WaitForSingleObject(process, 5'000U));
    }
}

[[nodiscard]] std::string requestStream()
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
    const Json forgeStatus{
        {"id", 3},
        {"jsonrpc", "2.0"},
        {"method", "tools/call"},
        {"params",
         Json{{"arguments", Json::object()}, {"name", "forge_status"}}}};
    return initialize.dump() + '\n' + initialized.dump() + '\n' +
        toolsList.dump() + '\n' + forgeStatus.dump() + '\n';
}

[[nodiscard]] std::string runRole(
    const std::filesystem::path& executable,
    const std::filesystem::path& home,
    const std::filesystem::path& workspace,
    const std::wstring_view role,
    const std::wstring_view deploymentId)
{
    auto child = launch(
        executable, home, workspace, role, deploymentId);
    PipeCapture output;
    PipeCapture error;
    std::jthread outputDrain{
        [&] { drainPipe(child.outputReader.get(), output); }};
    std::jthread errorDrain{
        [&] { drainPipe(child.errorReader.get(), error); }};

    DWORD exitCode{};
    try {
        const auto input = requestStream();
        writeAll(child.inputWriter.get(), input);
        child.inputWriter.reset();

        const DWORD wait = ::WaitForSingleObject(
            child.process.get(),
            static_cast<DWORD>(ChildTimeout.count() * 1'000));
        if (wait == WAIT_TIMEOUT) {
            terminateIfRunning(child.process.get());
            throw std::runtime_error{
                "The stdio MCP child did not exit after end-of-file."};
        }
        if (wait != WAIT_OBJECT_0) {
            throw win32Failure("WaitForSingleObject");
        }
        if (::GetExitCodeProcess(child.process.get(), &exitCode) == FALSE) {
            throw win32Failure("GetExitCodeProcess");
        }
    } catch (...) {
        child.inputWriter.reset();
        terminateIfRunning(child.process.get());
        outputDrain.join();
        errorDrain.join();
        throw;
    }

    outputDrain.join();
    errorDrain.join();
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
    return output.bytes;
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
    const std::filesystem::path& executable,
    const std::filesystem::path& home,
    const std::filesystem::path& workspace,
    const std::wstring_view role,
    const std::wstring_view deploymentId)
{
    const auto frames = parseProtocolFrames(runRole(
        executable, home, workspace, role, deploymentId));
    REQUIRE(frames.size() == 3U);

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

    const auto& status = responseFor(frames, 3);
    REQUIRE(status.size() == 3U);
    REQUIRE(status.at("jsonrpc") == "2.0");
    REQUIRE(status.contains("result"));
    REQUIRE(!status.contains("error"));
    const auto& statusResult = status.at("result");
    REQUIRE(statusResult.at("isError") == false);
    const auto& structuredStatus = statusResult.at("structuredContent");
    REQUIRE(structuredStatus.is_object());
    REQUIRE(structuredStatus.at("presence_count") == 1U);
    const auto& content = statusResult.at("content");
    REQUIRE(content.is_array());
    REQUIRE(content.size() == 1U);
    REQUIRE(content.front().at("type") == "text");
    const auto textStatus = Json::parse(
        content.front().at("text").get_ref<const std::string&>());
    REQUIRE(textStatus.at("presence_count") == 1U);
    return RoleObservation{
        initializeResult.at("serverInfo").at("name").get<std::string>(),
        tools};
}

void run(
    const std::filesystem::path& executable,
    const std::filesystem::path& goldenPath)
{
    REQUIRE(std::filesystem::is_regular_file(executable));
    const auto golden = loadGolden(goldenPath);

    TemporaryDirectory temporary;
    const auto primaryRoot = temporary.root() / "primary";
    const auto fallbackRoot = temporary.root() / "fallback";
    const auto primaryHome = primaryRoot / "home";
    const auto primaryWorkspace = primaryRoot / "workspace";
    const auto fallbackHome = fallbackRoot / "home";
    const auto fallbackWorkspace = fallbackRoot / "workspace";
    std::filesystem::create_directories(primaryHome);
    std::filesystem::create_directories(primaryWorkspace);
    std::filesystem::create_directories(fallbackHome);
    std::filesystem::create_directories(fallbackWorkspace);

    const auto primary = observeRole(
        executable,
        primaryHome,
        primaryWorkspace,
        L"primary",
        L"p14-process-snapshot-primary");
    const auto fallback = observeRole(
        executable,
        fallbackHome,
        fallbackWorkspace,
        L"fallback",
        L"p14-process-snapshot-fallback");

    REQUIRE(primary.serverName == "forge-conductor");
    REQUIRE(fallback.serverName == "forge-conductor-fallback");
    REQUIRE(primary.serverName != fallback.serverName);
    REQUIRE(primary.tools == fallback.tools);
    REQUIRE(primary.tools == golden.at("tools"));
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
