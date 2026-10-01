#include "ForgeConductor/Infrastructure/Windows/WindowsMachineToolResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::size_t MaximumRegistryStringBytes = 32U * 1024U;
constexpr std::size_t MaximumSearchPathBytes = 4'000U;
constexpr DWORD MachineRegistryStringFlags =
    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
constexpr wchar_t MachineEnvironmentKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
constexpr wchar_t WindowsCurrentVersionKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
constexpr std::string_view DefaultPathExt =
    ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";

[[nodiscard]] std::optional<std::wstring> registryString(
    const wchar_t* const subkey,
    const wchar_t* const name)
{
    DWORD size{};
    const LSTATUS measured = ::RegGetValueW(
        HKEY_LOCAL_MACHINE, subkey, name,
        MachineRegistryStringFlags,
        nullptr, nullptr, &size);
    if (measured != ERROR_SUCCESS || size < sizeof(wchar_t) ||
        size > MaximumRegistryStringBytes) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD bytes = size;
    const LSTATUS read = ::RegGetValueW(
        HKEY_LOCAL_MACHINE, subkey, name,
        MachineRegistryStringFlags,
        nullptr, value.data(), &bytes);
    if (read != ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    if (value.empty() || value.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> wideToUtf8(
    const std::wstring_view value)
{
    if (value.empty() ||
        value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int inputLength = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            converted.data(), required, nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return converted;
}

[[nodiscard]] std::wstring windowsDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT written = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    return written != 0U && written < MAX_PATH
        ? std::wstring{buffer, written}
        : std::wstring{L"C:\\Windows"};
}

[[nodiscard]] std::wstring systemDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const UINT written = ::GetSystemDirectoryW(buffer, MAX_PATH);
    return written != 0U && written < MAX_PATH
        ? std::wstring{buffer, written}
        : std::wstring{L"C:\\Windows\\System32"};
}

[[nodiscard]] std::wstring programFilesDirectory()
{
    if (const auto configured = registryString(
            WindowsCurrentVersionKey, L"ProgramFilesDir")) {
        try {
            const std::filesystem::path candidate{*configured};
            if (candidate.is_absolute() &&
                configured->find(L'%') == std::wstring::npos) {
                return candidate.lexically_normal().wstring();
            }
        } catch (...) {
        }
    }
    return L"C:\\Program Files";
}

[[nodiscard]] bool regularMachineDirectory(const std::wstring_view path)
{
    if (path.empty() ||
        path.size() > static_cast<std::size_t>(MAX_PATH) * 4U) {
        return false;
    }
    const std::filesystem::path candidate{path};
    if (!candidate.is_absolute()) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(candidate.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

void appendUniqueDirectory(
    std::vector<std::wstring>& directories,
    std::wstring candidate)
{
    try {
        candidate = std::filesystem::path{candidate}.lexically_normal().wstring();
    } catch (...) {
        return;
    }
    if (!regularMachineDirectory(candidate)) {
        return;
    }
    const auto duplicate = std::any_of(
        directories.begin(), directories.end(),
        [&](const std::wstring& existing) noexcept {
            return existing.size() <= static_cast<std::size_t>(INT_MAX) &&
                candidate.size() <= static_cast<std::size_t>(INT_MAX) &&
                ::CompareStringOrdinal(
                    existing.data(), static_cast<int>(existing.size()),
                    candidate.data(), static_cast<int>(candidate.size()),
                    TRUE) == CSTR_EQUAL;
        });
    if (!duplicate) {
        directories.push_back(std::move(candidate));
    }
}

void appendPathList(
    std::vector<std::wstring>& directories,
    const std::wstring_view pathList)
{
    std::size_t start{};
    while (start <= pathList.size()) {
        const auto end = pathList.find(L';', start);
        auto entry = std::wstring{pathList.substr(
            start, end == std::wstring_view::npos
                ? std::wstring_view::npos : end - start)};
        const auto first = entry.find_first_not_of(L" \t\"");
        if (first == std::wstring::npos) {
            entry.clear();
        } else {
            const auto last = entry.find_last_not_of(L" \t\"");
            entry = entry.substr(first, last - first + 1U);
        }
        appendUniqueDirectory(directories, std::move(entry));
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1U;
    }
}

[[nodiscard]] std::vector<std::wstring> machineDirectories()
{
    std::vector<std::wstring> directories;
    const auto windows = windowsDirectory();
    const auto system = systemDirectory();
    const auto programFiles = programFilesDirectory();
    appendUniqueDirectory(directories, system);
    appendUniqueDirectory(directories, windows);
    appendUniqueDirectory(directories, system + L"\\Wbem");
    appendUniqueDirectory(
        directories, system + L"\\WindowsPowerShell\\v1.0");
    appendUniqueDirectory(directories, system + L"\\OpenSSH");
    appendUniqueDirectory(directories, programFiles + L"\\PowerShell\\7");
    appendUniqueDirectory(directories, programFiles + L"\\Git\\cmd");
    appendUniqueDirectory(directories, programFiles + L"\\Git\\bin");
    appendUniqueDirectory(
        directories, programFiles + L"\\Python312\\Scripts");
    appendUniqueDirectory(directories, programFiles + L"\\Python312");
    appendUniqueDirectory(directories, programFiles + L"\\nodejs");
    appendUniqueDirectory(directories, programFiles + L"\\CMake\\bin");
    appendUniqueDirectory(directories, programFiles + L"\\GitHub CLI");
    if (const auto machine = registryString(MachineEnvironmentKey, L"Path")) {
        appendPathList(directories, *machine);
    }
    return directories;
}

[[nodiscard]] bool isRegularExecutable(
    const std::filesystem::path& candidate,
    const bool allowMultipleLinks = false) noexcept
{
    const HANDLE file = ::CreateFileW(
        candidate.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    const bool valid = ::GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE &&
        ::GetFileInformationByHandleEx(
            file, FileStandardInfo, &standard, sizeof(standard)) != FALSE &&
        (attributes.FileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U &&
        standard.DeletePending == FALSE &&
        (allowMultipleLinks || standard.NumberOfLinks == 1U);
    ::CloseHandle(file);
    if (!valid) {
        return false;
    }
    DWORD binaryType{};
    return ::GetBinaryTypeW(candidate.c_str(), &binaryType) != FALSE &&
        (binaryType == SCS_32BIT_BINARY || binaryType == SCS_64BIT_BINARY);
}

[[nodiscard]] std::optional<Domain::PathText> pathText(
    const std::filesystem::path& candidate)
{
    const auto encoded = wideToUtf8(candidate.wstring());
    if (!encoded) {
        return std::nullopt;
    }
    auto parsed = Domain::PathText::create(*encoded);
    return parsed
        ? std::optional<Domain::PathText>{std::move(parsed).value()}
        : std::nullopt;
}

[[nodiscard]] std::optional<Domain::PathText> regularExecutable(
    const std::filesystem::path& candidate,
    const bool allowMultipleLinks = false)
{
    return isRegularExecutable(candidate, allowMultipleLinks)
        ? pathText(candidate)
        : std::nullopt;
}

[[nodiscard]] Domain::Error missingTool(const std::string_view name)
{
    return Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "The required machine-installed " + std::string{name} +
            " executable was not found outside per-user PATH configuration.");
}

} // namespace

Domain::Result<Domain::PathText>
WindowsMachineToolResolver::gitExecutable() noexcept
{
    try {
        const auto programFiles = programFilesDirectory();
        if (auto git = regularExecutable(
                std::filesystem::path{programFiles} / L"Git" / L"bin" /
                    L"git.exe")) {
            return Domain::Result<Domain::PathText>::success(std::move(*git));
        }
        for (const auto& directory : machineDirectories()) {
            const auto candidate =
                std::filesystem::path{directory} / L"git.exe";
            if (auto git = regularExecutable(candidate)) {
                return Domain::Result<Domain::PathText>::success(
                    std::move(*git));
            }
            if (_wcsicmp(candidate.parent_path().filename().c_str(), L"cmd") == 0) {
                if (auto git = regularExecutable(
                        candidate.parent_path().parent_path() / L"bin" /
                            L"git.exe")) {
                    return Domain::Result<Domain::PathText>::success(
                        std::move(*git));
                }
            }
        }
        return Domain::Result<Domain::PathText>::failure(missingTool("Git"));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The machine-installed Git executable could not be resolved safely."));
    }
}

Domain::Result<Domain::PathText>
WindowsMachineToolResolver::powerShellExecutable() noexcept
{
    try {
        const auto programFiles = programFilesDirectory();
        if (auto powerShell = regularExecutable(
                std::filesystem::path{programFiles} / L"PowerShell" / L"7" /
                    L"pwsh.exe")) {
            return Domain::Result<Domain::PathText>::success(
                std::move(*powerShell));
        }
        for (const auto& directory : machineDirectories()) {
            if (auto powerShell = regularExecutable(
                    std::filesystem::path{directory} / L"pwsh.exe")) {
                return Domain::Result<Domain::PathText>::success(
                    std::move(*powerShell));
            }
        }
        if (auto powerShell = regularExecutable(
                std::filesystem::path{systemDirectory()} /
                    L"WindowsPowerShell" / L"v1.0" / L"powershell.exe",
                true)) {
            return Domain::Result<Domain::PathText>::success(
                std::move(*powerShell));
        }
        return Domain::Result<Domain::PathText>::failure(
            missingTool("PowerShell"));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The machine-installed PowerShell executable could not be resolved safely."));
    }
}

std::string WindowsMachineToolResolver::searchPath() noexcept
{
    try {
        std::string joined;
        for (const auto& directory : machineDirectories()) {
            const auto encoded = wideToUtf8(directory);
            if (!encoded) {
                continue;
            }
            const std::size_t extra =
                encoded->size() + (joined.empty() ? 0U : 1U);
            if (joined.size() + extra > MaximumSearchPathBytes) {
                break;
            }
            if (!joined.empty()) {
                joined.push_back(';');
            }
            joined.append(*encoded);
        }
        return joined;
    } catch (...) {
        return {};
    }
}

std::string WindowsMachineToolResolver::pathExt() noexcept
{
    try {
        auto configured = registryString(MachineEnvironmentKey, L"PATHEXT");
        if (!configured || configured->size() > MaximumSearchPathBytes) {
            return std::string{DefaultPathExt};
        }
        auto upper = *configured;
        std::ranges::transform(
            upper, upper.begin(), [](const wchar_t value) {
                return static_cast<wchar_t>(std::towupper(value));
            });
        if (upper.find(L".EXE") == std::wstring::npos) {
            return std::string{DefaultPathExt};
        }
        const auto encoded = wideToUtf8(*configured);
        return encoded ? std::move(*encoded) : std::string{DefaultPathExt};
    } catch (...) {
        return std::string{DefaultPathExt};
    }
}

std::string WindowsMachineToolResolver::commandInterpreter() noexcept
{
    try {
        const auto command =
            std::filesystem::path{systemDirectory()} / L"cmd.exe";
        const auto encoded = wideToUtf8(command.wstring());
        return encoded ? std::move(*encoded)
                       : std::string{"C:\\Windows\\System32\\cmd.exe"};
    } catch (...) {
        return "C:\\Windows\\System32\\cmd.exe";
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
