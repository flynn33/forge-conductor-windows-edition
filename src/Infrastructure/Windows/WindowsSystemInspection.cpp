#include "ForgeConductor/Infrastructure/Windows/WindowsSystemInspection.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UtfConversion.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winver.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <cstddef>
#include <cwchar>
#include <utility>
#include <string_view>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MaximumEntries = 256U;

[[nodiscard]] std::string utf8(const wchar_t* value)
{
    auto converted = Detail::strictUtf16ToUtf8(value);
    return converted ? std::move(converted).value() : std::string{};
}

[[nodiscard]] std::vector<std::byte> tokenUser(HANDLE token)
{
    DWORD needed{};
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &needed));
    if (needed == 0U || needed > 64U * 1024U) return {};
    std::vector<std::byte> buffer(needed);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) return {};
    return buffer;
}

[[nodiscard]] const char* serviceState(const DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_START_PENDING: return "start_pending";
    case SERVICE_STOP_PENDING: return "stop_pending";
    case SERVICE_RUNNING: return "running";
    case SERVICE_CONTINUE_PENDING: return "continue_pending";
    case SERVICE_PAUSE_PENDING: return "pause_pending";
    case SERVICE_PAUSED: return "paused";
    default: return "unknown";
    }
}

[[nodiscard]] bool active(const Domain::OperationContext& context)
{
    return !context.isCancellationRequested() && !context.isExpired(std::chrono::steady_clock::now());
}

[[nodiscard]] std::optional<std::string> productVersion(const std::wstring& executable)
{
    DWORD ignored{};
    const DWORD needed = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (needed == 0U || needed > 1024U * 1024U) return std::nullopt;
    std::vector<std::byte> bytes(needed);
    if (!GetFileVersionInfoW(executable.c_str(), 0U, needed, bytes.data())) return std::nullopt;
    struct Translation final { WORD language; WORD codePage; };
    void* raw{};
    UINT length{};
    if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", &raw, &length) &&
        raw && length >= sizeof(Translation)) {
        const auto& translation = *static_cast<const Translation*>(raw);
        std::array<wchar_t, 64> query{};
        if (swprintf_s(query.data(), query.size(), L"\\StringFileInfo\\%04x%04x\\ProductVersion",
            translation.language, translation.codePage) > 0 &&
            VerQueryValueW(bytes.data(), query.data(), &raw, &length) && raw && length > 1U && length <= 129U) {
            auto text = Detail::strictUtf16ToUtf8(std::wstring_view{static_cast<const wchar_t*>(raw), length - 1U});
            if (text && !text.value().empty()) return std::move(text).value();
        }
    }
    // The fixed product version is an observed binary resource if the vendor
    // omitted its string translation. It is not an inferred engine version.
    if (VerQueryValueW(bytes.data(), L"\\", &raw, &length) && raw && length >= sizeof(VS_FIXEDFILEINFO)) {
        const auto& value = *static_cast<const VS_FIXEDFILEINFO*>(raw);
        if (value.dwSignature == 0xFEEF04BDU) return
            std::to_string(HIWORD(value.dwProductVersionMS)) + "." + std::to_string(LOWORD(value.dwProductVersionMS)) + "." +
            std::to_string(HIWORD(value.dwProductVersionLS)) + "." + std::to_string(LOWORD(value.dwProductVersionLS));
    }
    return std::nullopt;
}

struct ServiceManager final {
    SC_HANDLE value{};
    ~ServiceManager() { if (value) CloseServiceHandle(value); }
};
} // namespace

Domain::Result<std::string> WindowsSystemInspection::inspect(
    const Domain::OperationContext& context) noexcept
{
    try {
        if (!active(context)) return Domain::Result<std::string>::failure(Domain::makeError(
            context.isCancellationRequested() ? Domain::ErrorCodes::Cancelled : Domain::ErrorCodes::DeadlineExceeded,
            "Windows process inspection did not begin within its operation deadline."));
        Json processes = Json::array();
        std::size_t unreadable{}, otherUser{}, matching{};
        HANDLE currentToken{};
        const bool tokenOpened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &currentToken) != FALSE;
        Detail::UniqueHandle ownedCurrentToken{currentToken};
        const auto currentUser = tokenOpened ? tokenUser(currentToken) : std::vector<std::byte>{};
        Detail::UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
        const bool available = !currentUser.empty() && snapshot.get() != INVALID_HANDLE_VALUE && snapshot.get() != nullptr;
        if (available) {
            PROCESSENTRY32W entry{};
            entry.dwSize = static_cast<DWORD>(sizeof(entry));
            if (Process32FirstW(snapshot.get(), &entry)) do {
                if (!active(context)) break;
                if (entry.th32ProcessID == GetCurrentProcessId()) {
                    ++matching;
                    Json current{{"pid", entry.th32ProcessID}, {"parent_pid", entry.th32ParentProcessID},
                        {"executable_name", utf8(entry.szExeFile)}, {"thread_count", entry.cntThreads}};
                    if (processes.size() == MaximumEntries) processes.erase(processes.end() - 1);
                    processes.insert(processes.begin(), std::move(current));
                    continue;
                }
                Detail::UniqueHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
                HANDLE processToken{};
                if (!process.get() || !OpenProcessToken(process.get(), TOKEN_QUERY, &processToken)) { ++unreadable; continue; }
                Detail::UniqueHandle ownedToken{processToken};
                const auto user = tokenUser(processToken);
                if (user.empty()) { ++unreadable; continue; }
                const auto* expected = reinterpret_cast<const TOKEN_USER*>(currentUser.data());
                const auto* actual = reinterpret_cast<const TOKEN_USER*>(user.data());
                if (!EqualSid(expected->User.Sid, actual->User.Sid)) { ++otherUser; continue; }
                ++matching;
                if (processes.size() < MaximumEntries) processes.push_back({
                    {"pid", entry.th32ProcessID}, {"parent_pid", entry.th32ParentProcessID},
                    {"executable_name", utf8(entry.szExeFile)}, {"thread_count", entry.cntThreads}});
            } while (Process32NextW(snapshot.get(), &entry));
        }
        Json services = Json::array();
        ServiceManager manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)};
        bool servicesAvailable{}, servicesTruncated{};
        DWORD serviceError{};
        if (manager.value) {
            DWORD needed{}, count{}, resume{};
            static_cast<void>(EnumServicesStatusExW(manager.value, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                SERVICE_STATE_ALL, nullptr, 0U, &needed, &count, &resume, nullptr));
            if (needed > 0U && needed <= 1024U * 1024U && active(context)) {
                std::vector<std::byte> buffer(needed);
                resume = 0U;
                const bool enumerated = EnumServicesStatusExW(manager.value, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                    SERVICE_STATE_ALL, reinterpret_cast<LPBYTE>(buffer.data()), static_cast<DWORD>(buffer.size()),
                    &needed, &count, &resume, nullptr) != FALSE;
                serviceError = enumerated ? ERROR_SUCCESS : GetLastError();
                servicesAvailable = enumerated || serviceError == ERROR_MORE_DATA;
                servicesTruncated = !enumerated || count > MaximumEntries;
                const auto* entries = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
                for (DWORD index{}; servicesAvailable && index < count && services.size() < MaximumEntries; ++index) {
                    services.push_back({{"name", utf8(entries[index].lpServiceName)},
                        {"display_name", utf8(entries[index].lpDisplayName)},
                        {"state", serviceState(entries[index].ServiceStatusProcess.dwCurrentState)},
                        {"pid", entries[index].ServiceStatusProcess.dwProcessId}});
                }
            } else serviceError = needed > 1024U * 1024U ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        } else serviceError = GetLastError();
        if (!active(context)) return Domain::Result<std::string>::failure(Domain::makeError(
            context.isCancellationRequested() ? Domain::ErrorCodes::Cancelled : Domain::ErrorCodes::DeadlineExceeded,
            "Windows process inspection did not complete before cancellation/deadline."));
        Json result{{"ok", true}, {"source", "Windows Toolhelp32 and Service Control Manager"},
            {"process_scope", "same_user_readable"}, {"processes_available", available},
            {"processes", std::move(processes)}, {"processes_truncated", matching > MaximumEntries},
            {"skipped_unreadable_processes", unreadable}, {"skipped_other_user_processes", otherUser},
            {"services_available", servicesAvailable}, {"services", std::move(services)},
            {"services_truncated", servicesTruncated}, {"services_win32_error", serviceError},
            {"observed_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}};
        return Domain::Result<std::string>::success(result.dump());
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Windows process inspection failed safely."));
    }
}

Domain::Result<std::string> WindowsSystemInspection::lmStudioDesktopVersion(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto failedContext = [&context]() {
            return Domain::Result<std::string>::failure(Domain::makeError(
                context.isCancellationRequested() ? Domain::ErrorCodes::Cancelled : Domain::ErrorCodes::DeadlineExceeded,
                "LM Studio process version inspection was cancelled or expired."));
        };
        if (!active(context)) return failedContext();
        HANDLE rawToken{};
        const bool opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) != FALSE;
        Detail::UniqueHandle token{rawToken};
        const auto currentUser = opened ? tokenUser(rawToken) : std::vector<std::byte>{};
        Detail::UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
        Json observations = Json::array();
        std::map<std::wstring, std::size_t> paths;
        std::set<std::string> versions;
        std::size_t missingVersions{}, unreadable{};
        bool truncated{};
        if (!currentUser.empty() && snapshot) {
            PROCESSENTRY32W entry{};
            entry.dwSize = static_cast<DWORD>(sizeof(entry));
            if (Process32FirstW(snapshot.get(), &entry)) do {
                if (!active(context)) return failedContext();
                if (CompareStringOrdinal(entry.szExeFile, -1, L"LM Studio.exe", -1, TRUE) != CSTR_EQUAL) continue;
                Detail::UniqueHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
                HANDLE processToken{};
                if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &processToken)) { ++unreadable; continue; }
                Detail::UniqueHandle ownedToken{processToken};
                const auto user = tokenUser(processToken);
                if (user.empty()) { ++unreadable; continue; }
                if (!EqualSid(reinterpret_cast<const TOKEN_USER*>(currentUser.data())->User.Sid,
                    reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid)) continue;
                std::wstring image(32768U, L'\0');
                DWORD characters = static_cast<DWORD>(image.size());
                if (!QueryFullProcessImageNameW(process.get(), 0U, image.data(), &characters) || characters == 0U) { ++unreadable; continue; }
                image.resize(characters);
                // Avoid network paths; this tool does not authenticate to a file share.
                if (image.size() < 3U || image[1U] != L':' || image[2U] != L'\\') { ++unreadable; continue; }
                auto existing = paths.find(image);
                if (existing != paths.end()) {
                    auto& pids = observations.at(existing->second).at("process_ids");
                    if (pids.size() < MaximumEntries) pids.push_back(entry.th32ProcessID); else truncated = true;
                    continue;
                }
                if (observations.size() >= 16U) { truncated = true; continue; }
                const auto version = productVersion(image);
                if (version) versions.insert(*version); else ++missingVersions;
                paths.emplace(image, observations.size());
                auto converted = Detail::strictUtf16ToUtf8(image);
                observations.push_back({{"executable_path", converted ? Json(converted.value()) : Json(nullptr)},
                    {"process_ids", Json::array({entry.th32ProcessID})},
                    {"product_version", version ? Json(*version) : Json(nullptr)}});
            } while (Process32NextW(snapshot.get(), &entry));
        }
        if (!active(context)) return failedContext();
        const bool known = versions.size() == 1U && missingVersions == 0U && !truncated;
        Json result{{"known", known}, {"value", known ? Json(*versions.begin()) : Json(nullptr)},
            {"source", "Win32 ProductVersion resource of confirmed same-user running LM Studio.exe images"},
            {"observations", std::move(observations)}, {"observations_truncated", truncated},
            {"unreadable_matching_processes", unreadable},
            {"reason", known ? Json(nullptr) : Json(versions.size() > 1U ?
                "Multiple running LM Studio desktop versions were observed." :
                "No single readable running LM Studio desktop binary version could be confirmed.")},
            {"server_api_version", nullptr}, {"inference_engine_version", nullptr},
            {"observed_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}};
        return Domain::Result<std::string>::success(result.dump());
    } catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, "LM Studio process version inspection failed safely.")); }
}

} // namespace ForgeConductor::Infrastructure::Windows
