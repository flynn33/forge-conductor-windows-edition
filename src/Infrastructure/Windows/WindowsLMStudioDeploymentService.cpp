#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h"

#include "Detail/UtfConversion.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

constexpr std::size_t MaximumPluginDocumentBytes = 64U * 1024U;
constexpr char InstallerId[] = "forge-conductor-app";
constexpr char LMStudioDeploymentToolName[] = "install-lmstudio-plugin";

class DeploymentFailure final : public std::runtime_error {
public:
    explicit DeploymentFailure(Domain::Error error)
        : std::runtime_error{error.message}, error_{std::move(error)}
    {
    }

    [[nodiscard]] Domain::Error releaseError() noexcept { return std::move(error_); }

private:
    Domain::Error error_;
};

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw DeploymentFailure{std::move(result).error()};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw DeploymentFailure{std::move(result).error()};
    }
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string_view value)
{
    std::vector<std::byte> bytes(value.size());
    if (!value.empty()) {
        std::memcpy(bytes.data(), value.data(), value.size());
    }
    return bytes;
}

[[nodiscard]] std::string textOf(const std::vector<std::byte>& bytes)
{
    std::string text(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(text.data(), bytes.data(), bytes.size());
    }
    return text;
}

[[nodiscard]] Domain::Result<Domain::PathText> parentPath(
    const Domain::PathText& path) noexcept
{
    try {
        const auto separator = path.value().find_last_of("\\/");
        if (separator == std::string::npos || separator == 0U) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio configuration path has no usable parent directory."));
        }
        return Domain::PathText::create(path.value().substr(0U, separator));
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio configuration parent path could not be derived."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> childPath(
    const Domain::PathText& parent,
    const std::string_view child) noexcept
{
    try {
        std::string value = parent.value();
        if (!value.empty() && value.back() != '\\' && value.back() != '/') {
            value.push_back('\\');
        }
        value.append(child);
        return Domain::PathText::create(value);
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio deployment child path could not be derived."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> currentModulePath() noexcept
{
    try {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U || length >= buffer.size()) {
            return Domain::Result<Domain::PathText>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The current Forge Conductor executable path could not be resolved."));
        }
        auto utf8 = Detail::strictUtf16ToUtf8(
            std::wstring_view{buffer.data(), static_cast<std::size_t>(length)});
        if (!utf8) {
            return Domain::Result<Domain::PathText>::failure(std::move(utf8).error());
        }
        return Domain::PathText::create(utf8.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The current Forge Conductor executable path could not be represented."));
    }
}

class UniqueNativeHandle final {
public:
    explicit UniqueNativeHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueNativeHandle() noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            ::CloseHandle(value_);
        }
    }

    UniqueNativeHandle(const UniqueNativeHandle&) = delete;
    UniqueNativeHandle& operator=(const UniqueNativeHandle&) = delete;
    UniqueNativeHandle(UniqueNativeHandle&&) = delete;
    UniqueNativeHandle& operator=(UniqueNativeHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_;
};

[[nodiscard]] Domain::Result<void> readExecutableBytes(
    const HANDLE file,
    const std::uint64_t offset,
    void* const destination,
    const DWORD size) noexcept
{
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The selected Forge executable header could not be positioned."));
    }
    DWORD read{};
    if (!::ReadFile(file, destination, size, &read, nullptr) || read != size) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The selected Forge executable header could not be read completely."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<bool> isExecutableWindowsBinary(
    const Domain::PathText& path) noexcept
{
    try {
        auto nativePath = Detail::strictUtf8ToUtf16(path.value());
        if (!nativePath) {
            return Domain::Result<bool>::failure(std::move(nativePath).error());
        }
        UniqueNativeHandle file{::CreateFileW(
            nativePath.value().c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!file.valid()) {
            const auto error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                error == ERROR_INVALID_NAME) {
                return Domain::Result<bool>::success(false);
            }
            return Domain::Result<bool>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The selected Forge executable could not be opened for PE validation.",
                true));
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!::GetFileInformationByHandleEx(
                file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) {
            return Domain::Result<bool>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The selected Forge executable attributes could not be inspected."));
        }
        if ((attributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
            return Domain::Result<bool>::success(false);
        }
        LARGE_INTEGER fileSize{};
        if (!::GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart < 0) {
            return Domain::Result<bool>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The selected Forge executable size could not be inspected."));
        }
        IMAGE_DOS_HEADER dos{};
        auto read = readExecutableBytes(
            file.get(), 0U, &dos, static_cast<DWORD>(sizeof(dos)));
        if (!read) {
            return Domain::Result<bool>::success(false);
        }
        constexpr std::uint64_t MaximumPeHeaderOffset = 1024U * 1024U;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
            static_cast<std::uint64_t>(dos.e_lfanew) > MaximumPeHeaderOffset) {
            return Domain::Result<bool>::success(false);
        }
        const auto ntOffset = static_cast<std::uint64_t>(dos.e_lfanew);
        const auto requiredBytes = ntOffset + sizeof(DWORD) +
            sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64);
        if (requiredBytes > static_cast<std::uint64_t>(fileSize.QuadPart)) {
            return Domain::Result<bool>::success(false);
        }
        DWORD signature{};
        read = readExecutableBytes(
            file.get(), ntOffset, &signature, static_cast<DWORD>(sizeof(signature)));
        if (!read || signature != IMAGE_NT_SIGNATURE) {
            return Domain::Result<bool>::success(false);
        }
        IMAGE_FILE_HEADER fileHeader{};
        read = readExecutableBytes(
            file.get(), ntOffset + sizeof(signature), &fileHeader,
            static_cast<DWORD>(sizeof(fileHeader)));
        if (!read || fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            fileHeader.NumberOfSections == 0U || fileHeader.NumberOfSections > 96U ||
            (fileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0U ||
            (fileHeader.Characteristics & IMAGE_FILE_DLL) != 0U ||
            fileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
            return Domain::Result<bool>::success(false);
        }
        IMAGE_OPTIONAL_HEADER64 optionalHeader{};
        read = readExecutableBytes(
            file.get(), ntOffset + sizeof(signature) + sizeof(fileHeader),
            &optionalHeader, static_cast<DWORD>(sizeof(optionalHeader)));
        if (!read || optionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            optionalHeader.AddressOfEntryPoint == 0U || optionalHeader.SizeOfImage == 0U ||
            (optionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI &&
             optionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI)) {
            return Domain::Result<bool>::success(false);
        }
        return Domain::Result<bool>::success(true);
    } catch (...) {
        return Domain::Result<bool>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The selected Forge executable failed PE validation."));
    }
}

[[nodiscard]] const char* roleText(const Domain::LMStudioConnectorRole role) noexcept
{
    return role == Domain::LMStudioConnectorRole::Primary ? "primary" : "fallback";
}

[[nodiscard]] const char* serverId(const Domain::LMStudioConnectorRole role) noexcept
{
    return role == Domain::LMStudioConnectorRole::Primary
        ? LMStudioPrimaryServerId
        : LMStudioFallbackServerId;
}

struct PluginLayout final {
    Domain::PathText lmStudioRoot;
    Domain::PathText pluginsRoot;
    Domain::PathText primaryPlugin;
    Domain::PathText fallbackPlugin;
};

[[nodiscard]] Domain::Result<PluginLayout> makeLayout(
    const Domain::PathText& configurationPath) noexcept
{
    try {
        auto root = parentPath(configurationPath);
        if (!root) {
            return Domain::Result<PluginLayout>::failure(std::move(root).error());
        }
        auto plugins = childPath(root.value(), "extensions\\plugins\\mcp");
        if (!plugins) {
            return Domain::Result<PluginLayout>::failure(std::move(plugins).error());
        }
        auto primary = childPath(plugins.value(), LMStudioPrimaryServerId);
        if (!primary) {
            return Domain::Result<PluginLayout>::failure(std::move(primary).error());
        }
        auto fallback = childPath(plugins.value(), LMStudioFallbackServerId);
        if (!fallback) {
            return Domain::Result<PluginLayout>::failure(std::move(fallback).error());
        }
        return Domain::Result<PluginLayout>::success(PluginLayout{
            std::move(root).value(),
            std::move(plugins).value(),
            std::move(primary).value(),
            std::move(fallback).value()});
    } catch (...) {
        return Domain::Result<PluginLayout>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio plugin layout could not be derived."));
    }
}

[[nodiscard]] const Domain::PathText& rolePath(
    const PluginLayout& layout,
    const Domain::LMStudioConnectorRole role) noexcept
{
    return role == Domain::LMStudioConnectorRole::Primary
        ? layout.primaryPlugin
        : layout.fallbackPlugin;
}

[[nodiscard]] Domain::Result<Json> parseJson(
    const std::vector<std::byte>& content,
    const std::string_view documentName) noexcept
{
    try {
        if (content.empty() || content.size() > MaximumPluginDocumentBytes) {
            return Domain::Result<Json>::failure(Domain::makeError(
                content.empty() ? Domain::ErrorCodes::MalformedMessage
                                : Domain::ErrorCodes::PayloadTooLarge,
                std::string{documentName} + " is empty or exceeds 64 KiB."));
        }
        auto value = Json::parse(textOf(content), nullptr, true, false);
        if (!value.is_object()) {
            return Domain::Result<Json>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                std::string{documentName} + " must be a JSON object."));
        }
        return Domain::Result<Json>::success(std::move(value));
    } catch (const Json::parse_error&) {
        return Domain::Result<Json>::failure(Domain::makeError(
            Domain::ErrorCodes::MalformedMessage,
            std::string{documentName} + " is malformed JSON."));
    } catch (...) {
        return Domain::Result<Json>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::string{documentName} + " could not be parsed."));
    }
}

[[nodiscard]] std::optional<std::string> jsonString(
    const Json& object,
    const std::string_view name)
{
    const auto member = object.find(name);
    if (member == object.end() || !member->is_string()) {
        return std::nullopt;
    }
    return member->get<std::string>();
}

[[nodiscard]] std::string jsonDocument(const Json& value)
{
    auto encoded = value.dump(2, ' ', false, Json::error_handler_t::strict);
    encoded.push_back('\n');
    return encoded;
}

[[nodiscard]] Json manifestFor(const Domain::LMStudioConnectorRole role)
{
    return Json{
        {"type", "plugin"},
        {"runner", "mcpBridge"},
        {"owner", "mcp"},
        {"name", serverId(role)}};
}

[[nodiscard]] Json bridgeFor(
    const Domain::LMStudioConnectorRole role,
    const Domain::PathText& binary,
    const Domain::PathText& forgeHome,
    const Domain::DeploymentId& deploymentId)
{
    return Json{
        {"command", binary.value()},
        {"args", Json::array({"serve"})},
        {"env", Json{
            {"FORGE_MCP_ROLE", roleText(role)},
            {"FORGE_CONDUCTOR_HOME", forgeHome.value()},
            {"FORGE_DEPLOYMENT_ID", deploymentId.value()}}}};
}

[[nodiscard]] Json installStateFor(
    const Domain::DeploymentId& deploymentId,
    const Domain::UtcTimePoint now)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return Json{
        {"by", InstallerId},
        {"at", milliseconds},
        {"deploymentID", deploymentId.value()}};
}

[[nodiscard]] bool stringEquals(
    const Json& object,
    const std::string_view name,
    const std::string_view expected)
{
    const auto value = jsonString(object, name);
    return value && *value == expected;
}

[[nodiscard]] bool validManifest(
    const Json& manifest,
    const Domain::LMStudioConnectorRole role)
{
    return stringEquals(manifest, "type", "plugin") &&
        stringEquals(manifest, "runner", "mcpBridge") &&
        stringEquals(manifest, "owner", "mcp") &&
        stringEquals(manifest, "name", serverId(role));
}

[[nodiscard]] bool validBridge(
    const Json& bridge,
    const Domain::LMStudioConnectorRole role,
    const Domain::PathText& binary,
    const Domain::PathText& forgeHome,
    const Domain::DeploymentId& deploymentId)
{
    if (!stringEquals(bridge, "command", binary.value())) {
        return false;
    }
    const auto args = bridge.find("args");
    const auto environment = bridge.find("env");
    if (args == bridge.end() || !args->is_array() || args->size() != 1U ||
        !(*args)[0].is_string() || (*args)[0].get<std::string>() != "serve" ||
        environment == bridge.end() || !environment->is_object()) {
        return false;
    }
    return stringEquals(*environment, "FORGE_MCP_ROLE", roleText(role)) &&
        stringEquals(*environment, "FORGE_CONDUCTOR_HOME", forgeHome.value()) &&
        stringEquals(*environment, "FORGE_DEPLOYMENT_ID", deploymentId.value());
}

[[nodiscard]] bool validInstallState(
    const Json& state,
    const Domain::DeploymentId& deploymentId)
{
    const auto at = state.find("at");
    return stringEquals(state, "by", InstallerId) &&
        stringEquals(state, "deploymentID", deploymentId.value()) &&
        at != state.end() && at->is_number_integer();
}

[[nodiscard]] bool exactAuthorization(
    const Contracts::WorkspaceAuthority& authority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::ToolEffect effect,
    const Domain::FileAccess intent,
    const Domain::OperationContext& context) noexcept
{
    return authority.intent() == intent &&
        authorization.toolName() == LMStudioDeploymentToolName &&
        authorization.matches(authority, context) &&
        authorization.matchesProject(authority.projectId()) &&
        authorization.effect() == effect;
}

[[nodiscard]] std::string healthDetail(
    const Domain::LMStudioConnectorHealth& primary,
    const Domain::LMStudioConnectorHealth& fallback)
{
    return std::string{"primary="} + (primary.ready ? "ready" : "failed") +
        " (" + primary.detail + "); fallback=" +
        (fallback.ready ? "ready" : "failed") + " (" + fallback.detail + ")";
}

[[nodiscard]] Domain::OperationContext rollbackContext(
    const Domain::OperationContext& caller,
    const Domain::MonotonicTimePoint now) noexcept
{
    return Domain::OperationContext{
        caller.operationId,
        now + std::chrono::seconds{30},
        {},
        caller.correlationId};
}

void appendUniqueRoot(
    std::vector<Domain::PathText>& roots,
    const Domain::PathText& root)
{
    if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
        roots.push_back(root);
    }
}

[[nodiscard]] std::uint64_t nextAuthorityGeneration(
    const Contracts::WorkspaceAuthority& authority)
{
    if (authority.generation() == (std::numeric_limits<std::uint64_t>::max)()) {
        throw DeploymentFailure{Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "LM Studio maintenance authority generation is exhausted.")};
    }
    return authority.generation() + 1U;
}

} // namespace

class WindowsLMStudioDeploymentService::Impl final {
public:
    Impl(
        Contracts::ILMStudioEnvironment& environmentValue,
        Contracts::ILMStudioServeVerifier& serveVerifierValue,
        Contracts::ILMStudioHostActivator& hostActivatorValue,
        Contracts::IWorkspaceAuthority& workspaceAuthorityValue,
        Contracts::IFileSystem& fileSystemValue,
        Contracts::IAtomicFileStore& atomicFileStoreValue,
        Contracts::IApplicationPaths& applicationPathsValue,
        Contracts::IClock& clockValue,
        Contracts::IUuidGenerator& uuidGeneratorValue,
        Contracts::IDiagnosticSink& diagnosticsValue)
        : environment{environmentValue},
          serveVerifier{serveVerifierValue},
          hostActivator{hostActivatorValue},
          workspaceAuthority{workspaceAuthorityValue},
          fileSystem{fileSystemValue},
          atomicFileStore{atomicFileStoreValue},
          applicationPaths{applicationPathsValue},
          clock{clockValue},
          uuidGenerator{uuidGeneratorValue},
          diagnostics{diagnosticsValue}
    {
    }

    [[nodiscard]] bool isCancelled(const Domain::OperationContext& context) noexcept
    {
        if (context.isCancellationRequested()) {
            return true;
        }
        std::scoped_lock lock{operationMutex};
        return closed ||
            (cancelledOperation && cancelledOperation.value() == context.operationId);
    }

    [[nodiscard]] bool beginOperation(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::scoped_lock lock{operationMutex};
            if (closed || active) {
                return false;
            }
            active = true;
            activeOperation = operationId;
            return true;
        } catch (...) {
            return false;
        }
    }

    void completeOperation() noexcept
    {
        try {
            {
                std::scoped_lock lock{operationMutex};
                active = false;
                activeOperation.reset();
            }
            operationChanged.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::Error admissionFailure(const std::string_view conflictMessage) noexcept
    {
        try {
            std::scoped_lock lock{operationMutex};
            return closed
                ? Domain::makeError(
                      Domain::ErrorCodes::Cancelled,
                      "LM Studio deployment service is shut down.")
                : Domain::makeError(
                      Domain::ErrorCodes::Conflict,
                      std::string{conflictMessage},
                      true);
        } catch (...) {
            return Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "LM Studio operation admission state could not be inspected.");
        }
    }

    void requireLive(const Domain::OperationContext& context, const std::string_view action)
    {
        if (isCancelled(context)) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                std::string{action} + " was cancelled.")};
        }
        if (context.isExpired(clock.monotonicNow())) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                std::string{action} + " deadline expired.")};
        }
    }

    [[nodiscard]] Contracts::AuthorizedPath authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& path,
        const Domain::PathText& base,
        const Domain::FileAccess access,
        const Domain::OperationContext& context)
    {
        return take(workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{path, base, access, false},
            context));
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> readAtomic(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& path,
        const Domain::PathText& base,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context)
    {
        auto authorized = workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Read, false},
            context);
        if (!authorized) {
            if (authorized.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            throw DeploymentFailure{std::move(authorized).error()};
        }
        auto content = atomicFileStore.read(
            authorized.value(), maximumBytes, context);
        if (!content) {
            if (content.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            throw DeploymentFailure{std::move(content).error()};
        }
        return std::move(content).value();
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> readFile(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& path,
        const Domain::PathText& base,
        const std::size_t maximumBytes,
        const Domain::OperationContext& context)
    {
        auto authorized = workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Read, false},
            context);
        if (!authorized) {
            if (authorized.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            throw DeploymentFailure{std::move(authorized).error()};
        }
        auto content = fileSystem.readFile(authorized.value(), maximumBytes, context);
        if (!content) {
            if (content.error().code == Domain::ErrorCodes::RecordNotFound) {
                return std::nullopt;
            }
            throw DeploymentFailure{std::move(content).error()};
        }
        return std::move(content).value();
    }

    void createDirectory(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& directory,
        const Domain::PathText& base,
        const Domain::OperationContext& context)
    {
        auto authorized = authorize(
            authority, directory, base, Domain::FileAccess::Create, context);
        take(fileSystem.createDirectory(authorized, context));
    }

    void writeNewFile(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& path,
        const Domain::PathText& base,
        const std::string& content,
        const Domain::OperationContext& context)
    {
        auto authorized = authorize(
            authority, path, base, Domain::FileAccess::Create, context);
        const auto bytes = bytesOf(content);
        take(fileSystem.writeFile(authorized, bytes, context));
    }

    [[nodiscard]] bool moveIfExists(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& source,
        const Domain::PathText& destination,
        const Domain::PathText& base,
        const Domain::OperationContext& context)
    {
        auto sourceAuthorization = workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                source, base, Domain::FileAccess::Delete, false},
            context);
        if (!sourceAuthorization) {
            if (sourceAuthorization.error().code == Domain::ErrorCodes::RecordNotFound) {
                return false;
            }
            throw DeploymentFailure{std::move(sourceAuthorization).error()};
        }
        auto destinationAuthorization = authorize(
            authority, destination, base, Domain::FileAccess::Create, context);
        auto moved = fileSystem.move(
            sourceAuthorization.value(), destinationAuthorization, context);
        if (!moved) {
            if (moved.error().code == Domain::ErrorCodes::RecordNotFound) {
                return false;
            }
            throw DeploymentFailure{std::move(moved).error()};
        }
        return true;
    }

    void moveRequired(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& source,
        const Domain::PathText& destination,
        const Domain::PathText& base,
        const Domain::OperationContext& context)
    {
        auto sourceAuthorization = authorize(
            authority, source, base, Domain::FileAccess::Delete, context);
        auto destinationAuthorization = authorize(
            authority, destination, base, Domain::FileAccess::Create, context);
        take(fileSystem.move(
            sourceAuthorization, destinationAuthorization, context));
    }

    void removeIfExists(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& path,
        const Domain::PathText& base,
        const bool recursive,
        const Domain::OperationContext& context)
    {
        auto authorization = workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                path, base, Domain::FileAccess::Delete, false},
            context);
        if (!authorization) {
            if (authorization.error().code == Domain::ErrorCodes::RecordNotFound) {
                return;
            }
            throw DeploymentFailure{std::move(authorization).error()};
        }
        auto removed = fileSystem.remove(authorization.value(), recursive, context);
        if (!removed && removed.error().code != Domain::ErrorCodes::RecordNotFound) {
            throw DeploymentFailure{std::move(removed).error()};
        }
    }

    [[nodiscard]] bool pluginInstalled(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& directory,
        const Domain::PathText& base,
        const Domain::LMStudioConnectorRole role,
        const Domain::PathText& binary,
        const Domain::PathText& forgeHome,
        const Domain::DeploymentId& deploymentId,
        const Domain::OperationContext& context)
    {
        auto manifestPath = take(childPath(directory, "manifest.json"));
        auto bridgePath = take(childPath(directory, "mcp-bridge-config.json"));
        auto statePath = take(childPath(directory, "install-state.json"));
        const auto manifestBytes = readFile(
            authority, manifestPath, base, MaximumPluginDocumentBytes, context);
        const auto bridgeBytes = readFile(
            authority, bridgePath, base, MaximumPluginDocumentBytes, context);
        const auto stateBytes = readFile(
            authority, statePath, base, MaximumPluginDocumentBytes, context);
        if (!manifestBytes || !bridgeBytes || !stateBytes) {
            return false;
        }
        auto manifest = parseJson(*manifestBytes, "LM Studio manifest.json");
        auto bridge = parseJson(*bridgeBytes, "LM Studio mcp-bridge-config.json");
        auto state = parseJson(*stateBytes, "LM Studio install-state.json");
        return manifest && bridge && state && validManifest(manifest.value(), role) &&
            validBridge(bridge.value(), role, binary, forgeHome, deploymentId) &&
            validInstallState(state.value(), deploymentId);
    }

    void stagePlugin(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathText& stageDirectory,
        const Domain::PathText& base,
        const Domain::LMStudioConnectorRole role,
        const Domain::PathText& binary,
        const Domain::PathText& forgeHome,
        const Domain::DeploymentId& deploymentId,
        const Domain::OperationContext& context)
    {
        requireLive(context, "LM Studio plugin staging");
        createDirectory(authority, stageDirectory, base, context);
        auto manifestPath = take(childPath(stageDirectory, "manifest.json"));
        auto bridgePath = take(childPath(stageDirectory, "mcp-bridge-config.json"));
        auto statePath = take(childPath(stageDirectory, "install-state.json"));
        writeNewFile(authority, manifestPath, base,
                     jsonDocument(manifestFor(role)), context);
        writeNewFile(authority, bridgePath, base,
                     jsonDocument(bridgeFor(role, binary, forgeHome, deploymentId)), context);
        writeNewFile(authority, statePath, base,
                     jsonDocument(installStateFor(deploymentId, clock.utcNow())), context);
        if (!pluginInstalled(authority, stageDirectory, base, role,
                             binary, forgeHome, deploymentId, context)) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{"Staged LM Studio connector failed validation: "} + serverId(role) + ".")};
        }
    }

    void record(
        const std::string_view event,
        const Domain::DiagnosticSeverity severity,
        const Domain::OperationContext& context,
        std::vector<Domain::DiagnosticField> fields = {}) noexcept
    {
        try {
            static_cast<void>(diagnostics.record(
                Domain::DiagnosticEnvelope{
                    clock.utcNow(),
                    std::string{event},
                    severity,
                    "manager",
                    ::GetCurrentProcessId(),
                    Domain::DiagnosticCategory::LmStudio,
                    std::move(fields)},
                context));
        } catch (...) {
        }
    }

    Contracts::ILMStudioEnvironment& environment;
    Contracts::ILMStudioServeVerifier& serveVerifier;
    Contracts::ILMStudioHostActivator& hostActivator;
    Contracts::IWorkspaceAuthority& workspaceAuthority;
    Contracts::IFileSystem& fileSystem;
    Contracts::IAtomicFileStore& atomicFileStore;
    Contracts::IApplicationPaths& applicationPaths;
    Contracts::IClock& clock;
    Contracts::IUuidGenerator& uuidGenerator;
    Contracts::IDiagnosticSink& diagnostics;
    std::mutex operationMutex;
    std::condition_variable operationChanged;
    bool closed{};
    bool active{};
    std::optional<Domain::OperationId> activeOperation;
    std::optional<Domain::OperationId> cancelledOperation;
};

namespace {

template <typename Implementation>
class ActiveOperation final {
public:
    ActiveOperation(Implementation& implementation, const Domain::OperationId& operationId)
        : implementation_{implementation},
          acquired_{implementation_.beginOperation(operationId)}
    {}

    ~ActiveOperation() noexcept
    {
        if (acquired_) {
            implementation_.completeOperation();
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    Implementation& implementation_;
    bool acquired_{};
};

struct ConfigurationSnapshot final {
    LMStudioConfigurationDocument document;
    std::optional<std::vector<std::byte>> original;
};

template <typename Implementation>
[[nodiscard]] ConfigurationSnapshot readConfiguration(
    Implementation& implementation,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& configurationPath,
    const PluginLayout& layout,
    const Domain::OperationContext& context)
{
    auto original = implementation.readAtomic(
        authority,
        configurationPath,
        layout.lmStudioRoot,
        LMStudioConfigurationCodec::MaximumDocumentBytes,
        context);
    if (!original) {
        return ConfigurationSnapshot{LMStudioConfigurationCodec::empty(), std::nullopt};
    }
    auto parsed = LMStudioConfigurationCodec::parse(*original);
    if (!parsed) {
        throw DeploymentFailure{std::move(parsed).error()};
    }
    return ConfigurationSnapshot{
        std::move(parsed).value(),
        std::move(original)};
}

[[nodiscard]] Domain::PathText selectedBinary(
    const Domain::LMStudioDeploymentRequest& request)
{
    if (request.preferredBinary) {
        return request.preferredBinary.value();
    }
    return take(currentModulePath());
}

template <typename Implementation>
[[nodiscard]] Domain::LMStudioEnvironmentStatus inspectEnvironment(
    Implementation& implementation,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context)
{
    return take(implementation.environment.inspect(std::nullopt, authority, context));
}

struct ValidatedDeploymentBinary final {
    Domain::PathText path;
    Domain::PathText authorityRoot;
};

template <typename Implementation>
[[nodiscard]] ValidatedDeploymentBinary validatedDeploymentBinary(
    Implementation& implementation,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& requestedBinary,
    const Domain::OperationContext& context)
{
    auto readAuthorization = take(implementation.workspaceAuthority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            requestedBinary, std::nullopt, Domain::FileAccess::Read, false},
        context));
    auto executable = take(isExecutableWindowsBinary(
        readAuthorization.canonicalPath()));
    if (!executable) {
        throw DeploymentFailure{Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "LM Studio deployment requires a regular x64 PE executable image with a GUI or console subsystem.")};
    }
    auto executeAuthorization = take(implementation.workspaceAuthority.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            readAuthorization.canonicalPath(), std::nullopt,
            Domain::FileAccess::Execute, false},
        context));
    return ValidatedDeploymentBinary{
        executeAuthorization.canonicalPath(),
        executeAuthorization.authorityRoot()};
}

template <typename Implementation>
[[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> statusInternal(
    Implementation& implementation,
    const Domain::LMStudioDeploymentRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    try {
        implementation.requireLive(context, "LM Studio deployment status");
        const auto environment = inspectEnvironment(implementation, authority, context);
        if (!environment.configurationPath) {
            return Domain::Result<Domain::LMStudioPluginStatus>::failure(Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "LM Studio configuration path discovery produced no selected path."));
        }
        auto binary = selectedBinary(request);
        const auto forgeHome = take(implementation.applicationPaths.dataRoot(context));
        const auto layout = take(makeLayout(environment.configurationPath.value()));

        bool binaryExecutable{};
        auto binaryAuthorization = implementation.workspaceAuthority.authorize(
            authority,
            Domain::PathAuthorizationRequest{
                binary, std::nullopt, Domain::FileAccess::Read, false},
            context);
        if (!binaryAuthorization) {
            auto error = std::move(binaryAuthorization).error();
            if (error.code != Domain::ErrorCodes::RecordNotFound &&
                error.code != Domain::ErrorCodes::Unauthorized &&
                error.code != Domain::ErrorCodes::PathOutsideAuthority) {
                throw DeploymentFailure{std::move(error)};
            }
        } else {
            binary = binaryAuthorization.value().canonicalPath();
            auto executable = isExecutableWindowsBinary(
                binary);
            if (!executable) {
                throw DeploymentFailure{std::move(executable).error()};
            }
            binaryExecutable = executable.value();
        }

        auto snapshot = implementation.readAtomic(
            authority,
            environment.configurationPath.value(),
            layout.lmStudioRoot,
            LMStudioConfigurationCodec::MaximumDocumentBytes,
            context);
        if (!snapshot) {
            return Domain::Result<Domain::LMStudioPluginStatus>::success(
                Domain::LMStudioPluginStatus{
                    false,
                    false,
                    false,
                    binary,
                    binaryExecutable,
                    environment.lmStudioPresent,
                    layout.primaryPlugin,
                    layout.fallbackPlugin,
                    environment.configurationPath.value(),
                    std::nullopt,
                    "LM Studio mcp.json is missing."});
        }
        auto document = LMStudioConfigurationCodec::parse(*snapshot);
        if (!document) {
            return Domain::Result<Domain::LMStudioPluginStatus>::success(
                Domain::LMStudioPluginStatus{
                    false,
                    false,
                    false,
                    binary,
                    binaryExecutable,
                    environment.lmStudioPresent,
                    layout.primaryPlugin,
                    layout.fallbackPlugin,
                    environment.configurationPath.value(),
                    std::nullopt,
                    "LM Studio mcp.json is malformed or has an invalid root; existing bytes were preserved."});
        }
        auto inspection = LMStudioConfigurationCodec::inspect(
            document.value(), binary, forgeHome);
        if (!inspection) {
            return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                std::move(inspection).error());
        }

        bool primaryInstalled{};
        bool fallbackInstalled{};
        if (inspection.value().deploymentId) {
            primaryInstalled = implementation.pluginInstalled(
                authority, layout.primaryPlugin, layout.lmStudioRoot,
                Domain::LMStudioConnectorRole::Primary, binary, forgeHome,
                inspection.value().deploymentId.value(), context);
            fallbackInstalled = implementation.pluginInstalled(
                authority, layout.fallbackPlugin, layout.lmStudioRoot,
                Domain::LMStudioConnectorRole::Fallback, binary, forgeHome,
                inspection.value().deploymentId.value(), context);
        }
        std::string detail;
        if (!environment.lmStudioPresent) {
            detail = "LM Studio application is not installed; ";
        }
        if (!binaryExecutable) {
            detail += "selected Forge binary is not an authorized native executable; ";
        }
        if (!primaryInstalled) {
            detail += "primary plugin missing or stale; ";
        }
        if (!fallbackInstalled) {
            detail += "fallback plugin missing or stale; ";
        }
        if (!inspection.value().registered) {
            detail += inspection.value().detail;
        }
        if (detail.empty()) {
            detail = "LM Studio primary and fallback plugins match one shared current revision.";
        }
        return Domain::Result<Domain::LMStudioPluginStatus>::success(
            Domain::LMStudioPluginStatus{
                primaryInstalled,
                fallbackInstalled,
                inspection.value().registered,
                binary,
                binaryExecutable,
                environment.lmStudioPresent,
                layout.primaryPlugin,
                layout.fallbackPlugin,
                environment.configurationPath.value(),
                inspection.value().deploymentId,
                std::move(detail)});
    } catch (DeploymentFailure& failure) {
        return Domain::Result<Domain::LMStudioPluginStatus>::failure(
            failure.releaseError());
    } catch (...) {
        return Domain::Result<Domain::LMStudioPluginStatus>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio deployment status failed at its exception boundary."));
    }
}

enum class BackupMutationState {
    NotStarted,
    Attempted,
    Absent,
    Present
};

struct RoleMutationJournal final {
    Domain::LMStudioConnectorRole role{Domain::LMStudioConnectorRole::Primary};
    BackupMutationState backup{BackupMutationState::NotStarted};
    bool targetMoveAttempted{};
};

struct DeploymentMutationJournal final {
    std::array<RoleMutationJournal, 2U> roles{
        RoleMutationJournal{Domain::LMStudioConnectorRole::Fallback},
        RoleMutationJournal{Domain::LMStudioConnectorRole::Primary}};
    bool transactionTreeMutationAttempted{};
    bool configurationMutationAttempted{};
    bool commitValidated{};
};

[[nodiscard]] bool strongLegacyWrapperSignature(
    const std::vector<std::byte>& content)
{
    if (content.empty() || content.size() > MaximumPluginDocumentBytes) {
        return false;
    }
    auto text = textOf(content);
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    const bool pythonLauncher = text.find("python") != std::string::npos;
    const bool forgeModule =
        text.find("-m forge_conductor.") != std::string::npos;
    const bool mcpModule = text.find("mcp") != std::string::npos;
    return pythonLauncher && forgeModule && mcpModule;
}

template <typename Implementation>
void cleanupExactLegacyLaunchers(
    Implementation& implementation,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& binary,
    const Domain::OperationContext& context)
{
    const auto binaryRoot = take(parentPath(binary));
    for (const auto* const leaf : {
             "forge-serve",
             "forge-serve-fallback",
             "forge-serve.cmd",
             "forge-serve-fallback.cmd"}) {
        const auto launcher = take(childPath(binaryRoot, leaf));
        const auto content = implementation.readFile(
            authority, launcher, binaryRoot, MaximumPluginDocumentBytes, context);
        if (content && strongLegacyWrapperSignature(content.value())) {
            implementation.removeIfExists(
                authority, launcher, binaryRoot, false, context);
        }
    }
}

template <typename Implementation>
[[nodiscard]] std::optional<Domain::Error> rollback(
    Implementation& implementation,
    const Contracts::WorkspaceAuthority& authority,
    const PluginLayout& layout,
    const Domain::PathText& transactionRoot,
    const Domain::PathText& backupRoot,
    const DeploymentMutationJournal& journal,
    const Domain::PathText& configurationPath,
    const std::optional<std::vector<std::byte>>& originalConfiguration,
    const Domain::OperationContext& context) noexcept
{
    std::optional<Domain::Error> rollbackError;
    const auto remember = [&](Domain::Error error) {
        if (!rollbackError) {
            rollbackError.emplace(std::move(error));
        }
    };
    for (auto role = journal.roles.rbegin(); role != journal.roles.rend(); ++role) {
        try {
            const auto target = rolePath(layout, role->role);
            const auto backup = take(childPath(backupRoot, serverId(role->role)));
            if (role->backup == BackupMutationState::Attempted) {
                static_cast<void>(implementation.moveIfExists(
                    authority, backup, target, layout.lmStudioRoot, context));
            } else if (role->backup == BackupMutationState::Present) {
                implementation.removeIfExists(
                    authority, target, layout.lmStudioRoot, true, context);
                implementation.moveRequired(
                    authority, backup, target, layout.lmStudioRoot, context);
            } else if (role->backup == BackupMutationState::Absent &&
                       role->targetMoveAttempted) {
                implementation.removeIfExists(
                    authority, target, layout.lmStudioRoot, true, context);
            } else if (role->backup == BackupMutationState::NotStarted &&
                       role->targetMoveAttempted) {
                remember(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "LM Studio rollback journal contains an impossible role state."));
            }
        } catch (DeploymentFailure& failure) {
            remember(failure.releaseError());
        } catch (...) {
            remember(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio plugin rollback failed at its exception boundary."));
        }
    }
    if (journal.configurationMutationAttempted) {
        try {
            if (originalConfiguration) {
                auto write = implementation.authorize(
                    authority, configurationPath, layout.lmStudioRoot,
                    Domain::FileAccess::Write, context);
                take(implementation.atomicFileStore.replace(
                    write, *originalConfiguration, false, context));
            } else {
                implementation.removeIfExists(
                    authority, configurationPath, layout.lmStudioRoot, false, context);
            }
        } catch (DeploymentFailure& failure) {
            remember(failure.releaseError());
        } catch (...) {
            remember(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio configuration rollback failed at its exception boundary."));
        }
    }
    if (journal.transactionTreeMutationAttempted) {
        try {
            implementation.removeIfExists(
                authority, transactionRoot, layout.lmStudioRoot, true, context);
        } catch (DeploymentFailure& failure) {
            remember(failure.releaseError());
        } catch (...) {
            remember(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio transaction-tree rollback cleanup failed at its exception boundary."));
        }
    }
    return rollbackError;
}

} // namespace

WindowsLMStudioDeploymentService::WindowsLMStudioDeploymentService(
    Contracts::ILMStudioEnvironment& environment,
    Contracts::ILMStudioServeVerifier& serveVerifier,
    Contracts::ILMStudioHostActivator& hostActivator,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IFileSystem& fileSystem,
    Contracts::IAtomicFileStore& atomicFileStore,
    Contracts::IApplicationPaths& applicationPaths,
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator,
    Contracts::IDiagnosticSink& diagnostics)
    : implementation_{std::make_shared<Impl>(
          environment,
          serveVerifier,
          hostActivator,
          workspaceAuthority,
          fileSystem,
          atomicFileStore,
          applicationPaths,
          clock,
          uuidGenerator,
          diagnostics)}
{
}

WindowsLMStudioDeploymentService::~WindowsLMStudioDeploymentService() noexcept
{
    shutdown();
}

Domain::Result<Domain::LMStudioPluginStatus> WindowsLMStudioDeploymentService::status(
    const Domain::LMStudioDeploymentRequest& request,
    const Contracts::WorkspaceAuthority& readAuthority,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    ActiveOperation operation{*implementation, context.operationId};
    if (!operation.acquired()) {
        return Domain::Result<Domain::LMStudioPluginStatus>::failure(
            implementation->admissionFailure(
                "Another LM Studio deployment operation is already in progress."));
    }
    return statusInternal(*implementation, request, readAuthority, context);
}

Domain::Result<Domain::LMStudioInstallResult> WindowsLMStudioDeploymentService::deploy(
    const Domain::LMStudioDeploymentRequest& request,
    const Contracts::WorkspaceAuthority& writeAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    ActiveOperation operation{*implementation, context.operationId};
    if (!operation.acquired()) {
        return Domain::Result<Domain::LMStudioInstallResult>::failure(
            implementation->admissionFailure(
                "Another LM Studio deployment operation is already in progress."));
    }
    if (!exactAuthorization(
            writeAuthority, authorization, Domain::ToolEffect::Write,
            Domain::FileAccess::Write, context)) {
        return Domain::Result<Domain::LMStudioInstallResult>::failure(Domain::makeError(
            Domain::ErrorCodes::Unauthorized,
            "LM Studio deployment capability is not bound to this project, caller, authority generation, correlation, and Write effect."));
    }
    if (!request.preserveForeignEntries) {
        return Domain::Result<Domain::LMStudioInstallResult>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "LM Studio deployment cannot disable preservation of foreign servers and unknown fields."));
    }

    std::optional<PluginLayout> layout;
    std::optional<Domain::PathText> transactionRoot;
    std::optional<Domain::PathText> backupRoot;
    std::optional<Domain::PathText> configurationPath;
    std::optional<std::vector<std::byte>> originalConfiguration;
    std::optional<Contracts::WorkspaceAuthority> maintenanceAuthority;
    DeploymentMutationJournal journal;

    try {
        implementation->requireLive(context, "LM Studio deployment");
        implementation->record("lmstudio_deploy_begin", Domain::DiagnosticSeverity::Info, context);
        const auto environment = inspectEnvironment(*implementation, writeAuthority, context);
        if (!environment.configurationPath) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "LM Studio configuration discovery produced no selected path.")};
        }
        configurationPath = environment.configurationPath;
        layout = take(makeLayout(configurationPath.value()));
        const auto validatedBinary = validatedDeploymentBinary(
            *implementation, writeAuthority, selectedBinary(request), context);
        const auto& binary = validatedBinary.path;
        const auto forgeHome = take(implementation->applicationPaths.dataRoot(context));

        std::vector<Domain::PathText> maintenanceRoots;
        maintenanceRoots.reserve(4U);
        appendUniqueRoot(maintenanceRoots, validatedBinary.authorityRoot);
        const auto configurationRead = implementation->authorize(
            writeAuthority, configurationPath.value(), layout->lmStudioRoot,
            Domain::FileAccess::Read, context);
        appendUniqueRoot(maintenanceRoots, configurationRead.authorityRoot());
        const auto forgeHomeRead = take(implementation->workspaceAuthority.authorize(
            writeAuthority,
            Domain::PathAuthorizationRequest{
                forgeHome, std::nullopt, Domain::FileAccess::Read, false},
            context));
        appendUniqueRoot(maintenanceRoots, forgeHomeRead.authorityRoot());
        if (environment.applicationExecutable) {
            const auto applicationRead = take(
                implementation->workspaceAuthority.authorize(
                    writeAuthority,
                    Domain::PathAuthorizationRequest{
                        environment.applicationExecutable.value(), std::nullopt,
                        Domain::FileAccess::Read, false},
                    context));
            appendUniqueRoot(maintenanceRoots, applicationRead.authorityRoot());
        }
        maintenanceAuthority.emplace(take(implementation->workspaceAuthority.narrow(
            writeAuthority,
            maintenanceRoots,
            {Domain::FileAccess::Read, Domain::FileAccess::Write,
             Domain::FileAccess::Create, Domain::FileAccess::Delete,
             Domain::FileAccess::Execute},
            true,
            nextAuthorityGeneration(writeAuthority),
            context)));
        const auto& maintenance = maintenanceAuthority.value();

        const auto prePrimary = take(implementation->serveVerifier.verify(
            binary, forgeHome, Domain::LMStudioConnectorRole::Primary,
            std::nullopt, maintenance, context));
        const auto preFallback = take(implementation->serveVerifier.verify(
            binary, forgeHome, Domain::LMStudioConnectorRole::Fallback,
            std::nullopt, maintenance, context));
        if (!prePrimary.ready || prePrimary.role != Domain::LMStudioConnectorRole::Primary ||
            !preFallback.ready || preFallback.role != Domain::LMStudioConnectorRole::Fallback) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio pre-deployment serve verification failed: " +
                    healthDetail(prePrimary, preFallback))};
        }

        auto generated = take(implementation->uuidGenerator.next());
        auto deploymentId = take(Domain::DeploymentId::parse(generated.value()));
        auto snapshot = readConfiguration(
            *implementation, maintenance, configurationPath.value(),
            layout.value(), context);
        originalConfiguration = snapshot.original;
        auto mergedConfiguration = take(LMStudioConfigurationCodec::mergeForgeServers(
            snapshot.document, binary, forgeHome, deploymentId));

        auto transactionName = std::string{".forge-conductor-install-"} + deploymentId.value();
        transactionRoot = take(childPath(layout->pluginsRoot, transactionName));
        auto stagedRoot = take(childPath(transactionRoot.value(), "staged"));
        backupRoot = take(childPath(transactionRoot.value(), "backup"));
        journal.transactionTreeMutationAttempted = true;
        implementation->createDirectory(
            maintenance, stagedRoot, layout->lmStudioRoot, context);
        implementation->createDirectory(
            maintenance, backupRoot.value(), layout->lmStudioRoot, context);

        auto stagedPrimary = take(childPath(stagedRoot, LMStudioPrimaryServerId));
        auto stagedFallback = take(childPath(stagedRoot, LMStudioFallbackServerId));
        implementation->stagePlugin(
            maintenance, stagedPrimary, layout->lmStudioRoot,
            Domain::LMStudioConnectorRole::Primary,
            binary, forgeHome, deploymentId, context);
        implementation->stagePlugin(
            maintenance, stagedFallback, layout->lmStudioRoot,
            Domain::LMStudioConnectorRole::Fallback,
            binary, forgeHome, deploymentId, context);

        for (auto& roleJournal : journal.roles) {
            implementation->requireLive(context, "LM Studio plugin commit");
            const auto role = roleJournal.role;
            const auto target = rolePath(layout.value(), role);
            const auto backup = take(childPath(backupRoot.value(), serverId(role)));
            const auto staged = take(childPath(stagedRoot, serverId(role)));
            roleJournal.backup = BackupMutationState::Attempted;
            const bool hadPrevious = implementation->moveIfExists(
                maintenance, target, backup, layout->lmStudioRoot, context);
            roleJournal.backup = hadPrevious
                ? BackupMutationState::Present
                : BackupMutationState::Absent;
            roleJournal.targetMoveAttempted = true;
            implementation->moveRequired(
                maintenance, staged, target, layout->lmStudioRoot, context);
        }

        Contracts::AuthorizedPath configAuthorization = originalConfiguration
            ? implementation->authorize(
                  maintenance, configurationPath.value(), layout->lmStudioRoot,
                  Domain::FileAccess::Write, context)
            : implementation->authorize(
                  maintenance, configurationPath.value(), layout->lmStudioRoot,
                  Domain::FileAccess::Create, context);
        journal.configurationMutationAttempted = true;
        take(implementation->atomicFileStore.replace(
            configAuthorization, mergedConfiguration, false, context));

        const auto committedStatus = take(statusInternal(
            *implementation, request, maintenance, context));
        if (!committedStatus.primaryPluginInstalled ||
            !committedStatus.fallbackPluginInstalled ||
            !committedStatus.mcpConfigurationRegistered ||
            !committedStatus.deploymentId ||
            committedStatus.deploymentId.value() != deploymentId) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio post-commit drift validation failed: " + committedStatus.detail)};
        }

        const auto postPrimary = take(implementation->serveVerifier.verify(
            binary, forgeHome, Domain::LMStudioConnectorRole::Primary,
            deploymentId, maintenance, context));
        const auto postFallback = take(implementation->serveVerifier.verify(
            binary, forgeHome, Domain::LMStudioConnectorRole::Fallback,
            deploymentId, maintenance, context));
        if (!postPrimary.ready || postPrimary.role != Domain::LMStudioConnectorRole::Primary ||
            !postFallback.ready || postFallback.role != Domain::LMStudioConnectorRole::Fallback) {
            throw DeploymentFailure{Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio post-deployment serve verification failed: " +
                    healthDetail(postPrimary, postFallback))};
        }

        journal.commitValidated = true;
        implementation->removeIfExists(
            maintenance, transactionRoot.value(), layout->lmStudioRoot, true, context);
        cleanupExactLegacyLaunchers(
            *implementation, maintenance, binary, context);
        implementation->record(
            "lmstudio_deploy_complete", Domain::DiagnosticSeverity::Info, context,
            {{"deployment_id", deploymentId.value()}});
        return Domain::Result<Domain::LMStudioInstallResult>::success(
            Domain::LMStudioInstallResult{
                true,
                binary,
                {layout->primaryPlugin, layout->fallbackPlugin},
                configurationPath.value(),
                deploymentId,
                "Committed one fresh LM Studio primary/fallback revision after pre-smoke, transactional install, post-validation, and post-smoke."});
    } catch (DeploymentFailure& failure) {
        auto error = failure.releaseError();
        if (journal.commitValidated) {
            error = Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio deployment committed successfully, but ownership-scoped cleanup failed: " +
                    error.message,
                error.retryable);
            implementation->record(
                "lmstudio_deploy_cleanup_failed",
                Domain::DiagnosticSeverity::Error,
                context,
                {{"error_code", error.code}});
        } else if (layout && transactionRoot && backupRoot && configurationPath) {
            const auto cleanupContext = rollbackContext(
                context, implementation->clock.monotonicNow());
            auto rollbackError = rollback(
                *implementation,
                maintenanceAuthority ? maintenanceAuthority.value() : writeAuthority,
                layout.value(),
                transactionRoot.value(), backupRoot.value(), journal,
                configurationPath.value(), originalConfiguration,
                cleanupContext);
            if (rollbackError) {
                error = Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    error.message + " Rollback also failed: " + rollbackError->message);
                implementation->record(
                    "lmstudio_deploy_rollback_failed",
                    Domain::DiagnosticSeverity::Error,
                    context,
                    {{"error_code", rollbackError->code}});
            }
        }
        implementation->record(
            "lmstudio_deploy_failed", Domain::DiagnosticSeverity::Error, context,
            {{"error_code", error.code}});
        return Domain::Result<Domain::LMStudioInstallResult>::failure(std::move(error));
    } catch (...) {
        auto error = Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio deployment failed at its exception boundary.");
        if (journal.commitValidated) {
            error = Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "LM Studio deployment committed successfully, but cleanup failed at its exception boundary.");
            implementation->record(
                "lmstudio_deploy_cleanup_failed",
                Domain::DiagnosticSeverity::Error,
                context,
                {{"error_code", error.code}});
        } else if (layout && transactionRoot && backupRoot && configurationPath) {
            const auto cleanupContext = rollbackContext(
                context, implementation->clock.monotonicNow());
            auto rollbackError = rollback(
                *implementation,
                maintenanceAuthority ? maintenanceAuthority.value() : writeAuthority,
                layout.value(),
                transactionRoot.value(), backupRoot.value(), journal,
                configurationPath.value(), originalConfiguration,
                cleanupContext);
            if (rollbackError) {
                error = Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    error.message + " Rollback also failed: " + rollbackError->message);
                implementation->record(
                    "lmstudio_deploy_rollback_failed",
                    Domain::DiagnosticSeverity::Error,
                    context,
                    {{"error_code", rollbackError->code}});
            }
        }
        return Domain::Result<Domain::LMStudioInstallResult>::failure(std::move(error));
    }
}

Domain::Result<Domain::LMStudioHostActivationResult>
WindowsLMStudioDeploymentService::activate(
    const Domain::LMStudioHostActivationRequest& request,
    const Contracts::WorkspaceAuthority& executionAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    const auto implementation = implementation_;
    ActiveOperation operation{*implementation, context.operationId};
    if (!operation.acquired()) {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
            implementation->admissionFailure(
                "Another LM Studio deployment operation is already in progress."));
    }
    if (!exactAuthorization(
            executionAuthority, authorization, Domain::ToolEffect::Execute,
            Domain::FileAccess::Execute, context)) {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "LM Studio activation capability is not bound to this project, caller, authority generation, correlation, and Execute effect."));
    }
    try {
        implementation->requireLive(context, "LM Studio activation");
        const auto environment = inspectEnvironment(
            *implementation, executionAuthority, context);
        if (!environment.configurationPath || !environment.applicationExecutable) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "LM Studio activation discovery did not select an application and configuration path."));
        }
        const auto executable = take(implementation->workspaceAuthority.authorize(
            executionAuthority,
            Domain::PathAuthorizationRequest{
                environment.applicationExecutable.value(), std::nullopt,
                Domain::FileAccess::Execute, false},
            context));
        const auto configuration = take(implementation->workspaceAuthority.authorize(
            executionAuthority,
            Domain::PathAuthorizationRequest{
                environment.configurationPath.value(), std::nullopt,
                Domain::FileAccess::Read, false},
            context));
        std::vector<Domain::PathText> maintenanceRoots;
        maintenanceRoots.reserve(2U);
        appendUniqueRoot(maintenanceRoots, executable.authorityRoot());
        appendUniqueRoot(maintenanceRoots, configuration.authorityRoot());
        const auto maintenance = take(implementation->workspaceAuthority.narrow(
            executionAuthority,
            maintenanceRoots,
            {Domain::FileAccess::Read, Domain::FileAccess::Execute},
            false,
            nextAuthorityGeneration(executionAuthority),
            context));
        auto result = implementation->hostActivator.activate(
            environment, request, maintenance, context);
        if (result) {
            implementation->record(
                "lmstudio_host_synchronized", Domain::DiagnosticSeverity::Info, context,
                {{"deployment_id", request.deploymentId.value()},
                 {"ready_roles", std::to_string(result.value().readyRoles.size())}});
        }
        return result;
    } catch (DeploymentFailure& failure) {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
            failure.releaseError());
    } catch (...) {
        return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "LM Studio activation failed at its exception boundary."));
    }
}

void WindowsLMStudioDeploymentService::cancel(
    const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    try {
        {
            std::scoped_lock lock{implementation->operationMutex};
            implementation->cancelledOperation = operationId;
        }
        implementation->serveVerifier.cancel(operationId);
        implementation->hostActivator.cancel(operationId);
    } catch (...) {
    }
}

void WindowsLMStudioDeploymentService::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (!implementation) {
        return;
    }
    std::optional<Domain::OperationId> activeOperation;
    {
        std::scoped_lock lock{implementation->operationMutex};
        implementation->closed = true;
        activeOperation = implementation->activeOperation;
    }
    if (activeOperation) {
        implementation->serveVerifier.cancel(activeOperation.value());
        implementation->hostActivator.cancel(activeOperation.value());
    }
    implementation->serveVerifier.shutdown();
    implementation->hostActivator.shutdown();
    std::unique_lock lock{implementation->operationMutex};
    static_cast<void>(implementation->operationChanged.wait_for(
        lock,
        std::chrono::seconds{30},
        [&] { return !implementation->active; }));
}

} // namespace ForgeConductor::Infrastructure::Windows
