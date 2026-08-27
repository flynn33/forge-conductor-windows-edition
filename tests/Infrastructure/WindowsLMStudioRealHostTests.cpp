#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using Json = nlohmann::json;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;
namespace NativeWindows = ForgeConductor::NativeTools::Windows;

constexpr std::size_t MaximumConfigurationBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumSynchronizationBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumEvidenceBytes = 1024U * 1024U;
constexpr std::size_t MaximumForeignEntries = 4'096U;
constexpr std::size_t MaximumForeignManifestPathBytes = 4U * 1024U;
constexpr std::size_t MaximumForeignManifestBytes = 32U * 1024U * 1024U;
constexpr std::uint64_t MaximumForeignFileBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumForeignTreeBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t MaximumDiagnosticEvents = 128U;
constexpr std::chrono::minutes DeploymentTimeout{5};
constexpr std::chrono::seconds ActivationTimeout{120};
constexpr std::string_view DeploymentToolName = "install-lmstudio-plugin";

class RunnerFailure final : public std::runtime_error {
public:
    RunnerFailure(std::string stage, Domain::Error error)
        : std::runtime_error{error.message}, stage_{std::move(stage)}, error_{std::move(error)}
    {
    }

    [[nodiscard]] const std::string& stage() const noexcept { return stage_; }
    [[nodiscard]] const Domain::Error& error() const noexcept { return error_; }

private:
    std::string stage_;
    Domain::Error error_;
};

[[noreturn]] void fail(
    const std::string_view stage,
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    throw RunnerFailure{
        std::string{stage}, Domain::makeError(code, std::move(message), retryable)};
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result, const std::string_view stage)
{
    if (!result) {
        throw RunnerFailure{std::string{stage}, std::move(result).error()};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result, const std::string_view stage)
{
    if (!result) {
        throw RunnerFailure{std::string{stage}, std::move(result).error()};
    }
}

[[nodiscard]] std::string truncate(std::string value, const std::size_t maximumBytes)
{
    if (value.size() > maximumBytes) {
        value.resize(maximumBytes);
    }
    return value;
}

[[nodiscard]] std::string wideToUtf8(const std::wstring_view input)
{
    if (input.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        fail("path_conversion", Domain::ErrorCodes::InvalidRequest,
             "A supplied Windows path is not valid Unicode.");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            result.data(), required, nullptr, nullptr) != required) {
        fail("path_conversion", Domain::ErrorCodes::InternalFailure,
             "A Windows path could not be converted to UTF-8.");
    }
    return result;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string_view input)
{
    if (input.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0);
    if (required <= 0) {
        fail("path_conversion", Domain::ErrorCodes::InvalidRequest,
             "A production path is not valid UTF-8.");
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            result.data(), required) != required) {
        fail("path_conversion", Domain::ErrorCodes::InternalFailure,
             "A UTF-8 path could not be converted to Windows Unicode.");
    }
    return result;
}

[[nodiscard]] std::filesystem::path requireAbsolutePath(
    const wchar_t* const value,
    const std::string_view stage)
{
    if (value == nullptr || *value == L'\0') {
        fail(stage, Domain::ErrorCodes::InvalidRequest,
             "A required absolute path argument is empty.");
    }
    std::filesystem::path path{value};
    if (!path.is_absolute()) {
        fail(stage, Domain::ErrorCodes::InvalidRequest,
             "The real-host runner accepts absolute paths only.");
    }
    return path.lexically_normal();
}

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path& path)
{
    return take(Domain::PathText::create(wideToUtf8(path.native())), "path_model");
}

[[nodiscard]] bool equalPath(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const auto leftNative = left.lexically_normal().native();
    const auto rightNative = right.lexically_normal().native();
    return leftNative.size() == rightNative.size() &&
        ::CompareStringOrdinal(
            leftNative.data(), static_cast<int>(leftNative.size()),
            rightNative.data(), static_cast<int>(rightNative.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool lmStudioProcessIsRunning(
    const std::filesystem::path& expectedExecutable,
    const std::string_view stage)
{
    HANDLE raw = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
    if (raw == INVALID_HANDLE_VALUE) {
        fail(stage, Domain::ErrorCodes::HostCapabilityUnavailable,
             "The expected LM Studio process state could not be inspected.");
    }
    struct HandleOwner final {
        HANDLE value;
        ~HandleOwner() noexcept { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    } owner{raw};

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(raw, &entry) == FALSE) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            return false;
        }
        fail(stage, Domain::ErrorCodes::HostCapabilityUnavailable,
             "The expected LM Studio process enumeration failed.");
    }
    constexpr std::wstring_view ExpectedLeaf{L"LM Studio.exe"};
    do {
        const std::wstring_view leaf{entry.szExeFile};
        if (leaf.size() == ExpectedLeaf.size() &&
            ::CompareStringOrdinal(
                leaf.data(), static_cast<int>(leaf.size()),
                ExpectedLeaf.data(), static_cast<int>(ExpectedLeaf.size()), TRUE) == CSTR_EQUAL) {
            HANDLE processRaw = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (processRaw == nullptr) {
                fail(stage, Domain::ErrorCodes::HostCapabilityUnavailable,
                     "A candidate LM Studio process image could not be inspected.");
            }
            HandleOwner process{processRaw};
            std::wstring image(32U * 1024U, L'\0');
            DWORD characters = static_cast<DWORD>(image.size());
            if (::QueryFullProcessImageNameW(
                    processRaw, 0U, image.data(), &characters) == FALSE) {
                fail(stage, Domain::ErrorCodes::HostCapabilityUnavailable,
                     "A candidate LM Studio process image path could not be inspected.");
            }
            image.resize(characters);
            if (equalPath(std::filesystem::path{image}, expectedExecutable)) {
                return true;
            }
        }
    } while (::Process32NextW(raw, &entry) != FALSE);
    if (::GetLastError() != ERROR_NO_MORE_FILES) {
        fail(stage, Domain::ErrorCodes::HostCapabilityUnavailable,
             "The expected LM Studio process enumeration ended unexpectedly.");
    }
    return false;
}

struct HostProcessObservation final {
    bool inspectionSucceeded{};
    bool running{};
};

[[nodiscard]] HostProcessObservation observeLMStudioProcess(
    const std::filesystem::path& expectedExecutable,
    const std::string_view stage) noexcept
{
    try {
        return HostProcessObservation{
            true, lmStudioProcessIsRunning(expectedExecutable, stage)};
    } catch (...) {
        return HostProcessObservation{};
    }
}

[[nodiscard]] Json hostProcessEvidence(const HostProcessObservation& observation)
{
    return Json{{"inspection_succeeded", observation.inspectionSucceeded},
                {"running", observation.running}};
}

[[nodiscard]] bool isWithin(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto candidateText = candidate.lexically_normal().native();
    auto rootText = root.lexically_normal().native();
    while (rootText.size() > 3U &&
           (rootText.back() == L'\\' || rootText.back() == L'/')) {
        rootText.pop_back();
    }
    if (candidateText.size() < rootText.size() ||
        ::CompareStringOrdinal(
            candidateText.data(), static_cast<int>(rootText.size()),
            rootText.data(), static_cast<int>(rootText.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    return candidateText.size() == rootText.size() ||
        candidateText[rootText.size()] == L'\\' ||
        candidateText[rootText.size()] == L'/';
}

[[nodiscard]] std::vector<Domain::PathText> coalesceRoots(
    std::vector<std::filesystem::path> roots)
{
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        return left.native().size() < right.native().size();
    });
    std::vector<std::filesystem::path> admitted;
    for (auto& root : roots) {
        root = root.lexically_normal();
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            fail("authority_roots", Domain::ErrorCodes::RecordNotFound,
                 "A required maintenance-authority root is not an existing directory.");
        }
        if (std::any_of(admitted.begin(), admitted.end(), [&](const auto& current) {
                return equalPath(root, current) || isWithin(root, current);
            })) {
            continue;
        }
        admitted.emplace_back(std::move(root));
    }
    if (admitted.empty() ||
        admitted.size() > Windows::WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy) {
        fail("authority_roots", Domain::ErrorCodes::LimitExceeded,
             "The exact maintenance-authority root set is empty or exceeds its bound.");
    }
    std::vector<Domain::PathText> result;
    result.reserve(admitted.size());
    for (const auto& root : admitted) {
        result.emplace_back(pathText(root));
    }
    return result;
}

[[nodiscard]] std::string environmentValue(const wchar_t* const name)
{
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U || required > 32U * 1024U) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0U || written >= required) {
        return {};
    }
    value.resize(written);
    return wideToUtf8(value);
}

[[nodiscard]] bool startsWithInsensitive(
    const std::string_view value,
    const std::string_view prefix)
{
    if (value.size() < prefix.size()) {
        return false;
    }
    const auto left = utf8ToWide(std::string{value.substr(0U, prefix.size())});
    const auto right = utf8ToWide(prefix);
    return left.size() == right.size() &&
        ::CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::string sanitizePath(std::string value)
{
    for (const auto& replacement : std::array{
             std::pair{environmentValue(L"USERPROFILE"), std::string{"<user-profile>"}},
             std::pair{environmentValue(L"LOCALAPPDATA"), std::string{"<local-app-data>"}}}) {
        if (!replacement.first.empty() && startsWithInsensitive(value, replacement.first)) {
            value.replace(0U, replacement.first.size(), replacement.second);
            return truncate(std::move(value), 512U);
        }
    }
    const auto marker = value.find("\\out\\build\\windows-msvc-x64\\");
    if (marker != std::string::npos) {
        return truncate(std::string{"<workspace>"} + value.substr(marker), 512U);
    }
    const auto separator = value.find_last_of("\\/");
    return separator == std::string::npos
        ? truncate(std::move(value), 128U)
        : truncate(std::string{"<redacted-root>\\"} + value.substr(separator + 1U), 256U);
}

[[nodiscard]] std::string sanitizeMessage(std::string value)
{
    const auto profile = environmentValue(L"USERPROFILE");
    const auto local = environmentValue(L"LOCALAPPDATA");
    for (const auto& replacement : std::array{
             std::pair{profile, std::string{"<user-profile>"}},
             std::pair{local, std::string{"<local-app-data>"}}}) {
        if (replacement.first.empty()) {
            continue;
        }
        for (std::size_t offset = 0U; offset < value.size();) {
            const auto remaining = std::string_view{value}.substr(offset);
            if (startsWithInsensitive(remaining, replacement.first)) {
                value.replace(offset, replacement.first.size(), replacement.second);
                offset += replacement.second.size();
            } else {
                ++offset;
            }
        }
    }
    return truncate(std::move(value), 1'024U);
}

[[nodiscard]] Domain::OperationContext makeContext(
    Windows::WindowsUuidGenerator& uuidGenerator,
    const std::string_view correlation,
    const std::chrono::steady_clock::duration timeout)
{
    const auto uuid = take(uuidGenerator.next(), "operation_id");
    return Domain::OperationContext{
        take(Domain::OperationId::parse(uuid.value()), "operation_id"),
        std::chrono::steady_clock::now() + timeout,
        {},
        take(Domain::CorrelationId::parse(correlation), "correlation_id")};
}

template <typename StrongId>
[[nodiscard]] StrongId nextStrongId(
    Windows::WindowsUuidGenerator& uuidGenerator,
    const std::string_view stage)
{
    const auto uuid = take(uuidGenerator.next(), stage);
    return take(StrongId::parse(uuid.value()), stage);
}

class EvidenceDiagnosticSink final : public Contracts::IDiagnosticSink {
public:
    [[nodiscard]] Domain::Result<void> record(
        const Domain::DiagnosticEnvelope& event,
        const Domain::OperationContext&) noexcept override
    {
        try {
            auto validation = Domain::validateDiagnosticEnvelope(event);
            if (!validation) {
                return validation;
            }
            std::scoped_lock lock{mutex_};
            if (closed_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The real-host evidence diagnostic sink is closed."));
            }
            if (events_.size() == MaximumDiagnosticEvents) {
                events_.erase(events_.begin());
            }
            events_.push_back(event);
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The real-host evidence diagnostic sink failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::scoped_lock lock{mutex_};
            const auto count = (std::min)(maximumCount, events_.size());
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(
                std::vector<Domain::DiagnosticEnvelope>{events_.end() - count, events_.end()});
        } catch (...) {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                  "Diagnostic evidence could not be copied."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest&,
        const Contracts::WorkspaceAuthority&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::DiagnosticExportResult>::failure(
            Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                              "The bounded real-host sink does not export a second artifact."));
    }

    void shutdown() noexcept override
    {
        std::scoped_lock lock{mutex_};
        closed_ = true;
    }

private:
    std::mutex mutex_;
    std::vector<Domain::DiagnosticEnvelope> events_;
    bool closed_{};
};

struct FileSnapshot final {
    bool exists{};
    std::uint64_t bytes{};
    std::string sha256;
    std::vector<std::byte> content;
};

[[nodiscard]] FileSnapshot snapshotFile(
    const std::filesystem::path& path,
    const std::uint64_t maximumBytes,
    Windows::BCryptSha256Hasher& hasher,
    const bool required)
{
    HANDLE raw = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (!required && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            const std::array<std::byte, 0U> empty{};
            return FileSnapshot{false, 0U,
                take(hasher.sha256(empty), "hash_absent_file").value(), {}};
        }
        fail("snapshot_file", Domain::ErrorCodes::RecordNotFound,
             "A required real-host snapshot file could not be opened.");
    }
    struct HandleOwner final {
        HANDLE value;
        ~HandleOwner() noexcept { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    } owner{raw};

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (::GetFileInformationByHandleEx(
            raw, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        fail("snapshot_file", Domain::ErrorCodes::IntegrityFailure,
             "A snapshot target is not a regular non-reparse file.");
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(raw, &size) == FALSE || size.QuadPart < 0) {
        fail("snapshot_file", Domain::ErrorCodes::LimitExceeded,
             "A real-host snapshot file exceeds its configured byte bound.");
    }
    const auto unsignedSize = static_cast<std::uint64_t>(size.QuadPart);
    if (unsignedSize > maximumBytes ||
        unsignedSize > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        fail("snapshot_file", Domain::ErrorCodes::LimitExceeded,
             "A real-host snapshot file exceeds its configured byte bound.");
    }
    std::vector<std::byte> content(static_cast<std::size_t>(unsignedSize));
    std::size_t offset{};
    while (offset < content.size()) {
        const DWORD requested = static_cast<DWORD>((std::min)(
            content.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read{};
        if (::ReadFile(raw, content.data() + offset, requested, &read, nullptr) == FALSE ||
            read == 0U) {
            fail("snapshot_file", Domain::ErrorCodes::IntegrityFailure,
                 "A real-host snapshot file changed or failed during bounded reading.");
        }
        offset += read;
    }
    return FileSnapshot{
        true,
        static_cast<std::uint64_t>(content.size()),
        take(hasher.sha256(content), "hash_snapshot_file").value(),
        std::move(content)};
}

[[nodiscard]] Json fileEvidence(const FileSnapshot& snapshot)
{
    return Json{{"exists", snapshot.exists},
                {"bytes", snapshot.bytes},
                {"sha256", snapshot.sha256}};
}

[[nodiscard]] std::string digestText(
    const std::string_view value,
    Windows::BCryptSha256Hasher& hasher)
{
    const auto bytes = std::span{
        reinterpret_cast<const std::byte*>(value.data()), value.size()};
    return take(hasher.sha256(bytes), "hash_manifest").value();
}

struct StrictConfiguration final {
    Windows::LMStudioConfigurationDocument document;
    Json root;
};

[[nodiscard]] StrictConfiguration strictConfiguration(
    const FileSnapshot& snapshot,
    const std::string_view stage)
{
    if (!snapshot.exists) {
        fail(stage, Domain::ErrorCodes::RecordNotFound,
             "A required LM Studio configuration snapshot does not exist.");
    }
    auto document = take(
        Windows::LMStudioConfigurationCodec::parse(
            std::span<const std::byte>{snapshot.content}),
        stage);
    auto root = Json::parse(document.sourceUtf8(), nullptr, false, false);
    if (root.is_discarded() || !root.is_object()) {
        fail(stage, Domain::ErrorCodes::MalformedMessage,
             "A strictly validated LM Studio configuration could not be decoded.");
    }
    return StrictConfiguration{std::move(document), std::move(root)};
}

struct ForeignConfigurationSnapshot final {
    std::size_t serverCount{};
    std::string sha256;
};

[[nodiscard]] std::optional<std::string> stringMember(
    const Json& object,
    const std::string_view name)
{
    const auto member = object.find(name);
    if (member == object.end() || !member->is_string()) {
        return std::nullopt;
    }
    return member->get<std::string>();
}

[[nodiscard]] std::string normalizedWindowsPath(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char byte) {
        if (byte == '/') {
            return '\\';
        }
        return static_cast<char>(std::tolower(byte));
    });
    while (value.size() > 3U && value.back() == '\\') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool legacyForgeLauncher(
    const std::string_view id,
    const Json& entry,
    const Domain::PathText& forgeHome)
{
    if (!entry.is_object()) {
        return false;
    }
    const bool primary = id == "forge-serve";
    const bool fallback = id == "forge-serve-fallback";
    if (!primary && !fallback) {
        return false;
    }
    const auto command = stringMember(entry, "command");
    if (!command) {
        return false;
    }
    const auto arguments = entry.find("args");
    if (arguments != entry.end() &&
        (!arguments->is_array() || !arguments->empty())) {
        return false;
    }
    auto ownedRoot = normalizedWindowsPath(forgeHome.value());
    ownedRoot.append("\\bin\\");
    const auto ownedLeaf = primary ? "forge-serve" : "forge-serve-fallback";
    const auto normalizedCommand = normalizedWindowsPath(*command);
    return normalizedCommand == ownedRoot + ownedLeaf ||
        normalizedCommand == ownedRoot + ownedLeaf + ".cmd";
}

[[nodiscard]] ForeignConfigurationSnapshot snapshotForeignConfiguration(
    const FileSnapshot& snapshot,
    const Domain::PathText& forgeHome,
    Windows::BCryptSha256Hasher& hasher)
{
    auto strict = strictConfiguration(snapshot, "snapshot_configuration");
    auto& root = strict.root;
    auto servers = root.find("mcpServers");
    if (servers == root.end() || !servers->is_object()) {
        fail("snapshot_configuration", Domain::ErrorCodes::MalformedMessage,
             "LM Studio mcp.json does not contain an object mcpServers member.");
    }
    servers->erase(Windows::LMStudioPrimaryServerId);
    servers->erase(Windows::LMStudioFallbackServerId);
    for (auto iterator = servers->begin(); iterator != servers->end();) {
        if (legacyForgeLauncher(iterator.key(), iterator.value(), forgeHome)) {
            iterator = servers->erase(iterator);
        } else {
            ++iterator;
        }
    }
    const auto count = servers->size();
    const auto canonical = root.dump();
    return ForeignConfigurationSnapshot{count, digestText(canonical, hasher)};
}

[[nodiscard]] bool equalPluginName(
    const std::wstring_view name,
    const std::wstring_view expected) noexcept
{
    return name.size() == expected.size() &&
        ::CompareStringOrdinal(
            name.data(), static_cast<int>(name.size()),
            expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool ownedPluginName(const std::wstring_view name) noexcept
{
    return equalPluginName(name, L"forge-conductor") ||
        equalPluginName(name, L"forge-conductor-fallback");
}

[[nodiscard]] bool transactionPluginName(const std::wstring_view name) noexcept
{
    constexpr std::wstring_view TransactionPrefix{L".forge-conductor-install-"};
    return name.size() >= TransactionPrefix.size() &&
        ::CompareStringOrdinal(
            name.data(), static_cast<int>(TransactionPrefix.size()),
            TransactionPrefix.data(), static_cast<int>(TransactionPrefix.size()), TRUE) ==
        CSTR_EQUAL;
}

struct ForeignPluginSnapshot final {
    std::size_t rootEntries{};
    std::size_t treeEntries{};
    std::uint64_t fileBytes{};
    std::string sha256;
    std::vector<std::string> transactionRoots;
    std::string transactionSha256;
};

[[nodiscard]] ForeignPluginSnapshot snapshotForeignPlugins(
    const std::filesystem::path& pluginsRoot,
    Windows::BCryptSha256Hasher& hasher)
{
    std::error_code error;
    if (!std::filesystem::exists(pluginsRoot, error)) {
        if (error) {
            fail("snapshot_plugins", Domain::ErrorCodes::InternalFailure,
                 "The LM Studio plugin root could not be inspected.");
        }
        return ForeignPluginSnapshot{
            0U, 0U, 0U, digestText("", hasher), {}, digestText("", hasher)};
    }
    const DWORD rootAttributes = ::GetFileAttributesW(pluginsRoot.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        fail("snapshot_plugins", Domain::ErrorCodes::IntegrityFailure,
             "The LM Studio plugin root is not a regular non-reparse directory.");
    }

    std::vector<std::filesystem::path> foreignRoots;
    std::vector<std::string> transactionRoots;
    std::size_t enumeratedRoots{};
    for (std::filesystem::directory_iterator iterator{pluginsRoot, error}, end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            fail("snapshot_plugins", Domain::ErrorCodes::InternalFailure,
                 "A foreign LM Studio plugin root could not be enumerated.");
        }
        if (++enumeratedRoots > MaximumForeignEntries) {
            fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                 "LM Studio plugin roots exceed the bounded snapshot inventory.");
        }
        const auto name = iterator->path().filename().native();
        if (ownedPluginName(name)) {
            continue;
        }
        const DWORD attributes = ::GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            fail("snapshot_plugins", Domain::ErrorCodes::IntegrityFailure,
                 "An LM Studio plugin root is inaccessible or a reparse point.");
        }
        if (transactionPluginName(name) &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            auto portableName = wideToUtf8(name);
            if (portableName.size() > MaximumForeignManifestPathBytes) {
                fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                     "An LM Studio transaction-root name exceeds the evidence path bound.");
            }
            transactionRoots.emplace_back(std::move(portableName));
        }
        foreignRoots.push_back(iterator->path());
    }
    if (error) {
        fail("snapshot_plugins", Domain::ErrorCodes::InternalFailure,
             "A foreign LM Studio plugin root could not be enumerated.");
    }
    std::sort(foreignRoots.begin(), foreignRoots.end(), [](const auto& left, const auto& right) {
        return wideToUtf8(left.filename().native()) < wideToUtf8(right.filename().native());
    });
    std::sort(transactionRoots.begin(), transactionRoots.end());

    std::vector<std::string> manifest;
    std::uint64_t totalBytes{};
    std::size_t entryCount{};
    std::size_t manifestBytes{};
    for (const auto& foreignRoot : foreignRoots) {
        std::vector<std::filesystem::path> paths{foreignRoot};
        const DWORD attributes = ::GetFileAttributesW(foreignRoot.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            fail("snapshot_plugins", Domain::ErrorCodes::IntegrityFailure,
                 "A foreign LM Studio plugin entry is inaccessible or a reparse point.");
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            for (std::filesystem::recursive_directory_iterator iterator{foreignRoot, error}, end;
                 iterator != end; iterator.increment(error)) {
                if (error) {
                    fail("snapshot_plugins", Domain::ErrorCodes::InternalFailure,
                         "A foreign LM Studio plugin tree could not be enumerated.");
                }
                paths.push_back(iterator->path());
                if (paths.size() + entryCount > MaximumForeignEntries) {
                    fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                         "Foreign LM Studio plugin contents exceed the snapshot entry bound.");
                }
            }
        }
        std::sort(paths.begin(), paths.end(), [&](const auto& left, const auto& right) {
            return wideToUtf8(left.lexically_relative(pluginsRoot).native()) <
                wideToUtf8(right.lexically_relative(pluginsRoot).native());
        });
        for (const auto& path : paths) {
            if (++entryCount > MaximumForeignEntries) {
                fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                     "Foreign LM Studio plugin contents exceed the snapshot entry bound.");
            }
            const DWORD itemAttributes = ::GetFileAttributesW(path.c_str());
            if (itemAttributes == INVALID_FILE_ATTRIBUTES ||
                (itemAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                fail("snapshot_plugins", Domain::ErrorCodes::IntegrityFailure,
                     "A foreign LM Studio plugin entry is inaccessible or a reparse point.");
            }
            const auto relative = wideToUtf8(path.lexically_relative(pluginsRoot).native());
            if (relative.empty() || relative.size() > MaximumForeignManifestPathBytes) {
                fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                     "A foreign LM Studio plugin path exceeds the manifest path bound.");
            }
            std::string manifestLine;
            if ((itemAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                manifestLine = "D\t" + relative + "\n";
            } else {
                auto file = snapshotFile(path, MaximumForeignFileBytes, hasher, true);
                if (file.bytes > MaximumForeignTreeBytes - totalBytes) {
                    fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                         "Foreign LM Studio plugin contents exceed the snapshot byte bound.");
                }
                totalBytes += file.bytes;
                manifestLine =
                    "F\t" + relative + "\t" + std::to_string(file.bytes) + "\t" +
                    file.sha256 + "\n";
            }
            if (manifestLine.size() > MaximumForeignManifestBytes - manifestBytes) {
                fail("snapshot_plugins", Domain::ErrorCodes::LimitExceeded,
                     "Foreign LM Studio plugin metadata exceeds the manifest byte bound.");
            }
            manifestBytes += manifestLine.size();
            manifest.emplace_back(std::move(manifestLine));
        }
    }
    std::sort(manifest.begin(), manifest.end());
    std::string encoded;
    encoded.reserve(manifestBytes);
    for (const auto& line : manifest) {
        encoded.append(line);
    }
    std::string encodedTransactions;
    for (const auto& name : transactionRoots) {
        encodedTransactions.append(name);
        encodedTransactions.push_back('\n');
    }
    return ForeignPluginSnapshot{
        foreignRoots.size(),
        entryCount,
        totalBytes,
        digestText(encoded, hasher),
        std::move(transactionRoots),
        digestText(encodedTransactions, hasher)};
}

[[nodiscard]] Json foreignPluginEvidence(const ForeignPluginSnapshot& snapshot)
{
    return Json{{"root_entries", snapshot.rootEntries},
                {"tree_entries", snapshot.treeEntries},
                {"file_bytes", snapshot.fileBytes},
                {"sha256", snapshot.sha256},
                {"transaction_root_count", snapshot.transactionRoots.size()},
                {"transaction_roots_sha256", snapshot.transactionSha256}};
}

[[nodiscard]] Json foreignConfigurationEvidence(
    const ForeignConfigurationSnapshot& snapshot)
{
    return Json{{"server_count", snapshot.serverCount}, {"sha256", snapshot.sha256}};
}

[[nodiscard]] bool identicalSnapshot(
    const FileSnapshot& left,
    const FileSnapshot& right) noexcept
{
    return left.exists == right.exists && left.bytes == right.bytes &&
        left.sha256 == right.sha256 && left.content == right.content;
}

struct SynchronizationVerification final {
    bool completeRoleObjectsEqual{};
    bool liveCodecRegistered{};
    bool synchronizedCodecRegistered{};
};

[[nodiscard]] SynchronizationVerification verifySynchronizedConfiguration(
    const FileSnapshot& liveSnapshot,
    const FileSnapshot& synchronizedSnapshot,
    const std::filesystem::path& expectedBinary,
    const std::filesystem::path& expectedForgeHome,
    const Domain::DeploymentId& deploymentId)
{
    auto live = strictConfiguration(liveSnapshot, "strict_live_configuration");
    auto synchronized = strictConfiguration(
        synchronizedSnapshot, "strict_synchronized_configuration");
    const auto binary = pathText(expectedBinary);
    const auto forgeHome = pathText(expectedForgeHome);
    const auto liveInspection = take(
        Windows::LMStudioConfigurationCodec::inspect(
            live.document, binary, forgeHome),
        "inspect_live_configuration");
    const auto synchronizedInspection = take(
        Windows::LMStudioConfigurationCodec::inspect(
            synchronized.document, binary, forgeHome),
        "inspect_synchronized_configuration");
    const auto inspectionMatches = [&](const auto& inspection) {
        return inspection.registered && inspection.deploymentId &&
            inspection.deploymentId.value() == deploymentId &&
            inspection.roles.size() == 2U &&
            std::all_of(
                inspection.roles.begin(), inspection.roles.end(),
                [](const auto& role) {
                    return role.present && role.valid && role.deploymentId.has_value();
                });
    };
    if (!inspectionMatches(liveInspection) ||
        !inspectionMatches(synchronizedInspection)) {
        fail("verify_synchronized_configuration", Domain::ErrorCodes::IntegrityFailure,
             "Codec inspection did not prove the expected binary, Forge home, roles, and deployment revision in both configurations.");
    }

    const auto roleObjects = [](const Json& root) -> const Json& {
        const auto servers = root.find("mcpServers");
        if (servers == root.end() || !servers->is_object()) {
            fail("verify_synchronized_configuration", Domain::ErrorCodes::MalformedMessage,
                 "A strictly parsed LM Studio configuration has no mcpServers object.");
        }
        return *servers;
    };
    const auto& liveRoles = roleObjects(live.root);
    const auto& synchronizedRoles = roleObjects(synchronized.root);
    for (const auto* const id :
         {Windows::LMStudioPrimaryServerId, Windows::LMStudioFallbackServerId}) {
        const auto liveRole = liveRoles.find(id);
        const auto synchronizedRole = synchronizedRoles.find(id);
        if (liveRole == liveRoles.end() || synchronizedRole == synchronizedRoles.end() ||
            !liveRole->is_object() || !synchronizedRole->is_object() ||
            *liveRole != *synchronizedRole) {
            fail("verify_synchronized_configuration", Domain::ErrorCodes::IntegrityFailure,
                 "LM Studio did not synchronize the complete primary and fallback role objects exactly.");
        }
    }
    return SynchronizationVerification{true, true, true};
}

[[nodiscard]] Contracts::AuthorizedToolCall authorizeDeploymentTool(
    ForgeConductor::Mcp::McpToolAuthorizer& authorizer,
    Windows::WindowsUuidGenerator& uuidGenerator,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::ToolEffect effect,
    const Domain::OperationContext& context)
{
    const auto requestUuid = take(uuidGenerator.next(), "tool_request_id");
    Domain::ToolCallRequest call{
        Domain::McpRequestMetadata{
            take(Domain::RequestId::parse(requestUuid.value()), "tool_request_id"),
            context.correlationId,
            authority.callerId(),
            authority.projectId(),
            "2025-11-25"},
        std::string{DeploymentToolName},
        "{\"preserve_foreign_entries\":true}"};
    return take(
        authorizer.authorize(
            Domain::ToolAuthorizationRequest{
                std::move(call),
                effect,
                Domain::AuthorityReference{
                    authority.authorityId(), authority.generation()}},
            authority,
            context),
        "authorize_install_lmstudio_plugin");
}

[[nodiscard]] std::filesystem::path candidateConfiguration(
    const std::vector<Windows::WindowsLMStudioDiscoveryCandidate>& candidates)
{
    const auto match = std::find_if(candidates.begin(), candidates.end(), [](const auto& item) {
        return item.valid && item.configurationPath.has_value();
    });
    if (match == candidates.end()) {
        fail("discover_configuration", Domain::ErrorCodes::HostCapabilityUnavailable,
             "Production discovery found no valid LM Studio mcp.json candidate.");
    }
    return std::filesystem::path{utf8ToWide(match->configurationPath->value())};
}

[[nodiscard]] std::filesystem::path candidateExecutable(
    const std::vector<Windows::WindowsLMStudioDiscoveryCandidate>& candidates)
{
    const auto match = std::find_if(candidates.begin(), candidates.end(), [](const auto& item) {
        return item.valid && item.applicationExecutable.has_value();
    });
    if (match == candidates.end()) {
        fail("discover_application", Domain::ErrorCodes::HostCapabilityUnavailable,
             "Production discovery found no valid LM Studio application executable.");
    }
    return std::filesystem::path{utf8ToWide(match->applicationExecutable->value())};
}

[[nodiscard]] Json pluginStatusEvidence(const Domain::LMStudioPluginStatus& status)
{
    return Json{
        {"primary_installed", status.primaryPluginInstalled},
        {"fallback_installed", status.fallbackPluginInstalled},
        {"configuration_registered", status.mcpConfigurationRegistered},
        {"binary_executable", status.binaryExecutable},
        {"lm_studio_present", status.lmStudioPresent},
        {"deployment_id", status.deploymentId ? status.deploymentId->value() : ""}};
}

[[nodiscard]] Json activationEvidence(
    const Domain::LMStudioHostActivationResult& activation)
{
    return Json{
        {"deployment_id", activation.deploymentId.value()},
        {"running_before_deploy", activation.runningBeforeDeploy},
        {"launched", activation.launched},
        {"restarted", activation.restarted},
        {"configuration_synchronized", activation.configurationSynchronized},
        {"ready_role_count", activation.readyRoles.size()}};
}

void writeEvidence(const std::filesystem::path& path, const Json& evidence)
{
    const std::string encoded = evidence.dump(2) + "\n";
    if (encoded.size() > MaximumEvidenceBytes) {
        fail("write_evidence", Domain::ErrorCodes::LimitExceeded,
             "Sanitized real-host JSON evidence exceeds one MiB.");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(path.parent_path(), error) || error) {
        fail("write_evidence", Domain::ErrorCodes::RecordNotFound,
             "The pre-authorized P15 evidence directory does not exist.");
    }
    const auto temporary = path.native() + L".tmp." + std::to_wstring(::GetCurrentProcessId());
    HANDLE raw = ::CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        fail("write_evidence", Domain::ErrorCodes::InternalFailure,
             "The temporary P15 evidence file could not be opened.");
    }
    struct HandleOwner final {
        HANDLE value;
        ~HandleOwner() noexcept { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    } owner{raw};
    std::size_t offset{};
    while (offset < encoded.size()) {
        const DWORD requested = static_cast<DWORD>((std::min)(
            encoded.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        if (::WriteFile(raw, encoded.data() + offset, requested, &written, nullptr) == FALSE ||
            written == 0U) {
            ::DeleteFileW(temporary.c_str());
            fail("write_evidence", Domain::ErrorCodes::InternalFailure,
                 "The temporary P15 evidence file could not be written.");
        }
        offset += written;
    }
    if (::FlushFileBuffers(raw) == FALSE) {
        ::DeleteFileW(temporary.c_str());
        fail("write_evidence", Domain::ErrorCodes::InternalFailure,
             "The temporary P15 evidence file could not be flushed.");
    }
    ::CloseHandle(owner.value);
    owner.value = INVALID_HANDLE_VALUE;
    if (::MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        ::DeleteFileW(temporary.c_str());
        fail("write_evidence", Domain::ErrorCodes::InternalFailure,
             "The P15 evidence file could not be committed atomically.");
    }
}

void preflightEvidenceTarget(
    const std::filesystem::path& path,
    const Json& evidence)
{
    const DWORD parentAttributes = ::GetFileAttributesW(path.parent_path().c_str());
    if (parentAttributes == INVALID_FILE_ATTRIBUTES ||
        (parentAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (parentAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        fail("preflight_evidence", Domain::ErrorCodes::IntegrityFailure,
             "The evidence parent must be an existing non-reparse directory.");
    }
    const DWORD targetAttributes = ::GetFileAttributesW(path.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES) {
        fail("preflight_evidence", Domain::ErrorCodes::Conflict,
             "The one-shot real-host evidence target already exists.");
    }
    const DWORD targetError = ::GetLastError();
    if (targetError != ERROR_FILE_NOT_FOUND && targetError != ERROR_PATH_NOT_FOUND) {
        fail("preflight_evidence", Domain::ErrorCodes::InternalFailure,
             "The evidence target absence could not be established.");
    }
    writeEvidence(path, evidence);
}

void writeFailureEvidenceNoThrow(
    const std::optional<std::filesystem::path>& path,
    const bool evidencePrepared,
    const Json& evidence) noexcept
{
    if (!path) {
        return;
    }
    try {
        if (evidencePrepared) {
            writeEvidence(*path, evidence);
        } else {
            preflightEvidenceTarget(*path, evidence);
        }
    } catch (...) {
    }
}

void runQualification(
    const std::filesystem::path& preferredBinary,
    const std::optional<std::filesystem::path>& explicitConfiguration,
    const std::filesystem::path& expectedLMStudioExecutable,
    const std::filesystem::path& evidencePath,
    Json& evidence,
    bool& deploymentCommitted)
{
    if (!std::filesystem::is_regular_file(preferredBinary)) {
        fail("preferred_binary", Domain::ErrorCodes::RecordNotFound,
             "The preferred Debug forge-conductor.exe does not exist.");
    }
    if (explicitConfiguration) {
        if (!std::filesystem::is_regular_file(*explicitConfiguration) ||
            explicitConfiguration->filename().native() != L"mcp.json" ||
            explicitConfiguration->parent_path().filename().native() != L".lmstudio") {
            fail("explicit_configuration", Domain::ErrorCodes::InvalidRequest,
                 "The optional configuration must be an existing .lmstudio\\mcp.json file.");
        }
    }
    if (!std::filesystem::is_regular_file(expectedLMStudioExecutable)) {
        fail("expected_lmstudio_executable", Domain::ErrorCodes::RecordNotFound,
             "The expected baseline LM Studio executable does not exist.");
    }

    Windows::SystemClock clock;
    Windows::WindowsUuidGenerator uuidGenerator;
    Windows::BCryptSha256Hasher hasher;
    Windows::WindowsApplicationPaths applicationPaths;
    auto discoveryContext = makeContext(
        uuidGenerator, "p15-real-host-discovery", std::chrono::seconds{30});
    const auto dataRoot = take(applicationPaths.dataRoot(discoveryContext), "forge_data_root");
    const auto dataRootPath = std::filesystem::path{utf8ToWide(dataRoot.value())};
    const auto forgeHome = pathText(dataRootPath);
    std::error_code directoryError;
    std::filesystem::create_directories(dataRootPath, directoryError);
    if (directoryError) {
        fail("forge_data_root", Domain::ErrorCodes::InternalFailure,
             "The Forge Conductor application data root could not be established.");
    }

    Windows::WindowsLMStudioDiscoverySourceOptions discoveryOptions;
    if (explicitConfiguration) {
        discoveryOptions.userProfileRoot = pathText(
            explicitConfiguration->parent_path().parent_path());
    }
    Windows::WindowsLMStudioDiscoverySource discoverySource{std::move(discoveryOptions)};
    const auto candidates = take(discoverySource.discover(discoveryContext), "lmstudio_discovery");
    const auto discoveredConfiguration = candidateConfiguration(candidates).lexically_normal();
    const auto discoveredExecutable = candidateExecutable(candidates).lexically_normal();
    if (explicitConfiguration && !equalPath(discoveredConfiguration, *explicitConfiguration)) {
        fail("explicit_configuration", Domain::ErrorCodes::IntegrityFailure,
             "Production discovery did not select the explicitly bound LM Studio configuration.");
    }
    if (!equalPath(discoveredExecutable, expectedLMStudioExecutable)) {
        fail("expected_lmstudio_executable", Domain::ErrorCodes::IntegrityFailure,
             "Production discovery selected a different LM Studio executable than the captured baseline.");
    }

    const auto writeRoots = coalesceRoots({
        discoveredConfiguration.parent_path(),
        preferredBinary.parent_path(),
        dataRootPath});
    const auto executeRoots = coalesceRoots({
        discoveredConfiguration.parent_path(),
        preferredBinary.parent_path(),
        discoveredExecutable.parent_path(),
        dataRootPath});

    const auto projectId = nextStrongId<Domain::ProjectId>(uuidGenerator, "project_id");
    const auto writeAuthorityId =
        nextStrongId<Domain::AuthorityId>(uuidGenerator, "write_authority_id");
    const auto executeAuthorityId =
        nextStrongId<Domain::AuthorityId>(uuidGenerator, "execute_authority_id");
    const auto callerId = take(
        Domain::ClientId::parse("p15-lmstudio-maintenance"), "maintenance_caller");

    auto writeProvider = std::make_shared<Windows::WindowsWorkspaceAuthority>(
        std::vector<Windows::WindowsWorkspaceAuthorityPolicy>{
            Windows::WindowsWorkspaceAuthorityPolicy{
                writeAuthorityId,
                projectId,
                callerId,
                writeRoots,
                Domain::FileAccess::Write,
                {Domain::FileAccess::Read,
                 Domain::FileAccess::Write,
                 Domain::FileAccess::Create,
                 Domain::FileAccess::Delete,
                 Domain::FileAccess::Execute},
                {},
                true,
                1U}});
    auto executeProvider = std::make_shared<Windows::WindowsWorkspaceAuthority>(
        std::vector<Windows::WindowsWorkspaceAuthorityPolicy>{
            Windows::WindowsWorkspaceAuthorityPolicy{
                executeAuthorityId,
                projectId,
                callerId,
                executeRoots,
                Domain::FileAccess::Execute,
                {Domain::FileAccess::Read, Domain::FileAccess::Execute},
                {},
                true,
                1U}});

    const auto writeAuthority = take(
        writeProvider->authorityFor(projectId, discoveryContext), "write_authority");
    const auto executeAuthority = take(
        executeProvider->authorityFor(projectId, discoveryContext), "execute_authority");

    const auto budgets = Domain::budgetsForProfile(Domain::ResourceProfile::Standard16GiB);
    auto atomicFileStore = std::make_shared<Windows::WindowsAtomicFileStore>();
    NativeWindows::WindowsFileSystem fileSystem{atomicFileStore};
    auto runtimeDiagnostics =
        std::make_shared<Windows::WindowsRuntimeDiagnostics>(clock, budgets);
    Windows::WindowsProcessSupervisor processSupervisor{budgets, runtimeDiagnostics};
    Windows::WindowsLMStudioServeVerifier serveVerifier{processSupervisor};
    Windows::WindowsLMStudioEnvironment writeEnvironment{
        *writeProvider, fileSystem, discoverySource};
    Windows::WindowsLMStudioEnvironment executeEnvironment{
        *executeProvider, fileSystem, discoverySource};
    Windows::WindowsLMStudioHostActivator hostActivator{
        *executeProvider, fileSystem, clock};
    EvidenceDiagnosticSink diagnostics;
    Windows::WindowsLMStudioDeploymentService writeDeploymentService{
        writeEnvironment,
        serveVerifier,
        hostActivator,
        *writeProvider,
        fileSystem,
        *atomicFileStore,
        applicationPaths,
        clock,
        uuidGenerator,
        diagnostics};

    const auto environment = take(
        writeEnvironment.inspect(std::nullopt, writeAuthority, discoveryContext),
        "inspect_lmstudio_environment");
    if (!environment.lmStudioPresent || !environment.configurationPath ||
        !environment.applicationExecutable ||
        !equalPath(
            std::filesystem::path{utf8ToWide(environment.configurationPath->value())},
            discoveredConfiguration) ||
        !equalPath(
            std::filesystem::path{utf8ToWide(environment.applicationExecutable->value())},
            discoveredExecutable)) {
        fail("inspect_lmstudio_environment", Domain::ErrorCodes::IntegrityFailure,
             "Production LM Studio inspection did not retain the preflight-selected resources.");
    }

    const auto configurationPath =
        std::filesystem::path{utf8ToWide(environment.configurationPath->value())};
    const auto lmStudioRoot = configurationPath.parent_path();
    const auto synchronizationPath =
        lmStudioRoot / L".internal" / L"last-synced-mcp-state.json";
    const auto pluginsRoot = lmStudioRoot / L"extensions" / L"plugins" / L"mcp";

    const auto binarySnapshot =
        snapshotFile(preferredBinary, MaximumForeignFileBytes, hasher, true);
    const auto configBefore =
        snapshotFile(configurationPath, MaximumConfigurationBytes, hasher, true);
    const auto syncBefore =
        snapshotFile(synchronizationPath, MaximumSynchronizationBytes, hasher, false);
    const auto foreignConfigBefore = snapshotForeignConfiguration(
        configBefore, forgeHome, hasher);
    const auto foreignPluginsBefore = snapshotForeignPlugins(pluginsRoot, hasher);

    const Domain::LMStudioDeploymentRequest request{
        pathText(preferredBinary), true};
    const auto statusBefore = take(
        writeDeploymentService.status(request, writeAuthority, discoveryContext),
        "status_before_deployment");

    evidence["host"] = Json{
        {"lm_studio_present", environment.lmStudioPresent},
        {"version", environment.version ? truncate(*environment.version, 64U) : ""},
        {"configuration", sanitizePath(environment.configurationPath->value())},
        {"application", sanitizePath(environment.applicationExecutable->value())},
        {"discovery_evidence_count", environment.discoveryEvidence.size()},
        {"expected_application_bound", true}};
    evidence["authority"] = Json{
        {"project_id", projectId.value()},
        {"caller", callerId.value()},
        {"write_root_count", writeRoots.size()},
        {"execute_root_count", executeRoots.size()},
        {"selected_roots_only", true},
        {"write_intent", "write"},
        {"execute_intent", "execute"},
        {"execute_shell_enabled", executeAuthority.shellEnabled()},
        {"exact_tool", DeploymentToolName}};
    evidence["binary"] = Json{
        {"path", sanitizePath(wideToUtf8(preferredBinary.native()))},
        {"bytes", binarySnapshot.bytes},
        {"sha256", binarySnapshot.sha256}};
    evidence["before"] = Json{
        {"configuration", fileEvidence(configBefore)},
        {"synchronized_state", fileEvidence(syncBefore)},
        {"foreign_configuration", foreignConfigurationEvidence(foreignConfigBefore)},
        {"foreign_plugins", foreignPluginEvidence(foreignPluginsBefore)},
        {"status", pluginStatusEvidence(statusBefore)}};

    const auto hostAtPreflight = observeLMStudioProcess(
        expectedLMStudioExecutable, "host_preflight");
    evidence["host_observations"] = Json{
        {"preflight", hostProcessEvidence(hostAtPreflight)}};
    evidence["result"] = Json{
        {"status", "ready_for_deployment"},
        {"deployment_left_installed", false},
        {"rollback_requested", false}};
    writeEvidence(evidencePath, evidence);
    if (!hostAtPreflight.inspectionSucceeded) {
        fail("host_preflight", Domain::ErrorCodes::HostCapabilityUnavailable,
             "The expected LM Studio process state could not be established before deployment.");
    }
    if (hostAtPreflight.running) {
        fail("host_preflight", Domain::ErrorCodes::Conflict,
             "LM Studio must be stopped so G15 can prove the supported launch path; it was not killed or restarted.");
    }

    auto deploymentContext = makeContext(
        uuidGenerator, "p15-real-host-deploy", DeploymentTimeout);
    ForgeConductor::Mcp::McpToolAuthorizer authorizer{clock};
    const auto writeAuthorization = authorizeDeploymentTool(
        authorizer, uuidGenerator, writeAuthority, Domain::ToolEffect::Write,
        deploymentContext);
    const auto deployment = take(
        writeDeploymentService.deploy(
            request, writeAuthority, writeAuthorization, deploymentContext),
        "deploy_lmstudio_plugins");
    deploymentCommitted = deployment.ok;
    evidence["deployment"] = Json{
        {"committed", deploymentCommitted},
        {"deployment_id", deployment.deploymentId.value()},
        {"plugin_count", deployment.pluginsWritten.size()},
        {"status", "committed_pending_validation"}};
    evidence["result"] = Json{
        {"status", deploymentCommitted
            ? "committed_pending_validation"
            : "deployment_not_committed"},
        {"deployment_left_installed", deploymentCommitted},
        {"rollback_requested", false}};
    writeEvidence(evidencePath, evidence);
    if (!deployment.ok ||
        !equalPath(
            std::filesystem::path{utf8ToWide(deployment.binaryPath.value())},
            preferredBinary) ||
        deployment.pluginsWritten.size() != 2U) {
        fail("deploy_lmstudio_plugins", Domain::ErrorCodes::IntegrityFailure,
             "Deployment returned an incomplete or mismatched committed result.");
    }

    auto statusContext = makeContext(
        uuidGenerator, "p15-real-host-status", std::chrono::seconds{30});
    const auto statusAfter = take(
        writeDeploymentService.status(request, writeAuthority, statusContext),
        "status_after_deployment");
    if (!statusAfter.primaryPluginInstalled || !statusAfter.fallbackPluginInstalled ||
        !statusAfter.mcpConfigurationRegistered || !statusAfter.binaryExecutable ||
        !statusAfter.deploymentId ||
        statusAfter.deploymentId.value() != deployment.deploymentId) {
        fail("status_after_deployment", Domain::ErrorCodes::IntegrityFailure,
             "Committed LM Studio deployment status reports drift or a revision mismatch.");
    }

    const auto configAfterDeploy =
        snapshotFile(configurationPath, MaximumConfigurationBytes, hasher, true);
    const auto syncAfterDeploy =
        snapshotFile(synchronizationPath, MaximumSynchronizationBytes, hasher, false);
    const auto foreignConfigAfterDeploy =
        snapshotForeignConfiguration(configAfterDeploy, forgeHome, hasher);
    const auto foreignPluginsAfterDeploy = snapshotForeignPlugins(pluginsRoot, hasher);
    const bool syncUnchangedBeforeActivation = identicalSnapshot(syncBefore, syncAfterDeploy);
    const bool foreignConfigurationSemanticsPreservedAfterDeploy =
        foreignConfigAfterDeploy.sha256 == foreignConfigBefore.sha256 &&
        foreignConfigAfterDeploy.serverCount == foreignConfigBefore.serverCount;
    const bool foreignPluginTreeBytesPreservedAfterDeploy =
        foreignPluginsAfterDeploy.sha256 == foreignPluginsBefore.sha256 &&
        foreignPluginsAfterDeploy.rootEntries == foreignPluginsBefore.rootEntries &&
        foreignPluginsAfterDeploy.treeEntries == foreignPluginsBefore.treeEntries &&
        foreignPluginsAfterDeploy.fileBytes == foreignPluginsBefore.fileBytes;
    const bool noNewTransactionRootsAfterDeploy =
        foreignPluginsAfterDeploy.transactionRoots == foreignPluginsBefore.transactionRoots;
    evidence["deployment"]["status"] = pluginStatusEvidence(statusAfter);
    evidence["after_deployment"] = Json{
        {"configuration", fileEvidence(configAfterDeploy)},
        {"synchronized_state", fileEvidence(syncAfterDeploy)},
        {"foreign_configuration", foreignConfigurationEvidence(foreignConfigAfterDeploy)},
        {"foreign_plugins", foreignPluginEvidence(foreignPluginsAfterDeploy)},
        {"synchronized_state_byte_identical_before_activation",
         syncUnchangedBeforeActivation},
        {"foreign_configuration_semantics_preserved",
         foreignConfigurationSemanticsPreservedAfterDeploy},
        {"foreign_plugin_tree_bytes_preserved",
         foreignPluginTreeBytesPreservedAfterDeploy},
        {"no_new_transaction_roots", noNewTransactionRootsAfterDeploy}};
    const auto hostBeforeActivation = observeLMStudioProcess(
        expectedLMStudioExecutable, "host_before_activation");
    evidence["host_observations"]["before_activation"] =
        hostProcessEvidence(hostBeforeActivation);
    evidence["result"] = Json{
        {"status", "committed_pending_activation"},
        {"deployment_left_installed", true},
        {"rollback_requested", false}};
    writeEvidence(evidencePath, evidence);
    if (!syncUnchangedBeforeActivation ||
        !foreignConfigurationSemanticsPreservedAfterDeploy ||
        !foreignPluginTreeBytesPreservedAfterDeploy ||
        !noNewTransactionRootsAfterDeploy) {
        fail("foreign_state_after_deployment", Domain::ErrorCodes::IntegrityFailure,
             "Deployment changed the pre-activation synchronized-state bytes, foreign configuration semantics, foreign plugin-tree bytes, or transaction-root inventory.");
    }
    if (!hostBeforeActivation.inspectionSucceeded) {
        fail("host_before_activation", Domain::ErrorCodes::HostCapabilityUnavailable,
             "The expected LM Studio process state could not be established immediately before activation.");
    }
    if (hostBeforeActivation.running) {
        fail("host_before_activation", Domain::ErrorCodes::Conflict,
             "LM Studio began running before the runner invoked the supported activation path.");
    }

    Windows::WindowsLMStudioDeploymentService executeDeploymentService{
        executeEnvironment,
        serveVerifier,
        hostActivator,
        *executeProvider,
        fileSystem,
        *atomicFileStore,
        applicationPaths,
        clock,
        uuidGenerator,
        diagnostics};
    auto activationContext = makeContext(
        uuidGenerator, "p15-real-host-activate", ActivationTimeout + std::chrono::seconds{30});
    const auto executeAuthorization = authorizeDeploymentTool(
        authorizer, uuidGenerator, executeAuthority, Domain::ToolEffect::Execute,
        activationContext);
    auto activationResult = executeDeploymentService.activate(
        Domain::LMStudioHostActivationRequest{
            deployment.deploymentId, ActivationTimeout},
        executeAuthority,
        executeAuthorization,
        activationContext);
    if (!activationResult) {
        auto activationFailure = std::move(activationResult).error();
        const auto hostAfterFailedActivation = observeLMStudioProcess(
            expectedLMStudioExecutable, "host_after_failed_activation");
        evidence["host_observations"]["after_activation_attempt"] =
            hostProcessEvidence(hostAfterFailedActivation);
        evidence["activation"] = Json{
            {"status", "failed"},
            {"deployment_id", deployment.deploymentId.value()},
            {"error_code", truncate(activationFailure.code, 128U)},
            {"error", sanitizeMessage(activationFailure.message)},
            {"retryable", activationFailure.retryable},
            {"process_inspection_succeeded",
             hostAfterFailedActivation.inspectionSucceeded},
            {"host_running_after_attempt", hostAfterFailedActivation.running}};
        evidence["result"] = Json{
            {"status", "failed_activation_attempt"},
            {"deployment_left_installed", true},
            {"rollback_requested", false}};
        writeEvidence(evidencePath, evidence);
        throw RunnerFailure{"activate_lmstudio_host", std::move(activationFailure)};
    }
    const auto activation = std::move(activationResult).value();
    const auto hostAfterActivation = observeLMStudioProcess(
        expectedLMStudioExecutable, "host_after_activation");
    evidence["host_observations"]["after_activation_attempt"] =
        hostProcessEvidence(hostAfterActivation);
    evidence["activation"] = activationEvidence(activation);
    evidence["activation"]["status"] = "returned";
    evidence["activation"]["process_inspection_succeeded"] =
        hostAfterActivation.inspectionSucceeded;
    evidence["activation"]["host_running_after_attempt"] =
        hostAfterActivation.running;
    evidence["result"] = Json{
        {"status", "activation_returned_pending_validation"},
        {"deployment_left_installed", true},
        {"rollback_requested", false}};
    writeEvidence(evidencePath, evidence);
    if (activation.runningBeforeDeploy || !activation.launched ||
        !activation.configurationSynchronized ||
        activation.deploymentId != deployment.deploymentId || activation.restarted ||
        !hostAfterActivation.inspectionSucceeded || !hostAfterActivation.running) {
        fail("activate_lmstudio_host", Domain::ErrorCodes::IntegrityFailure,
             "LM Studio activation did not prove one fresh supported launch, a running expected host image, and exact revision synchronization.");
    }

    const auto configAfterActivation =
        snapshotFile(configurationPath, MaximumConfigurationBytes, hasher, true);
    const auto syncAfterActivation =
        snapshotFile(synchronizationPath, MaximumSynchronizationBytes, hasher, true);
    const auto foreignConfigAfterActivation =
        snapshotForeignConfiguration(configAfterActivation, forgeHome, hasher);
    const auto foreignPluginsAfterActivation = snapshotForeignPlugins(pluginsRoot, hasher);
    const auto synchronization = verifySynchronizedConfiguration(
        configAfterActivation,
        syncAfterActivation,
        preferredBinary,
        dataRootPath,
        deployment.deploymentId);
    const bool exactRevision = synchronization.completeRoleObjectsEqual &&
        synchronization.liveCodecRegistered &&
        synchronization.synchronizedCodecRegistered;
    const bool foreignConfigurationSemanticsPreserved =
        foreignConfigAfterActivation.sha256 == foreignConfigBefore.sha256 &&
        foreignConfigAfterActivation.serverCount == foreignConfigBefore.serverCount;
    const bool foreignPluginTreeBytesPreserved =
        foreignPluginsAfterActivation.sha256 == foreignPluginsBefore.sha256 &&
        foreignPluginsAfterActivation.rootEntries == foreignPluginsBefore.rootEntries &&
        foreignPluginsAfterActivation.treeEntries == foreignPluginsBefore.treeEntries &&
        foreignPluginsAfterActivation.fileBytes == foreignPluginsBefore.fileBytes;
    const bool noNewTransactionRootsAfterActivation =
        foreignPluginsAfterActivation.transactionRoots ==
        foreignPluginsBefore.transactionRoots;
    if (!exactRevision || !foreignConfigurationSemanticsPreserved ||
        !foreignPluginTreeBytesPreserved || !noNewTransactionRootsAfterActivation) {
        fail("verify_activation", Domain::ErrorCodes::IntegrityFailure,
             "The synchronized revision, foreign configuration semantics, foreign plugin-tree bytes, or transaction-root inventory failed final verification.");
    }

    const auto diagnosticEvents = take(
        diagnostics.recent(MaximumDiagnosticEvents, activationContext),
        "read_deployment_diagnostics");
    Json eventNames = Json::array();
    for (const auto& event : diagnosticEvents) {
        eventNames.push_back(truncate(event.event, Domain::MaximumDiagnosticEventBytes));
    }
    evidence["activation"]["status"] = "passed";
    evidence["after_activation"] = Json{
        {"configuration", fileEvidence(configAfterActivation)},
        {"synchronized_state", fileEvidence(syncAfterActivation)},
        {"foreign_configuration", foreignConfigurationEvidence(foreignConfigAfterActivation)},
        {"foreign_plugins", foreignPluginEvidence(foreignPluginsAfterActivation)},
        {"exact_synchronized_revision", exactRevision},
        {"complete_role_objects_equal", synchronization.completeRoleObjectsEqual},
        {"live_codec_registered", synchronization.liveCodecRegistered},
        {"synchronized_codec_registered",
         synchronization.synchronizedCodecRegistered},
        {"foreign_configuration_semantics_preserved",
         foreignConfigurationSemanticsPreserved},
        {"foreign_plugin_tree_bytes_preserved", foreignPluginTreeBytesPreserved},
        {"no_new_transaction_roots", noNewTransactionRootsAfterActivation}};
    evidence["diagnostics"] = Json{
        {"bounded_event_count", diagnosticEvents.size()}, {"events", std::move(eventNames)}};
    evidence["result"] = Json{
        {"status", "passed"},
        {"deployment_left_installed", true},
        {"rollback_requested", false}};
}

} // namespace
} // namespace ForgeConductor::Tests

int wmain(const int argumentCount, wchar_t** const arguments)
{
    using namespace ForgeConductor::Tests;
    if (argumentCount != 5) {
        std::cerr << "usage_error: WindowsLMStudioRealHostTests.exe "
                     "<absolute-forge-binary> <absolute-evidence-json> "
                     "<absolute-lmstudio-mcp-json> "
                     "<absolute-expected-lmstudio-executable>\n";
        return 2;
    }

    std::optional<std::filesystem::path> evidencePath;
    bool evidencePrepared{};
    bool deploymentCommitted{};
    Json evidence{
        {"schema_version", 1},
        {"gate", "G15"},
        {"phase", "P15"},
        {"runner", "ForgeConductor.LMStudio.RealHostTests"},
        {"bounded", true},
        {"sanitized", true}};
    try {
        evidencePath = requireAbsolutePath(arguments[2], "evidence_path");
        const auto preferredBinary = requireAbsolutePath(arguments[1], "preferred_binary");
        const std::optional<std::filesystem::path> explicitConfiguration{
            requireAbsolutePath(arguments[3], "explicit_configuration")};
        const auto expectedLMStudioExecutable = requireAbsolutePath(
            arguments[4], "expected_lmstudio_executable");
        if (equalPath(*evidencePath, preferredBinary) ||
            equalPath(*evidencePath, *explicitConfiguration) ||
            equalPath(*evidencePath, expectedLMStudioExecutable)) {
            fail("preflight_evidence", Domain::ErrorCodes::InvalidRequest,
                 "The evidence target must be distinct from every production input path.");
        }
        evidence["result"] = Json{
            {"status", "preflight"},
            {"deployment_left_installed", false},
            {"rollback_requested", false}};
        preflightEvidenceTarget(*evidencePath, evidence);
        evidencePrepared = true;
        runQualification(
            preferredBinary,
            explicitConfiguration,
            expectedLMStudioExecutable,
            *evidencePath,
            evidence,
            deploymentCommitted);
        writeEvidence(*evidencePath, evidence);
        std::cout << "G15 LM Studio real-host qualification passed; sanitized evidence written.\n";
        return 0;
    } catch (const RunnerFailure& failure) {
        evidence["result"] = Json{
            {"status", deploymentCommitted ? "failed_after_commit" : "failed"},
            {"stage", truncate(failure.stage(), 128U)},
            {"error_code", truncate(failure.error().code, 128U)},
            {"error", sanitizeMessage(failure.error().message)},
            {"retryable", failure.error().retryable},
            {"deployment_left_installed", deploymentCommitted},
            {"rollback_requested", false}};
        writeFailureEvidenceNoThrow(evidencePath, evidencePrepared, evidence);
        std::cerr << truncate(failure.stage(), 128U) << ": "
                  << truncate(failure.error().code, 128U) << ": "
                  << sanitizeMessage(failure.error().message) << '\n';
        return 1;
    } catch (const std::exception& exception) {
        evidence["result"] = Json{
            {"status", deploymentCommitted ? "failed_after_commit" : "failed"},
            {"stage", "exception_boundary"},
            {"error_code", std::string{Domain::ErrorCodes::InternalFailure}},
            {"error", sanitizeMessage(exception.what())},
            {"retryable", false},
            {"deployment_left_installed", deploymentCommitted},
            {"rollback_requested", false}};
        writeFailureEvidenceNoThrow(evidencePath, evidencePrepared, evidence);
        std::cerr << "exception_boundary: internal_failure: "
                  << sanitizeMessage(exception.what()) << '\n';
        return 1;
    } catch (...) {
        evidence["result"] = Json{
            {"status", deploymentCommitted ? "failed_after_commit" : "failed"},
            {"stage", "exception_boundary"},
            {"error_code", std::string{Domain::ErrorCodes::InternalFailure}},
            {"error", "Unknown native failure."},
            {"retryable", false},
            {"deployment_left_installed", deploymentCommitted},
            {"rollback_requested", false}};
        writeFailureEvidenceNoThrow(evidencePath, evidencePrepared, evidence);
        std::cerr << "exception_boundary: internal_failure: unknown native failure\n";
        return 1;
    }
}
