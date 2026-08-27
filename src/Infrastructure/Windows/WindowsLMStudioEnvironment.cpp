#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h"

#include "Detail/BoundedSerialExecutor.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UtfConversion.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winver.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Version.lib")

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

constexpr std::size_t MaximumRegistrySubkeys = 512U;
constexpr std::size_t MaximumRegistryValueCharacters = 32U * 1024U;
constexpr std::size_t MaximumManifestBytes = 1024U * 1024U;
constexpr std::size_t MaximumVersionResourceBytes = 4U * 1024U * 1024U;
constexpr std::size_t MaximumHealthDetailBytes = 4U * 1024U;
constexpr std::size_t MaximumProtocolVersionBytes = 128U;
constexpr std::size_t MaximumReportedToolCount = 10'000U;
constexpr std::string_view SupportedMcpProtocolVersion = "2025-11-25";

class UniqueRegistryHandle final {
public:
    explicit UniqueRegistryHandle(const HKEY value = nullptr) noexcept : value_{value} {}

    ~UniqueRegistryHandle() noexcept
    {
        reset();
    }

    UniqueRegistryHandle(const UniqueRegistryHandle&) = delete;
    UniqueRegistryHandle& operator=(const UniqueRegistryHandle&) = delete;

    UniqueRegistryHandle(UniqueRegistryHandle&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)}
    {
    }

    UniqueRegistryHandle& operator=(UniqueRegistryHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HKEY get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HKEY* put() noexcept
    {
        reset();
        return &value_;
    }

private:
    void reset(const HKEY replacement = nullptr) noexcept
    {
        if (value_ != nullptr) {
            static_cast<void>(::RegCloseKey(value_));
        }
        value_ = replacement;
    }

    HKEY value_{};
};

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess candidate) noexcept
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

[[nodiscard]] bool isCandidateLocalReadError(const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::RecordNotFound ||
           error.code == Domain::ErrorCodes::Unauthorized ||
           error.code == Domain::ErrorCodes::PathOutsideAuthority ||
           error.code == Domain::ErrorCodes::InvalidRequest ||
           error.code == Domain::ErrorCodes::PayloadTooLarge;
}

[[nodiscard]] std::string appendDetail(std::string detail, const std::string_view addition)
{
    if (addition.empty()) {
        return detail;
    }
    if (!detail.empty()) {
        detail.append(" ");
    }
    detail.append(addition);
    return detail;
}

[[nodiscard]] Domain::Result<Domain::PathText> pathText(const std::wstring_view path)
{
    auto utf8 = Detail::strictUtf16ToUtf8(path);
    if (!utf8) {
        return Domain::Result<Domain::PathText>::failure(std::move(utf8).error());
    }
    return Domain::PathText::create(utf8.value());
}

[[nodiscard]] Domain::Result<std::wstring> widePath(const Domain::PathText& path) noexcept
{
    return Detail::strictUtf8ToUtf16(path.value());
}

[[nodiscard]] std::optional<Domain::PathText> parentPath(
    const Domain::PathText& child) noexcept
{
    try {
        auto converted = widePath(child);
        if (!converted) {
            return std::nullopt;
        }
        const auto parent = std::filesystem::path{converted.value()}.parent_path();
        if (parent.empty()) {
            return std::nullopt;
        }
        auto encoded = pathText(parent.native());
        return encoded ? std::optional<Domain::PathText>{std::move(encoded).value()} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::wstring> environmentPath(const wchar_t* const name) noexcept
{
    try {
        std::wstring value(32U * 1024U, L'\0');
        const DWORD length = ::GetEnvironmentVariableW(
            name, value.data(), static_cast<DWORD>(value.size()));
        if (length == 0U || length >= value.size()) {
            return std::nullopt;
        }
        value.resize(length);
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::wstring lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(::towlower(character));
    });
    return value;
}

[[nodiscard]] bool isLMStudioDisplayName(const std::wstring_view value)
{
    const auto normalized = lowercase(std::wstring{value});
    return normalized == L"lm studio" || normalized.starts_with(L"lm studio ");
}

[[nodiscard]] bool isRegularNonReparseFile(const std::wstring& path) noexcept
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

[[nodiscard]] std::optional<std::wstring> queryRegistryString(
    const HKEY key,
    const wchar_t* const name) noexcept
{
    try {
        DWORD type{};
        DWORD bytes{};
        const LSTATUS measured = ::RegQueryValueExW(
            key, name, nullptr, &type, nullptr, &bytes);
        if (measured != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) ||
            bytes > MaximumRegistryValueCharacters * sizeof(wchar_t)) {
            return std::nullopt;
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        DWORD actualBytes = bytes;
        if (::RegQueryValueExW(
                key,
                name,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(value.data()),
                &actualBytes) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        value.resize(actualBytes / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        if (value.empty()) {
            return std::nullopt;
        }
        if (type == REG_EXPAND_SZ) {
            const DWORD required = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0U);
            if (required == 0U || required > MaximumRegistryValueCharacters) {
                return std::nullopt;
            }
            std::wstring expanded(required, L'\0');
            if (::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) != required) {
                return std::nullopt;
            }
            while (!expanded.empty() && expanded.back() == L'\0') {
                expanded.pop_back();
            }
            value = std::move(expanded);
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::wstring> executableFromDisplayIcon(
    std::wstring value)
{
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.front() == L'"') {
        const auto closing = value.find(L'"', 1U);
        if (closing == std::wstring::npos) {
            return std::nullopt;
        }
        return value.substr(1U, closing - 1U);
    }
    const auto suffix = value.rfind(L',');
    if (suffix != std::wstring::npos) {
        value.resize(suffix);
    }
    while (!value.empty() && std::iswspace(value.back()) != 0) {
        value.pop_back();
    }
    return value.empty() ? std::nullopt : std::optional<std::wstring>{std::move(value)};
}

[[nodiscard]] std::optional<std::wstring> executableFromProtocolCommand(
    const std::wstring_view command)
{
    if (command.empty()) {
        return std::nullopt;
    }
    if (command.front() == L'"') {
        const auto closing = command.find(L'"', 1U);
        if (closing == std::wstring_view::npos) {
            return std::nullopt;
        }
        return std::wstring{command.substr(1U, closing - 1U)};
    }
    const auto executableEnd = lowercase(std::wstring{command}).find(L".exe");
    if (executableEnd == std::wstring::npos) {
        return std::nullopt;
    }
    return std::wstring{command.substr(0U, executableEnd + 4U)};
}

[[nodiscard]] std::optional<std::string> manifestVersion(
    const std::wstring& executable) noexcept
{
    try {
        const auto manifest = std::filesystem::path{executable}.parent_path() /
                              L"resources" / L"app" / L"package.json";
        Detail::UniqueHandle file{::CreateFileW(
            manifest.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)};
        if (!file) {
            return std::nullopt;
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart <= 0 ||
            size.QuadPart > static_cast<LONGLONG>(MaximumManifestBytes)) {
            return std::nullopt;
        }
        std::string encoded(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read{};
        const BOOL readResult = ::ReadFile(
            file.get(), encoded.data(), static_cast<DWORD>(encoded.size()), &read, nullptr);
        if (readResult == FALSE || static_cast<std::size_t>(read) != encoded.size()) {
            return std::nullopt;
        }
        const auto document = Json::parse(encoded, nullptr, true, false);
        const auto version = document.find("version");
        if (!document.is_object() || version == document.end() || !version->is_string()) {
            return std::nullopt;
        }
        auto value = version->get<std::string>();
        if (value.empty() || value.size() > 128U) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> fileProductVersion(
    const std::wstring& executable) noexcept
{
    try {
        DWORD ignored{};
        const DWORD size = ::GetFileVersionInfoSizeW(executable.c_str(), &ignored);
        if (size == 0U || size > MaximumVersionResourceBytes) {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(size);
        if (::GetFileVersionInfoW(executable.c_str(), 0U, size, bytes.data()) == FALSE) {
            return std::nullopt;
        }
        struct Translation final {
            WORD language;
            WORD codePage;
        };
        Translation* translations{};
        UINT translationBytes{};
        if (::VerQueryValueW(
                bytes.data(),
                L"\\VarFileInfo\\Translation",
                reinterpret_cast<void**>(&translations),
                &translationBytes) != FALSE &&
            translationBytes >= sizeof(Translation)) {
            std::array<wchar_t, 64U> query{};
            const int written = ::swprintf_s(
                query.data(),
                query.size(),
                L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                translations[0].language,
                translations[0].codePage);
            wchar_t* value{};
            UINT characters{};
            if (written > 0 &&
                ::VerQueryValueW(
                    bytes.data(), query.data(), reinterpret_cast<void**>(&value), &characters) !=
                    FALSE &&
                value != nullptr && characters > 1U) {
                auto converted = Detail::strictUtf16ToUtf8(
                    std::wstring_view{value, characters - 1U});
                if (converted && !converted.value().empty() && converted.value().size() <= 128U) {
                    return converted.value();
                }
            }
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::string> discoveredVersion(
    const std::wstring& executable,
    std::optional<std::string> registeredVersion = std::nullopt) noexcept
{
    if (registeredVersion && !registeredVersion->empty() && registeredVersion->size() <= 128U) {
        return registeredVersion;
    }
    if (auto version = manifestVersion(executable)) {
        return version;
    }
    return fileProductVersion(executable);
}

[[nodiscard]] Domain::Result<WindowsLMStudioDiscoveryCandidate> executableCandidate(
    const Domain::LMStudioDiscoverySource source,
    const std::wstring& executable,
    std::optional<std::string> registeredVersion,
    std::string detail)
{
    auto executableText = pathText(executable);
    if (!executableText) {
        return Domain::Result<WindowsLMStudioDiscoveryCandidate>::failure(
            std::move(executableText).error());
    }
    auto rootText = pathText(std::filesystem::path{executable}.parent_path().native());
    if (!rootText) {
        return Domain::Result<WindowsLMStudioDiscoveryCandidate>::failure(
            std::move(rootText).error());
    }
    const bool valid = isRegularNonReparseFile(executable);
    detail = appendDetail(
        std::move(detail),
        valid ? "The executable is a regular file." :
                "The executable is missing, inaccessible, or a reparse point.");
    return Domain::Result<WindowsLMStudioDiscoveryCandidate>::success(
        WindowsLMStudioDiscoveryCandidate{
            source,
            executableText.value(),
            executableText.value(),
            std::nullopt,
            rootText.value(),
            valid ? discoveredVersion(executable, std::move(registeredVersion)) : std::nullopt,
            valid,
            std::move(detail)});
}

[[nodiscard]] Domain::Result<WindowsLMStudioDiscoveryCandidate> configurationCandidate(
    const Domain::LMStudioDiscoverySource source,
    const std::wstring& configuration,
    std::string detail)
{
    auto path = pathText(configuration);
    if (!path) {
        return Domain::Result<WindowsLMStudioDiscoveryCandidate>::failure(std::move(path).error());
    }
    const bool exists = isRegularNonReparseFile(configuration);
    detail = appendDetail(
        std::move(detail),
        exists ? "The configuration candidate exists as a regular file." :
                 "The configuration candidate is missing, inaccessible, or a reparse point.");
    return Domain::Result<WindowsLMStudioDiscoveryCandidate>::success(
        WindowsLMStudioDiscoveryCandidate{
            source,
            path.value(),
            std::nullopt,
            path.value(),
            std::nullopt,
            std::nullopt,
            exists,
            std::move(detail)});
}

[[nodiscard]] WindowsLMStudioDiscoveryCandidate unavailableCandidate(
    const Domain::LMStudioDiscoverySource source,
    Domain::PathText evidencePath,
    std::string detail)
{
    return WindowsLMStudioDiscoveryCandidate{
        source,
        std::move(evidencePath),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        false,
        std::move(detail)};
}

[[nodiscard]] int sourcePriority(const Domain::LMStudioDiscoverySource source) noexcept
{
    switch (source) {
    case Domain::LMStudioDiscoverySource::ExplicitConfiguration:
        return 0;
    case Domain::LMStudioDiscoverySource::InstalledApplication:
        return 1;
    case Domain::LMStudioDiscoverySource::KnownUserLocation:
        return 2;
    case Domain::LMStudioDiscoverySource::RunningProcess:
        return 3;
    }
    return 4;
}

struct ConfigurationValidation final {
    bool valid{};
    std::string detail;
};

[[nodiscard]] Domain::Result<ConfigurationValidation> parseConfiguration(
    const std::vector<std::byte>& bytes,
    const std::size_t maximumDepth) noexcept
{
    try {
        if (bytes.empty()) {
            return Domain::Result<ConfigurationValidation>::success(
                ConfigurationValidation{false, "The configuration file is empty."});
        }
        const std::string text{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        std::vector<std::unordered_set<std::string>> objectKeys;
        bool rejectedDepth{};
        bool rejectedDuplicate{};
        const auto callback = [&](const int depth, const Json::parse_event_t event, Json& value) {
            if (depth < 0 || static_cast<std::size_t>(depth) > maximumDepth) {
                rejectedDepth = true;
                return false;
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() ||
                    !objectKeys.back().insert(value.get<std::string>()).second) {
                    rejectedDuplicate = true;
                    return false;
                }
            } else if (event == Json::parse_event_t::object_end && !objectKeys.empty()) {
                objectKeys.pop_back();
            }
            return true;
        };
        const auto document = Json::parse(text, callback, false, false);
        if (rejectedDepth) {
            return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
                false, "The configuration JSON exceeds the bounded nesting depth."});
        }
        if (rejectedDuplicate) {
            return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
                false, "The configuration JSON contains a duplicate object key."});
        }
        if (document.is_discarded()) {
            return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
                false, "The configuration file is not valid strict JSON."});
        }
        if (!document.is_object()) {
            return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
                false, "The configuration JSON root is not an object."});
        }
        const auto servers = document.find("mcpServers");
        if (servers != document.end()) {
            if (!servers->is_object()) {
                return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
                    false, "The configuration mcpServers member is not an object."});
            }
            for (const auto& [name, server] : servers->items()) {
                static_cast<void>(name);
                if (!server.is_object()) {
                    return Domain::Result<ConfigurationValidation>::success(
                        ConfigurationValidation{
                            false, "An MCP server registration is not a JSON object."});
                }
            }
        }
        return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
            true, "The configuration file parsed as bounded strict JSON."});
    } catch (...) {
        return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
            false, "The configuration file could not be parsed as strict JSON."});
    }
}

[[nodiscard]] Domain::Result<ConfigurationValidation> validateConfiguration(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    const Domain::PathText& candidate,
    const Contracts::WorkspaceAuthority& readAuthority,
    const WindowsLMStudioEnvironmentOptions& options,
    const Domain::OperationContext& context)
{
    auto authorized = workspaceAuthority.authorize(
        readAuthority,
        Domain::PathAuthorizationRequest{
            candidate, std::nullopt, Domain::FileAccess::Read, false},
        context);
    if (!authorized) {
        if (!isCandidateLocalReadError(authorized.error())) {
            return Domain::Result<ConfigurationValidation>::failure(
                std::move(authorized).error());
        }
        return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
            false,
            "Read authority rejected the configuration candidate (" +
                authorized.error().code + ")."});
    }
    auto content = fileSystem.readFile(
        authorized.value(), options.maximumConfigurationBytes, context);
    if (!content) {
        if (!isCandidateLocalReadError(content.error())) {
            return Domain::Result<ConfigurationValidation>::failure(
                std::move(content).error());
        }
        return Domain::Result<ConfigurationValidation>::success(ConfigurationValidation{
            false,
            "The configuration candidate could not be read (" + content.error().code + ")."});
    }
    return parseConfiguration(content.value(), options.maximumJsonDepth);
}

[[nodiscard]] bool validConnectionHealth(
    const Domain::LMStudioConnectionHealth& health) noexcept
{
    if (health.roles.size() > 2U ||
        health.state != Domain::deriveLMStudioConnectionState(health.roles)) {
        return false;
    }
    bool primary{};
    bool fallback{};
    for (const auto& role : health.roles) {
        if (role.detail.empty() || role.detail.size() > MaximumHealthDetailBytes ||
            role.toolCount > MaximumReportedToolCount ||
            (role.protocolVersion &&
             role.protocolVersion->size() > MaximumProtocolVersionBytes)) {
            return false;
        }
        if (role.ready) {
            if (!role.protocolVersion ||
                *role.protocolVersion != SupportedMcpProtocolVersion ||
                role.toolCount != 53U) {
                return false;
            }
        } else if (role.protocolVersion || role.toolCount != 0U) {
            return false;
        }
        bool& seen = role.role == Domain::LMStudioConnectorRole::Primary ? primary : fallback;
        if (seen) {
            return false;
        }
        seen = true;
    }
    return true;
}

} // namespace

class WindowsLMStudioDiscoverySource::Impl final {
public:
    explicit Impl(WindowsLMStudioDiscoverySourceOptions options)
        : options_{std::move(options)}
    {
        if (options_.maximumCandidates == 0U || options_.maximumCandidates > 256U) {
            constructionError_ = Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio discovery maximumCandidates must be between 1 and 256.");
        }
    }

    [[nodiscard]] Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>> discover(
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "discover the Windows LM Studio environment");
        if (!lease) {
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                std::move(lease).error());
        }
        if (constructionError_) {
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                *constructionError_);
        }
        try {
            std::vector<WindowsLMStudioDiscoveryCandidate> candidates;
            candidates.reserve(options_.maximumCandidates);
            if (options_.inspectInstalledApplicationRegistrations) {
                auto registered = appendRegisteredApplications(candidates, context);
                if (!registered) {
                    return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                        std::move(registered).error());
                }
            }
            if (options_.inspectKnownUserLocations) {
                auto known = appendKnownLocations(candidates, context);
                if (!known) {
                    return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                        std::move(known).error());
                }
            }
            if (options_.inspectRunningProcesses) {
                auto running = appendRunningProcesses(candidates, context);
                if (!running) {
                    return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                        std::move(running).error());
                }
            }
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::success(
                std::move(candidates));
        } catch (...) {
            return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Windows LM Studio discovery failed at the native boundary."));
        }
    }

    void shutdown() noexcept { executor_.shutdown(); }

private:
    [[nodiscard]] Domain::Result<void> checkContext(
        const Domain::OperationContext& context) const noexcept
    {
        return Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "continue LM Studio discovery");
    }

    [[nodiscard]] Domain::Result<void> append(
        std::vector<WindowsLMStudioDiscoveryCandidate>& candidates,
        WindowsLMStudioDiscoveryCandidate candidate) const
    {
        if (candidates.size() >= options_.maximumCandidates) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Windows LM Studio discovery exceeded its candidate bound."));
        }
        candidates.emplace_back(std::move(candidate));
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> appendRegisteredApplications(
        std::vector<WindowsLMStudioDiscoveryCandidate>& candidates,
        const Domain::OperationContext& context) const
    {
        bool found{};
        UniqueRegistryHandle uninstall;
        constexpr wchar_t UninstallKey[] =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, UninstallKey, 0U, KEY_READ, uninstall.put()) ==
            ERROR_SUCCESS) {
            for (DWORD index = 0U; index < MaximumRegistrySubkeys; ++index) {
                auto active = checkContext(context);
                if (!active) {
                    return active;
                }
                std::array<wchar_t, 512U> name{};
                DWORD characters = static_cast<DWORD>(name.size());
                const LSTATUS enumerated = ::RegEnumKeyExW(
                    uninstall.get(),
                    index,
                    name.data(),
                    &characters,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr);
                if (enumerated == ERROR_NO_MORE_ITEMS) {
                    break;
                }
                if (enumerated != ERROR_SUCCESS) {
                    continue;
                }
                UniqueRegistryHandle entry;
                if (::RegOpenKeyExW(uninstall.get(), name.data(), 0U, KEY_READ, entry.put()) !=
                    ERROR_SUCCESS) {
                    continue;
                }
                const auto displayName = queryRegistryString(entry.get(), L"DisplayName");
                if (!displayName || !isLMStudioDisplayName(*displayName)) {
                    continue;
                }
                std::optional<std::wstring> executable;
                if (auto displayIcon = queryRegistryString(entry.get(), L"DisplayIcon")) {
                    executable = executableFromDisplayIcon(std::move(*displayIcon));
                }
                if (!executable) {
                    if (auto installation = queryRegistryString(entry.get(), L"InstallLocation")) {
                        executable = (std::filesystem::path{*installation} /
                                      L"LM Studio.exe").native();
                    }
                }
                auto version = queryRegistryString(entry.get(), L"DisplayVersion");
                std::optional<std::string> encodedVersion;
                if (version) {
                    auto utf8 = Detail::strictUtf16ToUtf8(*version);
                    if (utf8) {
                        encodedVersion = std::move(utf8).value();
                    }
                }
                if (!executable) {
                    continue;
                }
                auto candidate = executableCandidate(
                    Domain::LMStudioDiscoverySource::InstalledApplication,
                    *executable,
                    std::move(encodedVersion),
                    "Discovered by enumerating the current-user uninstall registrations.");
                if (!candidate) {
                    return Domain::Result<void>::failure(std::move(candidate).error());
                }
                found = true;
                auto added = append(candidates, std::move(candidate).value());
                if (!added) {
                    return added;
                }
            }
        }

        UniqueRegistryHandle protocol;
        constexpr wchar_t ProtocolCommandKey[] =
            L"Software\\Classes\\lmstudio\\shell\\open\\command";
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, ProtocolCommandKey, 0U, KEY_READ, protocol.put()) ==
            ERROR_SUCCESS) {
            if (auto command = queryRegistryString(protocol.get(), nullptr)) {
                if (auto executable = executableFromProtocolCommand(*command)) {
                    auto candidate = executableCandidate(
                        Domain::LMStudioDiscoverySource::InstalledApplication,
                        *executable,
                        std::nullopt,
                        "Discovered from the registered current-user lmstudio protocol.");
                    if (!candidate) {
                        return Domain::Result<void>::failure(std::move(candidate).error());
                    }
                    found = true;
                    auto added = append(candidates, std::move(candidate).value());
                    if (!added) {
                        return added;
                    }
                }
            }
        }
        if (!found) {
            auto evidence = Domain::PathText::create(
                "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
            if (!evidence) {
                return Domain::Result<void>::failure(std::move(evidence).error());
            }
            return append(
                candidates,
                unavailableCandidate(
                    Domain::LMStudioDiscoverySource::InstalledApplication,
                    std::move(evidence).value(),
                    "No current-user LM Studio application registration was found."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> appendKnownLocations(
        std::vector<WindowsLMStudioDiscoveryCandidate>& candidates,
        const Domain::OperationContext& context) const
    {
        auto active = checkContext(context);
        if (!active) {
            return active;
        }
        std::optional<std::wstring> profile;
        if (options_.userProfileRoot) {
            auto converted = widePath(*options_.userProfileRoot);
            if (!converted) {
                return Domain::Result<void>::failure(std::move(converted).error());
            }
            profile = std::move(converted).value();
        } else {
            profile = environmentPath(L"USERPROFILE");
        }
        std::optional<std::wstring> localApplicationData;
        if (options_.localApplicationDataRoot) {
            auto converted = widePath(*options_.localApplicationDataRoot);
            if (!converted) {
                return Domain::Result<void>::failure(std::move(converted).error());
            }
            localApplicationData = std::move(converted).value();
        } else {
            localApplicationData = environmentPath(L"LOCALAPPDATA");
        }

        if (localApplicationData) {
            const auto executable =
                (std::filesystem::path{*localApplicationData} / L"Programs" /
                 L"LM Studio" / L"LM Studio.exe").native();
            auto candidate = executableCandidate(
                Domain::LMStudioDiscoverySource::KnownUserLocation,
                executable,
                std::nullopt,
                "Inspected the documented per-user LM Studio installation location.");
            if (!candidate) {
                return Domain::Result<void>::failure(std::move(candidate).error());
            }
            auto added = append(candidates, std::move(candidate).value());
            if (!added) {
                return added;
            }
        }
        if (profile) {
            const auto configuration =
                (std::filesystem::path{*profile} / L".lmstudio" / L"mcp.json").native();
            auto candidate = configurationCandidate(
                Domain::LMStudioDiscoverySource::KnownUserLocation,
                configuration,
                "Inspected the documented per-user LM Studio MCP configuration location.");
            if (!candidate) {
                return Domain::Result<void>::failure(std::move(candidate).error());
            }
            auto added = append(candidates, std::move(candidate).value());
            if (!added) {
                return added;
            }
        }
        if (!profile && !localApplicationData) {
            auto evidence = Domain::PathText::create("%USERPROFILE%\\.lmstudio");
            if (!evidence) {
                return Domain::Result<void>::failure(std::move(evidence).error());
            }
            return append(
                candidates,
                unavailableCandidate(
                    Domain::LMStudioDiscoverySource::KnownUserLocation,
                    std::move(evidence).value(),
                    "Windows could not resolve the current user's known roots."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> appendRunningProcesses(
        std::vector<WindowsLMStudioDiscoveryCandidate>& candidates,
        const Domain::OperationContext& context) const
    {
        bool found{};
        Detail::UniqueHandle snapshot{
            ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
        const bool enumerationAvailable = static_cast<bool>(snapshot);
        if (snapshot) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            BOOL next = ::Process32FirstW(snapshot.get(), &entry);
            while (next != FALSE) {
                auto active = checkContext(context);
                if (!active) {
                    return active;
                }
                if (lowercase(entry.szExeFile) == L"lm studio.exe") {
                    Detail::UniqueHandle process{::OpenProcess(
                        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
                    if (process) {
                        std::wstring image(32U * 1024U, L'\0');
                        DWORD characters = static_cast<DWORD>(image.size());
                        if (::QueryFullProcessImageNameW(
                                process.get(), 0U, image.data(), &characters) != FALSE) {
                            image.resize(characters);
                            auto candidate = executableCandidate(
                                Domain::LMStudioDiscoverySource::RunningProcess,
                                image,
                                std::nullopt,
                                "Discovered from a running LM Studio process image.");
                            if (!candidate) {
                                return Domain::Result<void>::failure(
                                    std::move(candidate).error());
                            }
                            found = true;
                            auto added = append(candidates, std::move(candidate).value());
                            if (!added) {
                                return added;
                            }
                        } else {
                            auto evidence = Domain::PathText::create("LM Studio.exe");
                            if (!evidence) {
                                return Domain::Result<void>::failure(std::move(evidence).error());
                            }
                            found = true;
                            auto added = append(
                                candidates,
                                unavailableCandidate(
                                    Domain::LMStudioDiscoverySource::RunningProcess,
                                    std::move(evidence).value(),
                                    "A running LM Studio process denied image-path inspection."));
                            if (!added) {
                                return added;
                            }
                        }
                    } else {
                        auto evidence = Domain::PathText::create("LM Studio.exe");
                        if (!evidence) {
                            return Domain::Result<void>::failure(std::move(evidence).error());
                        }
                        found = true;
                        auto added = append(
                            candidates,
                            unavailableCandidate(
                                Domain::LMStudioDiscoverySource::RunningProcess,
                                std::move(evidence).value(),
                                "A running LM Studio process denied query access."));
                        if (!added) {
                            return added;
                        }
                    }
                }
                next = ::Process32NextW(snapshot.get(), &entry);
            }
        }
        if (!found) {
            auto evidence = Domain::PathText::create("LM Studio.exe");
            if (!evidence) {
                return Domain::Result<void>::failure(std::move(evidence).error());
            }
            return append(
                candidates,
                unavailableCandidate(
                    Domain::LMStudioDiscoverySource::RunningProcess,
                    std::move(evidence).value(),
                    !enumerationAvailable ?
                        "Windows denied process enumeration for LM Studio discovery." :
                        "No running LM Studio process was found."));
        }
        return Domain::Result<void>::success();
    }

    WindowsLMStudioDiscoverySourceOptions options_;
    Detail::BoundedSerialExecutor executor_;
    std::optional<Domain::Error> constructionError_;
};

WindowsLMStudioDiscoverySource::WindowsLMStudioDiscoverySource(
    WindowsLMStudioDiscoverySourceOptions options)
    : implementation_{std::make_unique<Impl>(std::move(options))}
{
}

WindowsLMStudioDiscoverySource::~WindowsLMStudioDiscoverySource()
{
    shutdown();
}

Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>
WindowsLMStudioDiscoverySource::discover(const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->discover(context);
    } catch (...) {
        return Domain::Result<std::vector<WindowsLMStudioDiscoveryCandidate>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Windows LM Studio source discovery failed."));
    }
}

void WindowsLMStudioDiscoverySource::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

class WindowsLMStudioEnvironment::Impl final {
public:
    Impl(
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IFileSystem& fileSystem,
        IWindowsLMStudioDiscoverySource& discoverySource,
        WindowsLMStudioEnvironmentOptions options)
        : workspaceAuthority_{workspaceAuthority},
          fileSystem_{fileSystem},
          discoverySource_{discoverySource},
          options_{std::move(options)}
    {
        if (options_.maximumCandidates == 0U || options_.maximumCandidates > 256U ||
            options_.maximumConfigurationBytes == 0U ||
            options_.maximumConfigurationBytes > Contracts::IAtomicFileStore::MaximumBytes ||
            options_.maximumJsonDepth == 0U || options_.maximumJsonDepth > 128U) {
            constructionError_ = Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio environment bounds are outside their supported ranges.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioEnvironmentStatus> inspect(
        const std::optional<Domain::PathText>& explicitConfigurationPath,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "inspect the LM Studio environment");
        if (!lease) {
            return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                std::move(lease).error());
        }
        if (constructionError_) {
            return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(*constructionError_);
        }
        if (!containsAccess(readAuthority.grants(), Domain::FileAccess::Read) ||
            containsAccess(readAuthority.denials(), Domain::FileAccess::Read)) {
            return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "LM Studio inspection requires an authority that grants read access."));
        }
        try {
            auto discovered = discoverySource_.discover(context);
            if (!discovered) {
                return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                    std::move(discovered).error());
            }
            std::vector<WindowsLMStudioDiscoveryCandidate> candidates;
            candidates.reserve(discovered.value().size() +
                               static_cast<std::size_t>(explicitConfigurationPath.has_value()));
            if (explicitConfigurationPath) {
                candidates.emplace_back(WindowsLMStudioDiscoveryCandidate{
                    Domain::LMStudioDiscoverySource::ExplicitConfiguration,
                    *explicitConfigurationPath,
                    std::nullopt,
                    *explicitConfigurationPath,
                    std::nullopt,
                    std::nullopt,
                    true,
                    "Inspected the explicitly supplied LM Studio configuration path."});
            }
            for (auto& candidate : discovered.value()) {
                candidates.emplace_back(std::move(candidate));
            }
            if (candidates.size() > options_.maximumCandidates) {
                return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "LM Studio environment inspection exceeded its candidate bound."));
            }
            std::stable_sort(
                candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
                    return sourcePriority(left.source) < sourcePriority(right.source);
                });

            Domain::LMStudioEnvironmentStatus status;
            status.discoveryEvidence.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                auto active = Detail::validateOperationContext(
                    context,
                    std::chrono::steady_clock::now(),
                    "continue LM Studio environment inspection");
                if (!active) {
                    return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                        std::move(active).error());
                }
                bool valid = candidate.valid;
                bool selected{};
                std::string detail = candidate.detail;
                const std::size_t resourceCount =
                    static_cast<std::size_t>(candidate.applicationExecutable.has_value()) +
                    static_cast<std::size_t>(candidate.configurationPath.has_value());
                if (candidate.valid && resourceCount != 1U) {
                    valid = false;
                    detail = appendDetail(
                        std::move(detail),
                        "The source candidate did not identify exactly one resource.");
                }
                if (candidate.configurationPath && candidate.valid && resourceCount == 1U) {
                    auto validation = validateConfiguration(
                        workspaceAuthority_,
                        fileSystem_,
                        *candidate.configurationPath,
                        readAuthority,
                        options_,
                        context);
                    if (!validation) {
                        return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                            std::move(validation).error());
                    }
                    valid = validation.value().valid;
                    detail = appendDetail(
                        std::move(detail), validation.value().detail);
                    if (valid && !status.configurationPath) {
                        status.configurationPath = candidate.configurationPath;
                        selected = true;
                    }
                }
                if (candidate.applicationExecutable && valid &&
                    !status.applicationExecutable) {
                    status.applicationExecutable = candidate.applicationExecutable;
                    status.installationRoot = candidate.installationRoot ?
                        candidate.installationRoot : parentPath(*candidate.applicationExecutable);
                    status.version = candidate.version;
                    selected = true;
                }
                status.discoveryEvidence.emplace_back(Domain::LMStudioDiscoveryEvidence{
                    candidate.source,
                    candidate.evidencePath,
                    valid,
                    selected,
                    std::move(detail)});
            }
            status.lmStudioPresent = status.applicationExecutable.has_value();
            return Domain::Result<Domain::LMStudioEnvironmentStatus>::success(
                std::move(status));
        } catch (...) {
            return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "LM Studio environment inspection failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioConnectionHealth> connectionHealth(
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "read LM Studio connection health");
        if (!lease) {
            return Domain::Result<Domain::LMStudioConnectionHealth>::failure(
                std::move(lease).error());
        }
        try {
            return Domain::Result<Domain::LMStudioConnectionHealth>::success(cachedHealth_);
        } catch (...) {
            return Domain::Result<Domain::LMStudioConnectionHealth>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "LM Studio connection health could not be copied."));
        }
    }

    [[nodiscard]] Domain::Result<void> cacheConnectionHealth(
        Domain::LMStudioConnectionHealth health,
        const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "cache LM Studio connection health");
        if (!lease) {
            return Domain::Result<void>::failure(std::move(lease).error());
        }
        if (!validConnectionHealth(health)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio connection health is inconsistent or exceeds its bounds."));
        }
        try {
            cachedHealth_ = std::move(health);
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio connection health could not be cached."));
        }
    }

    void shutdown() noexcept { executor_.shutdown(); }

private:
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    Contracts::IFileSystem& fileSystem_;
    IWindowsLMStudioDiscoverySource& discoverySource_;
    WindowsLMStudioEnvironmentOptions options_;
    Detail::BoundedSerialExecutor executor_;
    Domain::LMStudioConnectionHealth cachedHealth_;
    std::optional<Domain::Error> constructionError_;
};

WindowsLMStudioEnvironment::WindowsLMStudioEnvironment(
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    IWindowsLMStudioDiscoverySource& discoverySource,
    WindowsLMStudioEnvironmentOptions options)
    : implementation_{std::make_unique<Impl>(
          workspaceAuthority, fileSystem, discoverySource, std::move(options))}
{
}

WindowsLMStudioEnvironment::~WindowsLMStudioEnvironment()
{
    shutdown();
}

Domain::Result<Domain::LMStudioEnvironmentStatus> WindowsLMStudioEnvironment::inspect(
    const std::optional<Domain::PathText>& explicitConfigurationPath,
    const Contracts::WorkspaceAuthority& readAuthority,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->inspect(explicitConfigurationPath, readAuthority, context);
    } catch (...) {
        return Domain::Result<Domain::LMStudioEnvironmentStatus>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio environment inspection failed at its ABI boundary."));
    }
}

Domain::Result<Domain::LMStudioConnectionHealth>
WindowsLMStudioEnvironment::connectionHealth(
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->connectionHealth(context);
    } catch (...) {
        return Domain::Result<Domain::LMStudioConnectionHealth>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio connection health failed at its ABI boundary."));
    }
}

Domain::Result<void> WindowsLMStudioEnvironment::cacheConnectionHealth(
    Domain::LMStudioConnectionHealth health,
    const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->cacheConnectionHealth(std::move(health), context);
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio connection health caching failed at its ABI boundary."));
    }
}

void WindowsLMStudioEnvironment::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
