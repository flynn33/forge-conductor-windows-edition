#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool writeAll(const HANDLE destination, const std::string_view bytes) noexcept
{
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        if (!::WriteFile(destination, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0U) {
            return false;
        }
        offset += written;
    }
    return true;
}

[[nodiscard]] std::string utf16ToUtf8(const std::wstring_view value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const auto length = static_cast<int>(value.size());
    const auto required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                                                nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(),
                              required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

[[nodiscard]] bool parseUnsigned(const wchar_t* value, unsigned long& result) noexcept
{
    if (value == nullptr || *value == L'\0') {
        return false;
    }
    wchar_t* end{};
    result = std::wcstoul(value, &end, 10);
    return end != value && *end == L'\0';
}

[[nodiscard]] int emitBytes(const std::size_t stdoutBytes, const std::size_t stderrBytes)
{
    constexpr std::size_t chunkSize = 4U * 1024U;
    const std::string stdoutChunk(chunkSize, 'O');
    const std::string stderrChunk(chunkSize, 'E');
    std::size_t stdoutOffset{};
    std::size_t stderrOffset{};
    while (stdoutOffset < stdoutBytes || stderrOffset < stderrBytes) {
        if (stdoutOffset < stdoutBytes) {
            const auto count = (std::min)(chunkSize, stdoutBytes - stdoutOffset);
            if (!writeAll(::GetStdHandle(STD_OUTPUT_HANDLE),
                          std::string_view{stdoutChunk}.substr(0U, count))) {
                return 10;
            }
            stdoutOffset += count;
        }
        if (stderrOffset < stderrBytes) {
            const auto count = (std::min)(chunkSize, stderrBytes - stderrOffset);
            if (!writeAll(::GetStdHandle(STD_ERROR_HANDLE),
                          std::string_view{stderrChunk}.substr(0U, count))) {
                return 11;
            }
            stderrOffset += count;
        }
    }
    return 0;
}

[[nodiscard]] std::wstring modulePath()
{
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;) {
        const auto written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0U) {
            return {};
        }
        if (written < buffer.size() - 1U) {
            return std::wstring{buffer.data(), static_cast<std::size_t>(written)};
        }
        buffer.resize(buffer.size() * 2U, L'\0');
    }
}

[[nodiscard]] int spawnDescendant()
{
    const auto executable = modulePath();
    if (executable.empty()) {
        return 20;
    }
    std::wstring commandLine = L"\"" + executable + L"\" --sleep 30000";
    std::vector<wchar_t> mutableCommand{commandLine.begin(), commandLine.end()};
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return 21;
    }
    static_cast<void>(::CloseHandle(process.hThread));
    static_cast<void>(::CloseHandle(process.hProcess));
    const auto pid = std::to_string(process.dwProcessId) + "\n";
    return writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), pid) ? 0 : 22;
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments)
{
    if (argumentCount < 2) {
        return 2;
    }
    const std::wstring_view mode{arguments[1]};
    if (mode == L"--exit" && argumentCount == 3) {
        unsigned long code{};
        return parseUnsigned(arguments[2], code) ? static_cast<int>(code) : 3;
    }
    if (mode == L"--sleep" && argumentCount == 3) {
        unsigned long milliseconds{};
        if (!parseUnsigned(arguments[2], milliseconds)) {
            return 3;
        }
        ::Sleep(milliseconds);
        return 0;
    }
    if (mode == L"--emit" && argumentCount == 4) {
        unsigned long stdoutBytes{};
        unsigned long stderrBytes{};
        if (!parseUnsigned(arguments[2], stdoutBytes) ||
            !parseUnsigned(arguments[3], stderrBytes)) {
            return 3;
        }
        return emitBytes(stdoutBytes, stderrBytes);
    }
    if (mode == L"--echo" && argumentCount >= 2) {
        for (int index = 2; index < argumentCount; ++index) {
            const auto value = utf16ToUtf8(arguments[index]);
            const auto line = std::to_string(value.size()) + ":" + value + "\n";
            if (!writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), line)) {
                return 4;
            }
        }
        return 0;
    }
    if (mode == L"--malformed") {
        constexpr char malformed[] = {'v', 'a', 'l', 'i', 'd', '-', static_cast<char>(0xff), 'x'};
        return writeAll(::GetStdHandle(STD_OUTPUT_HANDLE),
                        std::string_view{malformed, sizeof(malformed)})
                   ? 0
                   : 4;
    }
    if (mode == L"--utf8-euro") {
        constexpr char value[] = {'A', static_cast<char>(0xe2), static_cast<char>(0x82),
                                  static_cast<char>(0xac)};
        return writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), std::string_view{value, sizeof(value)})
                   ? 0
                   : 4;
    }
    if (mode == L"--environment" && argumentCount == 3) {
        const auto required = ::GetEnvironmentVariableW(arguments[2], nullptr, 0);
        if (required == 0U) {
            return 5;
        }
        std::vector<wchar_t> value(static_cast<std::size_t>(required), L'\0');
        const auto written = ::GetEnvironmentVariableW(arguments[2], value.data(), required);
        if (written == 0U || written >= required) {
            return 5;
        }
        return writeAll(::GetStdHandle(STD_OUTPUT_HANDLE),
                        utf16ToUtf8(std::wstring_view{value.data(), written}))
                   ? 0
                   : 4;
    }
    if (mode == L"--working-directory") {
        const auto required = ::GetCurrentDirectoryW(0, nullptr);
        if (required == 0U) {
            return 6;
        }
        std::vector<wchar_t> value(static_cast<std::size_t>(required), L'\0');
        const auto written = ::GetCurrentDirectoryW(required, value.data());
        if (written == 0U || written >= required) {
            return 6;
        }
        return writeAll(::GetStdHandle(STD_OUTPUT_HANDLE),
                        utf16ToUtf8(std::wstring_view{value.data(), written}))
                   ? 0
                   : 4;
    }
    if (mode == L"--spawn-descendant") {
        return spawnDescendant();
    }
    if (mode == L"--wait-events" && argumentCount == 4) {
        const HANDLE release = ::OpenEventW(SYNCHRONIZE, FALSE, arguments[2]);
        const HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[3]);
        if (release == nullptr || ready == nullptr) {
            if (release != nullptr) {
                static_cast<void>(::CloseHandle(release));
            }
            if (ready != nullptr) {
                static_cast<void>(::CloseHandle(ready));
            }
            return 7;
        }
        static_cast<void>(::SetEvent(ready));
        const auto wait = ::WaitForSingleObject(release, 60'000U);
        static_cast<void>(::CloseHandle(ready));
        static_cast<void>(::CloseHandle(release));
        return wait == WAIT_OBJECT_0 ? 0 : 8;
    }
    return 2;
}
