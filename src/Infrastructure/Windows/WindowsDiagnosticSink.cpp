#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h"

#include "Detail/BoundedSerialExecutor.h"
#include "Detail/DiagnosticDirectoryTree.h"
#include "Detail/DiagnosticRotationPublishObserver.h"
#include "Detail/EtwProvider.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/RelativeFileOperations.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UtfConversion.h"
#include "Detail/Win32Error.h"
#include "Detail/WindowsPathResolver.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows
{

namespace Detail
{

[[nodiscard]] Domain::Result<void> validateDiagnosticDirectoryCaseSensitivity(const DWORD flags) noexcept
{
    if ((flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0U)
    {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                                               "Case-sensitive directories are not supported for "
                                                               "diagnostic storage."));
    }
    return Domain::Result<void>::success();
}

} // namespace Detail

namespace
{

using Json = nlohmann::json;
constexpr auto DiagnosticShutdownDrainTimeout = std::chrono::seconds{2};
constexpr std::size_t DiagnosticRotationTemporarySlots = 16U;
constexpr std::wstring_view DiagnosticRotationTemporaryPrefix = L".forge-diagnostics-rotation-";
constexpr std::wstring_view DiagnosticRotationTemporarySuffix = L".tmp";
constexpr ULONG NativeFileRenameInformationEx = 65U;

struct NativeIoStatusBlock final
{
    union {
        LONG status;
        void *pointer;
    } result{};
    ULONG_PTR information{};
};

using NtSetInformationFileFunction = LONG(NTAPI *)(HANDLE, NativeIoStatusBlock *, void *, ULONG, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI *)(LONG);

struct RetainedDiagnostic final
{
    Domain::DiagnosticEnvelope envelope;
    std::size_t encodedBytes{};
};

[[nodiscard]] std::string severityName(const Domain::DiagnosticSeverity severity)
{
    switch (severity)
    {
    case Domain::DiagnosticSeverity::Info:
        return "info";
    case Domain::DiagnosticSeverity::Warn:
        return "warn";
    case Domain::DiagnosticSeverity::Error:
        return "error";
    case Domain::DiagnosticSeverity::Critical:
        return "critical";
    }
    throw std::invalid_argument{"Unsupported diagnostic severity."};
}

[[nodiscard]] std::string categoryName(const Domain::DiagnosticCategory category)
{
    switch (category)
    {
    case Domain::DiagnosticCategory::General:
        return "general";
    case Domain::DiagnosticCategory::Bootstrap:
        return "bootstrap";
    case Domain::DiagnosticCategory::Telemetry:
        return "telemetry";
    case Domain::DiagnosticCategory::Mcp:
        return "mcp";
    case Domain::DiagnosticCategory::LmStudio:
        return "lmstudio";
    case Domain::DiagnosticCategory::Manager:
        return "manager";
    case Domain::DiagnosticCategory::Tools:
        return "tools";
    case Domain::DiagnosticCategory::Agent:
        return "agent";
    case Domain::DiagnosticCategory::Diagnostics:
        return "diagnostics";
    case Domain::DiagnosticCategory::Ui:
        return "ui";
    }
    throw std::invalid_argument{"Unsupported diagnostic category."};
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    if (::gmtime_s(&utc, &seconds) != 0)
    {
        throw std::runtime_error{"Diagnostic timestamp is outside the UTC range."};
    }
    auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count() % 1'000;
    if (milliseconds < 0)
    {
        milliseconds += 1'000;
    }
    char buffer[32]{};
    if (::sprintf_s(buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                    utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<long long>(milliseconds)) <= 0)
    {
        throw std::runtime_error{"Diagnostic timestamp could not be encoded."};
    }
    return buffer;
}

[[nodiscard]] char asciiLower(const char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] std::string normalizedFieldName(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    bool delimiter{};
    for (const char raw : value)
    {
        const char character = asciiLower(raw);
        const bool alphaNumeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (alphaNumeric)
        {
            if (delimiter && !result.empty())
            {
                result.push_back('_');
            }
            result.push_back(character);
            delimiter = false;
        }
        else
        {
            delimiter = true;
        }
    }
    return result;
}

[[nodiscard]] bool containsNameToken(const std::string_view normalized, const std::string_view token) noexcept
{
    std::size_t start = 0U;
    while (start < normalized.size())
    {
        const std::size_t end = normalized.find('_', start);
        const std::size_t length = end == std::string_view::npos ? normalized.size() - start : end - start;
        if (normalized.substr(start, length) == token)
        {
            return true;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1U;
    }
    return false;
}

[[nodiscard]] bool isPrivateFieldName(const std::string_view rawName)
{
    const std::string name = normalizedFieldName(rawName);
    static const std::set<std::string, std::less<>> exact{
        "access_key",  "access_key_id", "api_key", "apikey",  "api_secret", "auth",           "authorization", "bearer",
        "body",        "client_secret", "command", "content", "cookie",     "credential",     "credentials",   "cwd",
        "error",       "goal",          "home",    "json",    "markdown",   "narrative",      "password",      "path",
        "private_key", "refresh_token", "prompt",  "query",   "secret",     "session_cookie", "summary",       "token"};
    if (exact.contains(name))
    {
        return true;
    }
    std::string compact{name};
    std::erase(compact, '_');
    static constexpr std::array<std::string_view, 11U> sensitiveCompactFragments{
        "accesskey", "apikey", "auth",       "bearer", "cookie", "credential",
        "password",  "passwd", "privatekey", "secret", "token"};
    if (std::any_of(sensitiveCompactFragments.begin(), sensitiveCompactFragments.end(),
                    [&compact](const std::string_view fragment) noexcept {
                        return compact.find(fragment) != std::string::npos;
                    }))
    {
        return true;
    }
    return containsNameToken(name, "path");
}
[[nodiscard]] bool looksLikePath(const std::string_view value) noexcept
{
    return value.starts_with("/") || value.starts_with("\\") || value.starts_with("file://") ||
           (value.size() >= 3U && std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':' &&
            (value[2] == '\\' || value[2] == '/')) ||
           value.find("\\Users\\") != std::string_view::npos || value.find("/Users/") != std::string_view::npos;
}

[[nodiscard]] bool isRedactionMarker(const std::string_view value) noexcept
{
    if (!value.starts_with("<redacted:") || !value.ends_with("b>") || value.size() < 13U)
    {
        return false;
    }
    return std::all_of(value.begin() + 10, value.end() - 2,
                       [](const char character) noexcept { return character >= '0' && character <= '9'; });
}
[[nodiscard]] std::string flatten(std::string value)
{
    std::replace_if(
        value.begin(), value.end(),
        [](const char character) noexcept { return character == '\r' || character == '\n' || character == '\t'; }, ' ');
    return value;
}

[[nodiscard]] Domain::Result<std::string> redactText(Contracts::IRedactor &redactor, const std::string_view value,
                                                     const std::size_t maximumBytes,
                                                     const std::string_view label) noexcept
{
    try
    {
        auto redacted = redactor.redact(value);
        if (!redacted)
        {
            return redacted;
        }
        auto output = flatten(std::move(redacted).value());
        auto validUtf8 = Detail::strictUtf8ToUtf16(output);
        if (!validUtf8)
        {
            return Domain::Result<std::string>::failure(std::move(validUtf8).error());
        }
        if (output.empty() || output.size() > maximumBytes || output.find('\0') != std::string::npos)
        {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, std::string{label} + " exceeds its post-redaction byte bound."));
        }
        return Domain::Result<std::string>::success(std::move(output));
    }
    catch (...)
    {
        return Domain::Result<std::string>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{label} + " could not be redacted."));
    }
}

[[nodiscard]] Domain::Result<Domain::DiagnosticEnvelope> redactEnvelope(const Domain::DiagnosticEnvelope &event,
                                                                        Contracts::IRedactor &redactor) noexcept
{
    try
    {
        auto valid = Domain::validateDiagnosticEnvelope(event);
        if (!valid)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(std::move(valid).error());
        }

        Domain::DiagnosticEnvelope output = event;
        auto eventName =
            redactText(redactor, event.event, Domain::MaximumDiagnosticEventBytes, "Diagnostic event name");
        if (!eventName)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(std::move(eventName).error());
        }
        output.event = std::move(eventName).value();

        auto role = redactText(redactor, event.role, Domain::MaximumDiagnosticRoleBytes, "Diagnostic role");
        if (!role)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(std::move(role).error());
        }
        output.role = std::move(role).value();

        std::set<std::string, std::less<>> names;
        std::size_t flattenedBytes = 0U;
        for (std::size_t index = 0U; index < event.fields.size(); ++index)
        {
            auto name = redactText(redactor, event.fields[index].name, Domain::MaximumDiagnosticFieldNameBytes,
                                   "Diagnostic field name");
            if (!name)
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(std::move(name).error());
            }
            auto value = redactText(redactor, event.fields[index].value, Domain::MaximumDiagnosticFieldValueBytes,
                                    "Diagnostic field value");
            if (!value)
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(std::move(value).error());
            }

            output.fields[index].name = std::move(name).value();
            if (isPrivateFieldName(event.fields[index].name) || looksLikePath(event.fields[index].value))
            {
                output.fields[index].value =
                    isRedactionMarker(event.fields[index].value)
                        ? event.fields[index].value
                        : "<redacted:" + std::to_string(event.fields[index].value.size()) + "b>";
            }
            else
            {
                output.fields[index].value = std::move(value).value();
            }
            if (!names.insert(output.fields[index].name).second)
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest, "Diagnostic field names must be unique after redaction."));
            }
            flattenedBytes += output.fields[index].name.size();
            flattenedBytes += output.fields[index].value.size();
        }
        if (flattenedBytes > Domain::MaximumDiagnosticFlattenedFieldBytes)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Flattened diagnostic fields exceed 512 UTF-8 bytes."));
        }
        std::sort(output.fields.begin(), output.fields.end(),
                  [](const Domain::DiagnosticField &left, const Domain::DiagnosticField &right) noexcept {
                      return std::tie(left.name, left.value) < std::tie(right.name, right.value);
                  });
        return Domain::Result<Domain::DiagnosticEnvelope>::success(std::move(output));
    }
    catch (...)
    {
        return Domain::Result<Domain::DiagnosticEnvelope>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "The diagnostic envelope could not be redacted."));
    }
}

[[nodiscard]] Json jsonForEnvelope(const Domain::DiagnosticEnvelope &envelope)
{
    Json fields = Json::object();
    for (const auto &field : envelope.fields)
    {
        fields[field.name] = field.value;
    }
    Json document{{"category", categoryName(envelope.category)},
                  {"event", envelope.event},
                  {"pid", envelope.processId},
                  {"role", envelope.role},
                  {"severity", severityName(envelope.severity)},
                  {"ts", timestampText(envelope.timestamp)}};
    if (!fields.empty())
    {
        document["fields"] = std::move(fields);
    }
    return document;
}

[[nodiscard]] Domain::Result<std::string> encodeJsonLine(const Domain::DiagnosticEnvelope &envelope) noexcept
{
    try
    {
        std::string line = jsonForEnvelope(envelope).dump(-1, ' ', false, Json::error_handler_t::strict);
        line.push_back('\n');
        return Domain::Result<std::string>::success(std::move(line));
    }
    catch (...)
    {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The diagnostic event could not be encoded as strict JSONL."));
    }
}

[[nodiscard]] std::vector<std::byte> bytesFor(const std::string &value)
{
    std::vector<std::byte> bytes(value.size());
    if (!value.empty())
    {
        std::memcpy(bytes.data(), value.data(), value.size());
    }
    return bytes;
}

[[nodiscard]] std::string markdownCell(std::string value)
{
    value = flatten(std::move(value));
    std::string output;
    output.reserve(value.size());
    for (const char character : value)
    {
        if (character == '|')
        {
            output.append("\\|");
        }
        else
        {
            output.push_back(character);
        }
    }
    return output;
}

[[nodiscard]] bool validBasename(const std::string_view value) noexcept
{
    return !value.empty() && value.size() <= WindowsDiagnosticSink::MaximumExportBasenameBytes &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) noexcept {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' || character == '_';
           });
}

[[nodiscard]] std::wstring extendedPath(const std::wstring_view path)
{
    std::wstring result{L"\\\\?\\"};
    result.append(path);
    return result;
}

[[nodiscard]] std::wstring withoutExtendedPrefix(std::wstring value)
{
    if (value.starts_with(L"\\\\?\\UNC\\"))
    {
        value.erase(0, 7);
        value.insert(value.begin(), L'\\');
    }
    else if (value.starts_with(L"\\\\?\\"))
    {
        value.erase(0, 4);
    }
    return value;
}

[[nodiscard]] bool equalPath(const std::wstring_view left, const std::wstring_view right) noexcept
{
    return left.size() == right.size() &&
           ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] Domain::Result<void> verifyOpenedPath(const HANDLE handle, const std::wstring_view expectedPath,
                                                    const bool expectDirectory) noexcept
{
    try
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE)
        {
            return Domain::Result<void>::failure(
                Detail::makeWin32Error("inspect the diagnostic path handle", ::GetLastError()));
        }
        const bool isDirectory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || isDirectory != expectDirectory)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                  "The diagnostic path has an unexpected type or is a reparse point."));
        }

        if (expectDirectory)
        {
            FILE_CASE_SENSITIVE_INFO caseSensitivity{};
            if (::GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &caseSensitivity,
                                               sizeof(caseSensitivity)) == FALSE)
            {
                return Domain::Result<void>::failure(
                    Detail::makeWin32Error("inspect diagnostic directory case sensitivity", ::GetLastError()));
            }
            auto supported = Detail::validateDiagnosticDirectoryCaseSensitivity(caseSensitivity.Flags);
            if (!supported)
            {
                return supported;
            }
        }
        else
        {
            FILE_STANDARD_INFO standard{};
            if (::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE)
            {
                return Domain::Result<void>::failure(
                    Detail::makeWin32Error("inspect diagnostic file link state", ::GetLastError()));
            }
            if (standard.NumberOfLinks != 1U || standard.DeletePending != FALSE)
            {
                return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                                                       "The diagnostic file is delete-pending or has "
                                                                       "more than one hard link."));
            }
        }

        const DWORD required = ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0U)
        {
            return Domain::Result<void>::failure(
                Detail::makeWin32Error("resolve the diagnostic path handle", ::GetLastError()));
        }
        if (required > Domain::PathText::MaximumBytes + 4U)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "The resolved diagnostic path exceeds the Windows path bound."));
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0U || written >= buffer.size())
        {
            return Domain::Result<void>::failure(
                Detail::makeWin32Error("resolve the diagnostic path handle", ::GetLastError()));
        }
        const auto opened = withoutExtendedPrefix(std::wstring{buffer.data(), static_cast<std::size_t>(written)});
        if (!equalPath(opened, expectedPath))
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority, "The opened diagnostic path escaped its canonical path."));
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                                               "The opened diagnostic path could not be verified "
                                                               "within its allocation bound."));
    }
}

[[nodiscard]] Domain::Result<void> verifyOpenedFile(const HANDLE handle, const std::wstring_view expectedPath) noexcept
{
    return verifyOpenedPath(handle, expectedPath, false);
}

[[nodiscard]] Domain::Result<void> verifyOpenedDirectory(const HANDLE handle,
                                                         const std::wstring_view expectedPath) noexcept
{
    return verifyOpenedPath(handle, expectedPath, true);
}
[[nodiscard]] Domain::Result<void> validateDiagnosticTransactionStart(const Domain::OperationContext &context,
                                                                      HANDLE shutdownEvent) noexcept;
[[nodiscard]] DWORD boundedWaitMilliseconds(Domain::MonotonicTimePoint deadline) noexcept;

[[nodiscard]] Domain::Result<std::wstring> ensureDiagnosticsRoot(
    const Domain::PathText &rootText, const Domain::OperationContext &context, const HANDLE shutdownEvent,
    Detail::IDiagnosticDirectoryAnchorObserver *const observer = nullptr,
    std::vector<Detail::UniqueHandle> *const retainedAnchors = nullptr) noexcept
{
    try
    {
        auto root = Detail::WindowsPathResolver::resolveAppOwnedRoot(rootText.value());
        if (!root)
        {
            return root;
        }
        auto currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!currentContext)
        {
            return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
        }

        std::vector<Detail::UniqueHandle> creationAnchors;
        std::wstring current = root.value().substr(0U, 3U);
        const auto driveNative = extendedPath(current);
        Detail::UniqueHandle driveAnchor{::CreateFileW(
            driveNative.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!driveAnchor)
        {
            return Domain::Result<std::wstring>::failure(
                Detail::makeWin32Error("anchor the diagnostics volume root", ::GetLastError()));
        }
        auto verified = verifyOpenedDirectory(driveAnchor.get(), current);
        if (!verified)
        {
            return Domain::Result<std::wstring>::failure(std::move(verified).error());
        }
        creationAnchors.push_back(std::move(driveAnchor));
        if (observer != nullptr)
        {
            observer->onDirectoryAnchored(current);
        }

        std::size_t cursor = 3U;
        while (cursor < root.value().size())
        {
            currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
            if (!currentContext)
            {
                return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
            }

            const std::size_t separator = root.value().find(L'\\', cursor);
            const std::size_t end = separator == std::wstring::npos ? root.value().size() : separator;
            const std::wstring_view component{root.value().data() + cursor, end - cursor};
            const std::wstring childPath{root.value().substr(0U, end)};
            const ACCESS_MASK childAccess = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE;

            Detail::RelativeOpenOptions childOptions{childAccess, FILE_SHARE_READ,
                                                     Detail::RelativeOpenDisposition::OpenExisting,
                                                     FILE_ATTRIBUTE_NORMAL, Detail::RelativeObjectType::Directory};
            auto child = Detail::openRelative(creationAnchors.back().get(), component, childOptions);
            while (!child && child.win32Error == ERROR_SHARING_VIOLATION)
            {
                const DWORD waitResult =
                    ::WaitForSingleObject(shutdownEvent, boundedWaitMilliseconds(context.deadline));
                if (waitResult != WAIT_TIMEOUT)
                {
                    currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
                    if (!currentContext)
                    {
                        return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
                    }
                    if (waitResult == WAIT_FAILED)
                    {
                        return Domain::Result<std::wstring>::failure(Detail::makeWin32Error(
                            "wait for diagnostics directory mutation authority", ::GetLastError()));
                    }
                }
                currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
                if (!currentContext)
                {
                    return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
                }
                child = Detail::openRelative(creationAnchors.back().get(), component, childOptions);
            }
            if (!child && child.win32Error != ERROR_FILE_NOT_FOUND && child.win32Error != ERROR_PATH_NOT_FOUND)
            {
                return Domain::Result<std::wstring>::failure(
                    Detail::makeWin32Error("open an anchored diagnostics directory", child.win32Error));
            }
            if (!child)
            {
                currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
                if (!currentContext)
                {
                    return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
                }
                childOptions.disposition = Detail::RelativeOpenDisposition::OpenOrCreate;
                child = Detail::openRelative(creationAnchors.back().get(), component, childOptions);
                if (!child)
                {
                    return Domain::Result<std::wstring>::failure(
                        Detail::makeWin32Error("create an anchored diagnostics directory", child.win32Error));
                }
            }

            verified = verifyOpenedDirectory(child.handle.get(), childPath);
            if (!verified)
            {
                return Domain::Result<std::wstring>::failure(std::move(verified).error());
            }
            creationAnchors.push_back(std::move(child.handle));
            current = childPath;
            if (observer != nullptr)
            {
                observer->onDirectoryAnchored(current);
            }
            currentContext = validateDiagnosticTransactionStart(context, shutdownEvent);
            if (!currentContext)
            {
                return Domain::Result<std::wstring>::failure(std::move(currentContext).error());
            }
            cursor = end + 1U;
        }
        if (retainedAnchors != nullptr)
        {
            *retainedAnchors = std::move(creationAnchors);
        }
        return Domain::Result<std::wstring>::success(std::move(root).value());
    }
    catch (...)
    {
        return Domain::Result<std::wstring>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "The diagnostics directory could not be prepared."));
    }
}

using AnchoredDirectoryTree = Detail::AnchoredDiagnosticDirectoryTree;

[[nodiscard]] Domain::Result<AnchoredDirectoryTree> prepareAnchoredDiagnosticsRoot(
    const Domain::PathText &rootText, const Domain::OperationContext &context, const HANDLE shutdownEvent) noexcept
{
    return Detail::prepareAnchoredDiagnosticDirectory(rootText, context, shutdownEvent);
}
struct ExistingFile final
{
    bool exists{};
    std::uint64_t bytes{};
};

[[nodiscard]] Domain::Error diagnosticFileError(const std::string_view action, const DWORD nativeError) noexcept
{
    const bool storageFull = nativeError == ERROR_DISK_FULL || nativeError == ERROR_HANDLE_DISK_FULL;
    return Detail::makeWin32Error(action, nativeError,
                                  storageFull ? Domain::ErrorCodes::StorageFull : Domain::ErrorCodes::InternalFailure,
                                  nativeError == ERROR_SHARING_VIOLATION || nativeError == ERROR_LOCK_VIOLATION);
}

[[nodiscard]] Domain::Error diagnosticLockInterruption(const Domain::OperationContext &context,
                                                       const HANDLE shutdownEvent) noexcept
{
    if (context.isCancellationRequested())
    {
        return Domain::makeError(Domain::ErrorCodes::Cancelled,
                                 "The diagnostic file transaction was cancelled while waiting for "
                                 "the interprocess lock.");
    }
    if (::WaitForSingleObject(shutdownEvent, 0U) == WAIT_OBJECT_0)
    {
        return Domain::makeError(Domain::ErrorCodes::TransportClosed,
                                 "The diagnostic sink began shutdown while waiting "
                                 "for the interprocess lock.");
    }
    return Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,
                             "The diagnostic file transaction exceeded its deadline while waiting "
                             "for the interprocess lock.");
}

[[nodiscard]] Domain::Result<void> validateDiagnosticTransactionStart(const Domain::OperationContext &context,
                                                                      const HANDLE shutdownEvent) noexcept
{
    if (context.isCancellationRequested())
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled, "The diagnostic file transaction was cancelled before mutation."));
    }
    const DWORD shutdownStatus = ::WaitForSingleObject(shutdownEvent, 0U);
    if (shutdownStatus == WAIT_OBJECT_0)
    {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::TransportClosed,
                                                               "The diagnostic sink shut down before the file "
                                                               "transaction could mutate storage."));
    }
    if (shutdownStatus == WAIT_FAILED)
    {
        return Domain::Result<void>::failure(
            diagnosticFileError("inspect diagnostic sink shutdown state", ::GetLastError()));
    }
    if (context.isExpired(std::chrono::steady_clock::now()))
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded, "The diagnostic file transaction expired before mutation."));
    }
    return Domain::Result<void>::success();
}
[[nodiscard]] DWORD boundedWaitMilliseconds(const Domain::MonotonicTimePoint deadline) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return 0U;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    constexpr auto maximumWait = static_cast<long long>((std::numeric_limits<DWORD>::max)() - 1U);
    constexpr long long MaximumPollMilliseconds = 25LL;
    return static_cast<DWORD>((std::min)(remaining, (std::min)(maximumWait, MaximumPollMilliseconds)));
}

class DiagnosticFileLock final
{
  public:
    DiagnosticFileLock(const DiagnosticFileLock &) = delete;
    DiagnosticFileLock &operator=(const DiagnosticFileLock &) = delete;
    DiagnosticFileLock &operator=(DiagnosticFileLock &&) = delete;

    DiagnosticFileLock(DiagnosticFileLock &&other) noexcept
        : handle_{std::move(other.handle_)}, locked_{std::exchange(other.locked_, false)}
    {
    }

    ~DiagnosticFileLock() noexcept
    {
        release();
    }

    [[nodiscard]] static Domain::Result<DiagnosticFileLock> acquire(const std::wstring_view path,
                                                                    AnchoredDirectoryTree &anchoredRoot,
                                                                    const Domain::OperationContext &context,
                                                                    const HANDLE shutdownEvent) noexcept
    {
        try
        {
            if (context.isCancellationRequested() || context.isExpired(std::chrono::steady_clock::now()) ||
                ::WaitForSingleObject(shutdownEvent, 0U) == WAIT_OBJECT_0)
            {
                return Domain::Result<DiagnosticFileLock>::failure(diagnosticLockInterruption(context, shutdownEvent));
            }

            if (anchoredRoot.handles.empty())
            {
                return Domain::Result<DiagnosticFileLock>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure, "The diagnostic lock has no retained root anchor."));
            }
            Detail::RelativeOpenOptions lockOptions{};
            lockOptions.desiredAccess = GENERIC_READ | GENERIC_WRITE;
            lockOptions.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
            lockOptions.disposition = Detail::RelativeOpenDisposition::OpenOrCreate;
            lockOptions.objectType = Detail::RelativeObjectType::File;
            auto openedLock =
                Detail::openRelative(anchoredRoot.handles.back().get(), L".forge-diagnostics.lock", lockOptions);
            if (!openedLock)
            {
                return Domain::Result<DiagnosticFileLock>::failure(diagnosticFileError(
                    "open the handle-relative diagnostic interprocess lock", openedLock.win32Error));
            }
            auto verified = verifyOpenedFile(openedLock.handle.get(), path);
            if (!verified)
            {
                return Domain::Result<DiagnosticFileLock>::failure(std::move(verified).error());
            }
            Detail::UniqueHandle handle{std::move(openedLock.handle)};

            Detail::UniqueHandle cancellationEvent{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!cancellationEvent)
            {
                return Domain::Result<DiagnosticFileLock>::failure(
                    diagnosticFileError("create a diagnostic lock wait event", ::GetLastError()));
            }

            std::stop_callback cancellationWake{context.cancellation, [event = cancellationEvent.get()]() noexcept {
                                                    static_cast<void>(::SetEvent(event));
                                                }};
            const std::array<HANDLE, 2U> waitHandles{cancellationEvent.get(), shutdownEvent};

            while (true)
            {
                if (context.isCancellationRequested() || context.isExpired(std::chrono::steady_clock::now()) ||
                    ::WaitForSingleObject(shutdownEvent, 0U) == WAIT_OBJECT_0)
                {
                    return Domain::Result<DiagnosticFileLock>::failure(
                        diagnosticLockInterruption(context, shutdownEvent));
                }

                // FAIL_IMMEDIATELY keeps OVERLAPPED stack storage synchronous:
                // no kernel operation can outlive this loop iteration.
                OVERLAPPED operation{};
                if (::LockFileEx(handle.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U,
                                 &operation) != FALSE)
                {
                    DiagnosticFileLock acquired{std::move(handle), true};
                    if (context.isCancellationRequested() || context.isExpired(std::chrono::steady_clock::now()) ||
                        ::WaitForSingleObject(shutdownEvent, 0U) == WAIT_OBJECT_0)
                    {
                        return Domain::Result<DiagnosticFileLock>::failure(
                            diagnosticLockInterruption(context, shutdownEvent));
                    }
                    return Domain::Result<DiagnosticFileLock>::success(std::move(acquired));
                }
                const DWORD lockError = ::GetLastError();
                if (lockError != ERROR_LOCK_VIOLATION)
                {
                    return Domain::Result<DiagnosticFileLock>::failure(
                        diagnosticFileError("acquire the diagnostic interprocess lock", lockError));
                }

                const DWORD waitResult =
                    ::WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(), FALSE,
                                             boundedWaitMilliseconds(context.deadline));
                if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_OBJECT_0 + 1U ||
                    (waitResult == WAIT_TIMEOUT && context.isExpired(std::chrono::steady_clock::now())))
                {
                    return Domain::Result<DiagnosticFileLock>::failure(
                        diagnosticLockInterruption(context, shutdownEvent));
                }
                if (waitResult == WAIT_TIMEOUT)
                {
                    continue;
                }

                const DWORD waitError = waitResult == WAIT_FAILED ? ::GetLastError() : ERROR_INVALID_FUNCTION;
                return Domain::Result<DiagnosticFileLock>::failure(
                    diagnosticFileError("wait for the diagnostic interprocess lock", waitError));
            }
        }
        catch (...)
        {
            return Domain::Result<DiagnosticFileLock>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure, "The diagnostic interprocess lock could not be acquired."));
        }
    }

  private:
    DiagnosticFileLock(Detail::UniqueHandle handle, const bool locked) noexcept
        : handle_{std::move(handle)}, locked_{locked}
    {
    }

    void release() noexcept
    {
        if (locked_ && handle_)
        {
            OVERLAPPED operation{};
            static_cast<void>(::UnlockFileEx(handle_.get(), 0U, 1U, 0U, &operation));
        }
        locked_ = false;
    }

    Detail::UniqueHandle handle_;
    bool locked_{};
};
[[nodiscard]] Domain::Result<ExistingFile> inspectFile(const std::wstring_view path) noexcept
{
    try
    {
        const auto native = extendedPath(path);
        Detail::UniqueHandle handle{
            ::CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!handle)
        {
            const DWORD nativeError = ::GetLastError();
            if (nativeError == ERROR_FILE_NOT_FOUND || nativeError == ERROR_PATH_NOT_FOUND)
            {
                return Domain::Result<ExistingFile>::success(ExistingFile{});
            }
            return Domain::Result<ExistingFile>::failure(
                diagnosticFileError("inspect the diagnostic file", nativeError));
        }
        auto verified = verifyOpenedFile(handle.get(), path);
        if (!verified)
        {
            return Domain::Result<ExistingFile>::failure(std::move(verified).error());
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(handle.get(), &size) == FALSE || size.QuadPart < 0)
        {
            return Domain::Result<ExistingFile>::failure(
                diagnosticFileError("measure the diagnostic file", ::GetLastError()));
        }
        return Domain::Result<ExistingFile>::success(ExistingFile{true, static_cast<std::uint64_t>(size.QuadPart)});
    }
    catch (...)
    {
        return Domain::Result<ExistingFile>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "The diagnostic file could not be inspected."));
    }
}

[[nodiscard]] Domain::Result<std::wstring> diagnosticBasename(const std::wstring_view root,
                                                              const std::wstring_view path) noexcept
{
    try
    {
        const std::size_t separator = path.find_last_of(L'\\');
        if (separator == std::wstring_view::npos || separator + 1U >= path.size() ||
            !equalPath(root, path.substr(0U, separator)))
        {
            return Domain::Result<std::wstring>::failure(
                Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                  "A diagnostic file operation escaped its retained root handle."));
        }
        return Domain::Result<std::wstring>::success(std::wstring{path.substr(separator + 1U)});
    }
    catch (...)
    {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "A diagnostic file basename could not be prepared."));
    }
}

[[nodiscard]] Domain::Result<ExistingFile> inspectRelativeFile(const std::wstring_view root,
                                                               const std::wstring_view path,
                                                               const HANDLE anchoredRoot) noexcept
{
    auto basename = diagnosticBasename(root, path);
    if (!basename)
    {
        return Domain::Result<ExistingFile>::failure(std::move(basename).error());
    }
    Detail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_READ_ATTRIBUTES;
    options.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    options.disposition = Detail::RelativeOpenDisposition::OpenExisting;
    options.objectType = Detail::RelativeObjectType::File;
    auto opened = Detail::openRelative(anchoredRoot, basename.value(), options);
    if (!opened)
    {
        if (opened.win32Error == ERROR_FILE_NOT_FOUND || opened.win32Error == ERROR_PATH_NOT_FOUND)
        {
            return Domain::Result<ExistingFile>::success(ExistingFile{});
        }
        return Domain::Result<ExistingFile>::failure(
            diagnosticFileError("inspect a handle-relative diagnostic file", opened.win32Error));
    }
    auto verified = verifyOpenedFile(opened.handle.get(), path);
    if (!verified)
    {
        return Domain::Result<ExistingFile>::failure(std::move(verified).error());
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(opened.handle.get(), &size) == FALSE || size.QuadPart < 0)
    {
        return Domain::Result<ExistingFile>::failure(
            diagnosticFileError("measure a handle-relative diagnostic file", ::GetLastError()));
    }
    return Domain::Result<ExistingFile>::success(ExistingFile{true, static_cast<std::uint64_t>(size.QuadPart)});
}

[[nodiscard]] Domain::Result<std::uint64_t> openedFileSize(HANDLE handle) noexcept;

[[nodiscard]] Domain::Result<void> deleteIfExists(const std::wstring_view root, const std::wstring_view path,
                                                  const HANDLE anchoredRoot) noexcept
{
    auto basename = diagnosticBasename(root, path);
    if (!basename)
    {
        return Domain::Result<void>::failure(std::move(basename).error());
    }
    Detail::RelativeOpenOptions options{};
    options.desiredAccess = DELETE | FILE_READ_ATTRIBUTES;
    options.shareAccess = FILE_SHARE_READ;
    options.disposition = Detail::RelativeOpenDisposition::OpenExisting;
    options.objectType = Detail::RelativeObjectType::File;
    auto opened = Detail::openRelative(anchoredRoot, basename.value(), options);
    if (!opened)
    {
        if (opened.win32Error == ERROR_FILE_NOT_FOUND || opened.win32Error == ERROR_PATH_NOT_FOUND)
        {
            return Domain::Result<void>::success();
        }
        return Domain::Result<void>::failure(
            diagnosticFileError("open a handle-relative diagnostic file for deletion", opened.win32Error));
    }
    auto verified = verifyOpenedFile(opened.handle.get(), path);
    if (!verified)
    {
        return verified;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (::SetFileInformationByHandle(opened.handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)) ==
        FALSE)
    {
        return Domain::Result<void>::failure(
            diagnosticFileError("delete a handle-relative diagnostic file", ::GetLastError()));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::wstring diagnosticRotationTemporaryName(const std::size_t slot)
{
    std::wstring result{DiagnosticRotationTemporaryPrefix};
    result.append(std::to_wstring(slot));
    result.append(DiagnosticRotationTemporarySuffix);
    return result;
}

[[nodiscard]] std::wstring diagnosticChildPath(const std::wstring_view root, const std::wstring_view name)
{
    std::wstring result{root};
    if (!result.ends_with(L'\\'))
    {
        result.push_back(L'\\');
    }
    result.append(name);
    return result;
}

class PendingDiagnosticRotationFile final
{
  public:
    PendingDiagnosticRotationFile(std::wstring name, std::wstring path, Detail::UniqueHandle handle) noexcept
        : name_{std::move(name)}, path_{std::move(path)}, handle_{std::move(handle)}
    {
    }

    PendingDiagnosticRotationFile(const PendingDiagnosticRotationFile &) = delete;
    PendingDiagnosticRotationFile &operator=(const PendingDiagnosticRotationFile &) = delete;
    PendingDiagnosticRotationFile &operator=(PendingDiagnosticRotationFile &&) = delete;

    PendingDiagnosticRotationFile(PendingDiagnosticRotationFile &&other) noexcept
        : name_{std::move(other.name_)}, path_{std::move(other.path_)}, handle_{std::move(other.handle_)},
          discardOnClose_{other.discardOnClose_}
    {
        other.discardOnClose_ = false;
    }

    ~PendingDiagnosticRotationFile() noexcept
    {
        if (discardOnClose_ && handle_)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            static_cast<void>(
                ::SetFileInformationByHandle(handle_.get(), FileDispositionInfo, &disposition, sizeof(disposition)));
        }
    }

    [[nodiscard]] HANDLE handle() const noexcept
    {
        return handle_.get();
    }
    [[nodiscard]] const std::wstring &name() const noexcept
    {
        return name_;
    }
    [[nodiscard]] const std::wstring &path() const noexcept
    {
        return path_;
    }
    void markCommitted() noexcept
    {
        discardOnClose_ = false;
    }

  private:
    std::wstring name_;
    std::wstring path_;
    Detail::UniqueHandle handle_;
    bool discardOnClose_{true};
};

[[nodiscard]] Domain::Result<PendingDiagnosticRotationFile> createDiagnosticRotationTemporary(
    const std::wstring_view root, const HANDLE anchoredRoot) noexcept
{
    try
    {
        UCHAR entropy{};
        const NTSTATUS randomStatus =
            ::BCryptGenRandom(nullptr, &entropy, sizeof(entropy), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(randomStatus))
        {
            return Domain::Result<PendingDiagnosticRotationFile>::failure(
                Detail::makeNtStatusError("generate a diagnostic rotation temporary name", randomStatus));
        }

        const std::size_t firstSlot = entropy % DiagnosticRotationTemporarySlots;
        for (std::size_t attempt = 0U; attempt < DiagnosticRotationTemporarySlots; ++attempt)
        {
            const std::size_t slot = (firstSlot + attempt) % DiagnosticRotationTemporarySlots;
            std::wstring name = diagnosticRotationTemporaryName(slot);
            std::wstring path = diagnosticChildPath(root, name);

            Detail::RelativeOpenOptions options{};
            options.desiredAccess = GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES;
            options.shareAccess = 0U;
            options.disposition = Detail::RelativeOpenDisposition::CreateNew;
            options.objectType = Detail::RelativeObjectType::File;
            options.writeThrough = true;
            options.sequentialAccess = true;
            auto opened = Detail::openRelative(anchoredRoot, name, options);
            if (!opened)
            {
                if (opened.win32Error == ERROR_FILE_EXISTS || opened.win32Error == ERROR_ALREADY_EXISTS)
                {
                    continue;
                }
                return Domain::Result<PendingDiagnosticRotationFile>::failure(
                    diagnosticFileError("create an exclusive diagnostic rotation temporary", opened.win32Error));
            }

            PendingDiagnosticRotationFile pending{std::move(name), std::move(path), std::move(opened.handle)};
            auto verified = verifyOpenedFile(pending.handle(), pending.path());
            if (!verified)
            {
                return Domain::Result<PendingDiagnosticRotationFile>::failure(std::move(verified).error());
            }
            return Domain::Result<PendingDiagnosticRotationFile>::success(std::move(pending));
        }

        return Domain::Result<PendingDiagnosticRotationFile>::failure(Domain::makeError(
            Domain::ErrorCodes::Conflict, "Every bounded diagnostic rotation temporary slot is occupied.", true));
    }
    catch (...)
    {
        return Domain::Result<PendingDiagnosticRotationFile>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The diagnostic rotation temporary could not be prepared."));
    }
}

[[nodiscard]] Domain::Result<void> publishDiagnosticRotationTemporary(
    PendingDiagnosticRotationFile &temporary, const HANDLE anchoredRoot, const std::wstring_view destinationName,
    Detail::IDiagnosticRotationPublishObserver *const observer) noexcept
{
    try
    {
        if (anchoredRoot == nullptr || anchoredRoot == INVALID_HANDLE_VALUE || destinationName.empty() ||
            destinationName.find_first_of(L"\\/:") != std::wstring_view::npos ||
            destinationName.size() > (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t))
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest, "The diagnostic rotation publication name is invalid."));
        }

        const std::size_t nameBytes = destinationName.size() * sizeof(wchar_t);
        const std::size_t informationBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        if (informationBytes > (std::numeric_limits<ULONG>::max)())
        {
            return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                                                   "The diagnostic rotation publication name exceeds "
                                                                   "its native bound."));
        }

        std::vector<std::uint64_t> storage((informationBytes + sizeof(std::uint64_t) - 1U) / sizeof(std::uint64_t));
        auto *const information = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
        std::memset(information, 0, informationBytes);
        // Rotation deletes or vacates each destination before moving its
        // predecessor. Preserve CREATE_NEW semantics here: a concurrently
        // introduced destination must make publication fail instead of being
        // unlinked or replaced.
        information->Flags = 0U;
        // The staged file was created relative to the retained final directory.
        // Native class 65 with a null root and one-component name performs the
        // proven same-directory rename without reopening that strongly shared
        // parent path.
        information->RootDirectory = nullptr;
        information->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(information->FileName, destinationName.data(), nameBytes);
        information->FileName[destinationName.size()] = L'\0';

        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return Domain::Result<void>::failure(
                diagnosticFileError("load native diagnostic rotation support", ::GetLastError()));
        }
        const auto ntSetInformationFile =
            reinterpret_cast<NtSetInformationFileFunction>(::GetProcAddress(ntdll, "NtSetInformationFile"));
        const auto rtlNtStatusToDosError =
            reinterpret_cast<RtlNtStatusToDosErrorFunction>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        if (ntSetInformationFile == nullptr || rtlNtStatusToDosError == nullptr)
        {
            return Domain::Result<void>::failure(
                diagnosticFileError("resolve native diagnostic rotation support", ERROR_PROC_NOT_FOUND));
        }

        if (observer != nullptr)
        {
            observer->beforeStagedFileValidation(temporary.path());
        }
        // CreateHardLinkW does not honor file share mode. Query the exact staged
        // handle only after every fallible native-publication preparation step and
        // immediately before the class-65 rename.
        auto verified = verifyOpenedFile(temporary.handle(), temporary.path());
        if (!verified)
        {
            return verified;
        }

        NativeIoStatusBlock ioStatus{};
        const LONG status = ntSetInformationFile(temporary.handle(), &ioStatus, information,
                                                 static_cast<ULONG>(informationBytes), NativeFileRenameInformationEx);
        if (status < 0)
        {
            const DWORD nativeError = static_cast<DWORD>(rtlNtStatusToDosError(status));
            if (nativeError == ERROR_FILE_EXISTS || nativeError == ERROR_ALREADY_EXISTS)
            {
                return Domain::Result<void>::failure(
                    Domain::makeError(Domain::ErrorCodes::Conflict,
                                      "A diagnostic rotation destination changed during publication.", true));
            }
            return Domain::Result<void>::failure(
                diagnosticFileError("publish a complete handle-relative diagnostic archive", nativeError));
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "The diagnostic rotation temporary could not be published."));
    }
}

[[nodiscard]] Domain::Result<void> pruneStaleDiagnosticRotationTemporaries(const std::wstring_view root,
                                                                           const HANDLE anchoredRoot) noexcept
{
    try
    {
        for (std::size_t slot = 0U; slot < DiagnosticRotationTemporarySlots; ++slot)
        {
            const std::wstring path = diagnosticChildPath(root, diagnosticRotationTemporaryName(slot));
            auto removed = deleteIfExists(root, path, anchoredRoot);
            if (!removed)
            {
                return removed;
            }
        }
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Stale diagnostic rotation temporaries could not be pruned."));
    }
}

[[nodiscard]] Domain::Result<void> moveIfExists(const std::wstring_view root, const std::wstring_view source,
                                                const std::wstring_view destination, const HANDLE anchoredRoot,
                                                const std::size_t maximumBytes, const Domain::OperationContext &context,
                                                const HANDLE shutdownEvent,
                                                Detail::IDiagnosticRotationPublishObserver *const observer) noexcept
{
    try
    {
        const std::size_t sourceSeparator = source.find_last_of(L'\\');
        const std::size_t destinationSeparator = destination.find_last_of(L'\\');
        if (sourceSeparator == std::wstring_view::npos || destinationSeparator == std::wstring_view::npos ||
            !equalPath(source.substr(0U, sourceSeparator), destination.substr(0U, destinationSeparator)))
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                  "Diagnostic rotation must remain in its anchored directory."));
        }
        auto sourceName = diagnosticBasename(root, source);
        if (!sourceName)
        {
            return Domain::Result<void>::failure(std::move(sourceName).error());
        }
        auto destinationName = diagnosticBasename(root, destination);
        if (!destinationName)
        {
            return Domain::Result<void>::failure(std::move(destinationName).error());
        }

        Detail::RelativeOpenOptions sourceOptions{};
        sourceOptions.desiredAccess = GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES;
        sourceOptions.shareAccess = FILE_SHARE_READ;
        sourceOptions.disposition = Detail::RelativeOpenDisposition::OpenExisting;
        sourceOptions.objectType = Detail::RelativeObjectType::File;
        sourceOptions.writeThrough = true;
        sourceOptions.sequentialAccess = true;
        auto sourceHandle = Detail::openRelative(anchoredRoot, sourceName.value(), sourceOptions);
        if (!sourceHandle)
        {
            if (sourceHandle.win32Error == ERROR_FILE_NOT_FOUND || sourceHandle.win32Error == ERROR_PATH_NOT_FOUND)
            {
                return Domain::Result<void>::success();
            }
            return Domain::Result<void>::failure(
                diagnosticFileError("open a handle-relative diagnostic file for rotation", sourceHandle.win32Error));
        }
        auto verified = verifyOpenedFile(sourceHandle.handle.get(), source);
        if (!verified)
        {
            return verified;
        }
        auto sourceBytes = openedFileSize(sourceHandle.handle.get());
        if (!sourceBytes)
        {
            return Domain::Result<void>::failure(std::move(sourceBytes).error());
        }
        if (sourceBytes.value() > maximumBytes)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "A diagnostic archive source exceeds its configured byte bound."));
        }

        auto temporary = createDiagnosticRotationTemporary(root, anchoredRoot);
        if (!temporary)
        {
            return Domain::Result<void>::failure(std::move(temporary).error());
        }
        if (observer != nullptr)
        {
            observer->afterStagedFileCreation(temporary.value().path());
        }
        std::array<std::byte, 64U * 1024U> buffer{};
        std::uint64_t copied{};
        while (copied < sourceBytes.value())
        {
            auto ready = validateDiagnosticTransactionStart(context, shutdownEvent);
            if (!ready)
            {
                return ready;
            }
            const DWORD requested =
                static_cast<DWORD>((std::min)(sourceBytes.value() - copied, static_cast<std::uint64_t>(buffer.size())));
            DWORD read{};
            if (::ReadFile(sourceHandle.handle.get(), buffer.data(), requested, &read, nullptr) == FALSE || read == 0U)
            {
                const DWORD nativeError = ::GetLastError();
                return Domain::Result<void>::failure(
                    diagnosticFileError("read a bounded diagnostic archive", nativeError));
            }
            DWORD writeOffset{};
            while (writeOffset < read)
            {
                ready = validateDiagnosticTransactionStart(context, shutdownEvent);
                if (!ready)
                {
                    return ready;
                }
                DWORD written{};
                if (::WriteFile(temporary.value().handle(), buffer.data() + writeOffset, read - writeOffset, &written,
                                nullptr) == FALSE ||
                    written == 0U)
                {
                    const DWORD nativeError = ::GetLastError();
                    return Domain::Result<void>::failure(
                        diagnosticFileError("write a bounded diagnostic archive", nativeError));
                }
                writeOffset += written;
            }
            copied += read;
        }
        if (::FlushFileBuffers(temporary.value().handle()) == FALSE)
        {
            const DWORD nativeError = ::GetLastError();
            return Domain::Result<void>::failure(
                diagnosticFileError("flush a bounded diagnostic archive", nativeError));
        }

        auto destinationState = inspectRelativeFile(root, destination, anchoredRoot);
        if (!destinationState)
        {
            return Domain::Result<void>::failure(std::move(destinationState).error());
        }
        if (destinationState.value().exists)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict, "A diagnostic rotation destination changed before publication.", true));
        }
        auto ready = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!ready)
        {
            return ready;
        }
        auto published =
            publishDiagnosticRotationTemporary(temporary.value(), anchoredRoot, destinationName.value(), observer);
        if (!published)
        {
            return published;
        }

        verified = verifyOpenedFile(temporary.value().handle(), destination);
        if (!verified)
        {
            return verified;
        }
        verified = verifyOpenedFile(sourceHandle.handle.get(), source);
        if (!verified)
        {
            return verified;
        }

        FILE_DISPOSITION_INFO sourceDisposition{TRUE};
        if (::SetFileInformationByHandle(sourceHandle.handle.get(), FileDispositionInfo, &sourceDisposition,
                                         sizeof(sourceDisposition)) == FALSE)
        {
            const DWORD nativeError = ::GetLastError();
            return Domain::Result<void>::failure(diagnosticFileError("retire a rotated diagnostic file", nativeError));
        }
        temporary.value().markCommitted();
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
                                                               "Diagnostic log rotation could not prepare its "
                                                               "bounded handle-relative copy."));
    }
}

[[nodiscard]] std::wstring archivePath(const std::wstring_view master, const std::size_t generation)
{
    std::wstring result{master};
    result.push_back(L'.');
    result.append(std::to_wstring(generation));
    return result;
}

[[nodiscard]] Domain::Result<void> validateDiagnosticBudgets(const Domain::ResourceBudgets &budgets) noexcept
{
    if (budgets.diagnosticLogFileBytesMaximum == 0U || budgets.diagnosticLogFilesMaximum == 0U)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "Diagnostic file count and byte budgets must be positive."));
    }
    if (budgets.diagnosticLogFilesMaximum > WindowsDiagnosticSink::MaximumRetainedLogFiles)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "The diagnostic log file budget exceeds the hard cap of ten files."));
    }
    return Domain::Result<void>::success();
}

struct DiagnosticTransaction final
{
    AnchoredDirectoryTree anchoredRoot;
    DiagnosticFileLock lock;
    std::wstring masterPath;
};

[[nodiscard]] Domain::Result<DiagnosticTransaction> prepareDiagnosticTransaction(
    const Domain::PathText &rootText, const Domain::ResourceBudgets &budgets, const Domain::OperationContext &context,
    const HANDLE shutdownEvent) noexcept
{
    auto validBudgets = validateDiagnosticBudgets(budgets);
    if (!validBudgets)
    {
        return Domain::Result<DiagnosticTransaction>::failure(std::move(validBudgets).error());
    }
    auto anchored = prepareAnchoredDiagnosticsRoot(rootText, context, shutdownEvent);
    if (!anchored)
    {
        return Domain::Result<DiagnosticTransaction>::failure(std::move(anchored).error());
    }
    auto master = Detail::WindowsPathResolver::resolveAppOwnedChild(anchored.value().root, L"forge-diagnostics.jsonl",
                                                                    Detail::MissingPathPolicy::AllowLeaf);
    if (!master)
    {
        return Domain::Result<DiagnosticTransaction>::failure(std::move(master).error());
    }
    auto lockPath = Detail::WindowsPathResolver::resolveAppOwnedChild(anchored.value().root, L".forge-diagnostics.lock",
                                                                      Detail::MissingPathPolicy::AllowLeaf);
    if (!lockPath)
    {
        return Domain::Result<DiagnosticTransaction>::failure(std::move(lockPath).error());
    }
    auto lock = DiagnosticFileLock::acquire(lockPath.value(), anchored.value(), context, shutdownEvent);
    if (!lock)
    {
        return Domain::Result<DiagnosticTransaction>::failure(std::move(lock).error());
    }
    return Domain::Result<DiagnosticTransaction>::success(
        DiagnosticTransaction{std::move(anchored).value(), std::move(lock).value(), std::move(master).value()});
}

[[nodiscard]] Domain::Result<void> pruneStaleArchives(const std::wstring_view root, const std::wstring_view master,
                                                      const std::size_t maximumFiles,
                                                      const HANDLE anchoredRoot) noexcept
{
    auto temporaryCleanup = pruneStaleDiagnosticRotationTemporaries(root, anchoredRoot);
    if (!temporaryCleanup)
    {
        return temporaryCleanup;
    }
    for (std::size_t generation = maximumFiles; generation < WindowsDiagnosticSink::MaximumRetainedLogFiles;
         ++generation)
    {
        auto removed = deleteIfExists(root, archivePath(master, generation), anchoredRoot);
        if (!removed)
        {
            return removed;
        }
    }
    return Domain::Result<void>::success();
}
[[nodiscard]] Domain::Result<void> rotateLog(const std::wstring_view root, const std::wstring_view master,
                                             const std::size_t maximumFiles, const std::size_t maximumBytes,
                                             const HANDLE anchoredRoot, const Domain::OperationContext &context,
                                             const HANDLE shutdownEvent,
                                             Detail::IDiagnosticRotationPublishObserver *const observer) noexcept
{
    try
    {
        if (maximumFiles == 0U)
        {
            return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                                                   "The diagnostic log file budget must be positive."));
        }
        const std::size_t archiveCount = maximumFiles - 1U;
        if (archiveCount == 0U)
        {
            return deleteIfExists(root, master, anchoredRoot);
        }

        auto removed = deleteIfExists(root, archivePath(master, archiveCount), anchoredRoot);
        if (!removed)
        {
            return removed;
        }
        for (std::size_t generation = archiveCount; generation > 1U; --generation)
        {
            auto moved = moveIfExists(root, archivePath(master, generation - 1U), archivePath(master, generation),
                                      anchoredRoot, maximumBytes, context, shutdownEvent, observer);
            if (!moved)
            {
                return moved;
            }
        }
        return moveIfExists(root, master, archivePath(master, 1U), anchoredRoot, maximumBytes, context, shutdownEvent,
                            observer);
    }
    catch (...)
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Diagnostic log rotation could not allocate its bounded paths."));
    }
}

[[nodiscard]] Domain::Result<Detail::UniqueHandle> openAppendHandle(const std::wstring_view root,
                                                                    const std::wstring_view path,
                                                                    const HANDLE anchoredRoot) noexcept
{
    auto basename = diagnosticBasename(root, path);
    if (!basename)
    {
        return Domain::Result<Detail::UniqueHandle>::failure(std::move(basename).error());
    }
    Detail::RelativeOpenOptions options{};
    options.desiredAccess = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES;
    options.shareAccess = FILE_SHARE_READ;
    options.disposition = Detail::RelativeOpenDisposition::OpenOrCreate;
    options.objectType = Detail::RelativeObjectType::File;
    options.writeThrough = true;
    auto opened = Detail::openRelative(anchoredRoot, basename.value(), options);
    if (!opened)
    {
        return Domain::Result<Detail::UniqueHandle>::failure(
            diagnosticFileError("open the handle-relative bounded diagnostic log", opened.win32Error));
    }
    auto verified = verifyOpenedFile(opened.handle.get(), path);
    if (!verified)
    {
        return Domain::Result<Detail::UniqueHandle>::failure(std::move(verified).error());
    }
    return Domain::Result<Detail::UniqueHandle>::success(std::move(opened.handle));
}

[[nodiscard]] Domain::Result<std::uint64_t> openedFileSize(const HANDLE handle) noexcept
{
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0)
    {
        return Domain::Result<std::uint64_t>::failure(
            diagnosticFileError("recheck the bounded diagnostic log size", ::GetLastError()));
    }
    return Domain::Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
}

[[nodiscard]] bool appendWouldExceed(const std::uint64_t existingBytes, const std::size_t lineBytes,
                                     const std::size_t maximumBytes) noexcept
{
    return existingBytes > maximumBytes || existingBytes > maximumBytes - lineBytes;
}

[[nodiscard]] Domain::Result<void> appendJsonLine(const Domain::PathText &rootText,
                                                  const Domain::ResourceBudgets &budgets, const std::string &line,
                                                  const Domain::OperationContext &context, const HANDLE shutdownEvent,
                                                  Detail::IDiagnosticRotationPublishObserver *const observer) noexcept
{
    try
    {
        auto validBudgets = validateDiagnosticBudgets(budgets);
        if (!validBudgets)
        {
            return validBudgets;
        }
        if (line.size() > budgets.diagnosticLogFileBytesMaximum)
        {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "One diagnostic JSONL record exceeds the active-file budget."));
        }

        auto transaction = prepareDiagnosticTransaction(rootText, budgets, context, shutdownEvent);
        if (!transaction)
        {
            return Domain::Result<void>::failure(std::move(transaction).error());
        }

        auto transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return transactionReady;
        }

        auto pruned = pruneStaleArchives(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                                         budgets.diagnosticLogFilesMaximum,
                                         transaction.value().anchoredRoot.handles.back().get());
        if (!pruned)
        {
            return pruned;
        }
        transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return transactionReady;
        }

        auto existing = inspectRelativeFile(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                                            transaction.value().anchoredRoot.handles.back().get());
        if (!existing)
        {
            return Domain::Result<void>::failure(std::move(existing).error());
        }
        if (existing.value().exists &&
            appendWouldExceed(existing.value().bytes, line.size(), budgets.diagnosticLogFileBytesMaximum))
        {
            auto rotated =
                rotateLog(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                          budgets.diagnosticLogFilesMaximum, budgets.diagnosticLogFileBytesMaximum,
                          transaction.value().anchoredRoot.handles.back().get(), context, shutdownEvent, observer);
            if (!rotated)
            {
                return rotated;
            }
        }
        transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return transactionReady;
        }

        auto opened = openAppendHandle(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                                       transaction.value().anchoredRoot.handles.back().get());
        if (!opened)
        {
            return Domain::Result<void>::failure(std::move(opened).error());
        }
        auto measured = openedFileSize(opened.value().get());
        if (!measured)
        {
            return Domain::Result<void>::failure(std::move(measured).error());
        }
        if (appendWouldExceed(measured.value(), line.size(), budgets.diagnosticLogFileBytesMaximum))
        {
            opened.value().reset();
            auto rotated =
                rotateLog(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                          budgets.diagnosticLogFilesMaximum, budgets.diagnosticLogFileBytesMaximum,
                          transaction.value().anchoredRoot.handles.back().get(), context, shutdownEvent, observer);
            if (!rotated)
            {
                return rotated;
            }
            opened = openAppendHandle(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                                      transaction.value().anchoredRoot.handles.back().get());
            if (!opened)
            {
                return Domain::Result<void>::failure(std::move(opened).error());
            }
            measured = openedFileSize(opened.value().get());
            if (!measured)
            {
                return Domain::Result<void>::failure(std::move(measured).error());
            }
            if (appendWouldExceed(measured.value(), line.size(), budgets.diagnosticLogFileBytesMaximum))
            {
                return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Conflict,
                                                                       "The active diagnostic log changed "
                                                                       "outside the interprocess lock."));
            }
        }
        transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return transactionReady;
        }

        std::size_t offset = 0U;
        while (offset < line.size())
        {
            const auto remaining = line.size() - offset;
            const DWORD requested = static_cast<DWORD>(
                (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written{};
            if (::WriteFile(opened.value().get(), line.data() + offset, requested, &written, nullptr) == FALSE ||
                written == 0U)
            {
                return Domain::Result<void>::failure(
                    diagnosticFileError("append the bounded diagnostic log", ::GetLastError()));
            }
            offset += written;
        }
        if (::FlushFileBuffers(opened.value().get()) == FALSE)
        {
            return Domain::Result<void>::failure(
                diagnosticFileError("flush the bounded diagnostic log", ::GetLastError()));
        }

        // A completed flush is the durability linearization point. A
        // cancellation arriving after it must not turn success into failure.
        return Domain::Result<void>::success();
    }
    catch (...)
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "The bounded diagnostic log append failed."));
    }
}
[[nodiscard]] Domain::Error hostileDiagnosticError(std::string message)
{
    return Domain::makeError(Domain::ErrorCodes::IntegrityFailure, std::move(message));
}

[[nodiscard]] std::optional<int> decimalComponent(const std::string_view text, const std::size_t offset,
                                                  const std::size_t count) noexcept
{
    if (offset > text.size() || count > text.size() - offset)
    {
        return std::nullopt;
    }
    int value{};
    for (std::size_t index = offset; index < offset + count; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return std::nullopt;
        }
        value = value * 10 + (text[index] - '0');
    }
    return value;
}

[[nodiscard]] Domain::Result<Domain::UtcTimePoint> parseTimestamp(const std::string_view text) noexcept
{
    try
    {
        if (text.size() != 24U || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
            text[16] != ':' || text[19] != '.' || text[23] != 'Z')
        {
            return Domain::Result<Domain::UtcTimePoint>::failure(
                hostileDiagnosticError("A persisted diagnostic timestamp is not canonical UTC."));
        }
        const auto year = decimalComponent(text, 0U, 4U);
        const auto month = decimalComponent(text, 5U, 2U);
        const auto day = decimalComponent(text, 8U, 2U);
        const auto hour = decimalComponent(text, 11U, 2U);
        const auto minute = decimalComponent(text, 14U, 2U);
        const auto second = decimalComponent(text, 17U, 2U);
        const auto millisecond = decimalComponent(text, 20U, 3U);
        if (!year || !month || !day || !hour || !minute || !second || !millisecond || *year < 1970 || *month < 1 ||
            *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59)
        {
            return Domain::Result<Domain::UtcTimePoint>::failure(
                hostileDiagnosticError("A persisted diagnostic timestamp contains an "
                                       "invalid component."));
        }

        std::tm utc{};
        utc.tm_year = *year - 1900;
        utc.tm_mon = *month - 1;
        utc.tm_mday = *day;
        utc.tm_hour = *hour;
        utc.tm_min = *minute;
        utc.tm_sec = *second;
        const __time64_t seconds = ::_mkgmtime64(&utc);
        if (seconds < 0)
        {
            return Domain::Result<Domain::UtcTimePoint>::failure(
                hostileDiagnosticError("A persisted diagnostic timestamp is outside "
                                       "the supported UTC range."));
        }
        std::tm roundTrip{};
        if (::_gmtime64_s(&roundTrip, &seconds) != 0 || roundTrip.tm_year != utc.tm_year ||
            roundTrip.tm_mon != utc.tm_mon || roundTrip.tm_mday != utc.tm_mday || roundTrip.tm_hour != utc.tm_hour ||
            roundTrip.tm_min != utc.tm_min || roundTrip.tm_sec != utc.tm_sec)
        {
            return Domain::Result<Domain::UtcTimePoint>::failure(
                hostileDiagnosticError("A persisted diagnostic timestamp is not a "
                                       "real calendar instant."));
        }
        const auto timestamp = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(seconds)) +
                               std::chrono::milliseconds{*millisecond};
        if (timestampText(timestamp) != text)
        {
            return Domain::Result<Domain::UtcTimePoint>::failure(
                hostileDiagnosticError("A persisted diagnostic timestamp is not "
                                       "representable canonically."));
        }
        return Domain::Result<Domain::UtcTimePoint>::success(timestamp);
    }
    catch (...)
    {
        return Domain::Result<Domain::UtcTimePoint>::failure(
            hostileDiagnosticError("A persisted diagnostic timestamp could not be decoded."));
    }
}

[[nodiscard]] std::optional<Domain::DiagnosticSeverity> parseSeverity(const std::string_view value) noexcept
{
    if (value == "info")
    {
        return Domain::DiagnosticSeverity::Info;
    }
    if (value == "warn")
    {
        return Domain::DiagnosticSeverity::Warn;
    }
    if (value == "error")
    {
        return Domain::DiagnosticSeverity::Error;
    }
    if (value == "critical")
    {
        return Domain::DiagnosticSeverity::Critical;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Domain::DiagnosticCategory> parseCategory(const std::string_view value) noexcept
{
    if (value == "general")
        return Domain::DiagnosticCategory::General;
    if (value == "bootstrap")
        return Domain::DiagnosticCategory::Bootstrap;
    if (value == "telemetry")
        return Domain::DiagnosticCategory::Telemetry;
    if (value == "mcp")
        return Domain::DiagnosticCategory::Mcp;
    if (value == "lmstudio")
        return Domain::DiagnosticCategory::LmStudio;
    if (value == "manager")
        return Domain::DiagnosticCategory::Manager;
    if (value == "tools")
        return Domain::DiagnosticCategory::Tools;
    if (value == "agent")
        return Domain::DiagnosticCategory::Agent;
    if (value == "diagnostics")
        return Domain::DiagnosticCategory::Diagnostics;
    if (value == "ui")
        return Domain::DiagnosticCategory::Ui;
    return std::nullopt;
}

[[nodiscard]] Domain::Result<Domain::DiagnosticEnvelope> decodePersistedEnvelope(
    const std::string_view line, Contracts::IRedactor &redactor) noexcept
{
    try
    {
        std::vector<std::set<std::string, std::less<>>> objectKeys;
        Json document = Json::parse(
            line,
            [&objectKeys](const int, const Json::parse_event_t event, Json &parsed) {
                if (event == Json::parse_event_t::object_start)
                {
                    objectKeys.emplace_back();
                }
                else if (event == Json::parse_event_t::key)
                {
                    if (objectKeys.empty() || !objectKeys.back().insert(parsed.get<std::string>()).second)
                    {
                        throw std::runtime_error{"Duplicate persisted diagnostic JSON key."};
                    }
                }
                else if (event == Json::parse_event_t::object_end)
                {
                    if (objectKeys.empty())
                    {
                        throw std::runtime_error{"Unbalanced persisted diagnostic JSON object."};
                    }
                    objectKeys.pop_back();
                }
                return true;
            },
            true, false);
        if (!document.is_object())
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                hostileDiagnosticError("A persisted diagnostic record is not a JSON object."));
        }

        static const std::set<std::string, std::less<>> requiredKeys{"category", "event",    "pid",
                                                                     "role",     "severity", "ts"};
        static const std::set<std::string, std::less<>> allowedKeys{"category", "event",    "fields", "pid",
                                                                    "role",     "severity", "ts"};
        for (const auto &key : requiredKeys)
        {
            if (!document.contains(key))
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                    hostileDiagnosticError("A persisted diagnostic record is missing a required member."));
            }
        }
        for (auto iterator = document.begin(); iterator != document.end(); ++iterator)
        {
            if (!allowedKeys.contains(iterator.key()))
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                    hostileDiagnosticError("A persisted diagnostic record contains an unknown member."));
            }
        }
        if (!document["category"].is_string() || !document["event"].is_string() ||
            !document["pid"].is_number_unsigned() || !document["role"].is_string() ||
            !document["severity"].is_string() || !document["ts"].is_string() ||
            (document.contains("fields") && !document["fields"].is_object()))
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                hostileDiagnosticError("A persisted diagnostic record contains a "
                                       "member of the wrong type."));
        }

        const auto severity = parseSeverity(document["severity"].get_ref<const std::string &>());
        const auto category = parseCategory(document["category"].get_ref<const std::string &>());
        const auto timestamp = parseTimestamp(document["ts"].get_ref<const std::string &>());
        const auto processId = document["pid"].get<std::uint64_t>();
        if (!severity || !category || !timestamp || processId > (std::numeric_limits<std::uint32_t>::max)())
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                hostileDiagnosticError("A persisted diagnostic record contains an invalid enum, "
                                       "timestamp, or process id."));
        }

        std::vector<Domain::DiagnosticField> fields;
        if (document.contains("fields"))
        {
            if (document["fields"].size() > Domain::MaximumDiagnosticFieldCount)
            {
                return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                    hostileDiagnosticError("A persisted diagnostic record contains too many fields."));
            }
            fields.reserve(document["fields"].size());
            for (auto iterator = document["fields"].begin(); iterator != document["fields"].end(); ++iterator)
            {
                if (!iterator.value().is_string())
                {
                    return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                        hostileDiagnosticError("A persisted diagnostic field value is not a string."));
                }
                fields.push_back(Domain::DiagnosticField{iterator.key(), iterator.value().get<std::string>()});
            }
        }

        Domain::DiagnosticEnvelope envelope{timestamp.value(),
                                            document["event"].get<std::string>(),
                                            *severity,
                                            document["role"].get<std::string>(),
                                            static_cast<std::uint32_t>(processId),
                                            *category,
                                            std::move(fields)};
        auto valid = Domain::validateDiagnosticEnvelope(envelope);
        if (!valid)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                hostileDiagnosticError("A persisted diagnostic record violates its domain bounds."));
        }
        auto redacted = redactEnvelope(envelope, redactor);
        if (!redacted)
        {
            return Domain::Result<Domain::DiagnosticEnvelope>::failure(
                hostileDiagnosticError("A persisted diagnostic record failed mandatory re-redaction."));
        }
        return redacted;
    }
    catch (...)
    {
        return Domain::Result<Domain::DiagnosticEnvelope>::failure(
            hostileDiagnosticError("A persisted diagnostic JSONL record is malformed or ambiguous."));
    }
}
inline constexpr std::size_t MaximumPersistedExportRecords = 50'000U;
inline constexpr std::size_t MaximumPersistedJsonLineBytes = 16U * 1024U;

[[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> loadPersistedDiagnostics(
    const Domain::PathText &rootText, const Domain::ResourceBudgets &budgets, Contracts::IRedactor &redactor,
    const Domain::OperationContext &context, const HANDLE shutdownEvent) noexcept
{
    try
    {
        auto transaction = prepareDiagnosticTransaction(rootText, budgets, context, shutdownEvent);
        if (!transaction)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(transaction).error());
        }
        auto transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                std::move(transactionReady).error());
        }

        auto pruned = pruneStaleArchives(transaction.value().anchoredRoot.root, transaction.value().masterPath,
                                         budgets.diagnosticLogFilesMaximum,
                                         transaction.value().anchoredRoot.handles.back().get());
        if (!pruned)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(pruned).error());
        }

        transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
        if (!transactionReady)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                std::move(transactionReady).error());
        }

        auto basename = diagnosticBasename(transaction.value().anchoredRoot.root, transaction.value().masterPath);
        if (!basename)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(basename).error());
        }
        Detail::RelativeOpenOptions readOptions{};
        readOptions.desiredAccess = GENERIC_READ | FILE_READ_ATTRIBUTES;
        readOptions.shareAccess = FILE_SHARE_READ;
        readOptions.disposition = Detail::RelativeOpenDisposition::OpenExisting;
        readOptions.objectType = Detail::RelativeObjectType::File;
        readOptions.sequentialAccess = true;
        auto opened =
            Detail::openRelative(transaction.value().anchoredRoot.handles.back().get(), basename.value(), readOptions);
        if (!opened)
        {
            if (opened.win32Error == ERROR_FILE_NOT_FOUND || opened.win32Error == ERROR_PATH_NOT_FOUND)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success({});
            }
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                diagnosticFileError("open the handle-relative active diagnostic log for export", opened.win32Error));
        }
        auto verified = verifyOpenedFile(opened.handle.get(), transaction.value().masterPath);
        if (!verified)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(verified).error());
        }
        auto measured = openedFileSize(opened.handle.get());
        if (!measured)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(measured).error());
        }
        if (measured.value() > budgets.diagnosticLogFileBytesMaximum ||
            measured.value() > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()))
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                hostileDiagnosticError("The active diagnostic log changed beyond its byte bound."));
        }

        std::string content(static_cast<std::size_t>(measured.value()), '\0');
        std::size_t offset{};
        while (offset < content.size())
        {
            transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
            if (!transactionReady)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                    std::move(transactionReady).error());
            }
            const DWORD requested = static_cast<DWORD>(
                (std::min)(content.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read{};
            if (::ReadFile(opened.handle.get(), content.data() + offset, requested, &read, nullptr) == FALSE ||
                read == 0U)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                    diagnosticFileError("read the active diagnostic log for export", ::GetLastError()));
            }
            offset += read;
        }
        if (content.empty())
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success({});
        }
        if (content.back() != '\n' || content.find('\0') != std::string::npos ||
            content.find('\r') != std::string::npos)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                hostileDiagnosticError("The active diagnostic log is not canonical complete JSONL."));
        }

        std::deque<Domain::DiagnosticEnvelope> bounded;
        std::size_t lineStart{};
        while (lineStart < content.size())
        {
            transactionReady = validateDiagnosticTransactionStart(context, shutdownEvent);
            if (!transactionReady)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                    std::move(transactionReady).error());
            }
            const std::size_t lineEnd = content.find('\n', lineStart);
            if (lineEnd == std::string::npos || lineEnd == lineStart ||
                lineEnd - lineStart > MaximumPersistedJsonLineBytes)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                    hostileDiagnosticError("The active diagnostic log contains an "
                                           "empty or oversized JSONL record."));
            }
            auto decoded =
                decodePersistedEnvelope(std::string_view{content.data() + lineStart, lineEnd - lineStart}, redactor);
            if (!decoded)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(decoded).error());
            }
            if (bounded.size() >= MaximumPersistedExportRecords)
            {
                bounded.pop_front();
            }
            bounded.push_back(std::move(decoded).value());
            lineStart = lineEnd + 1U;
        }

        std::vector<Domain::DiagnosticEnvelope> output;
        output.reserve(bounded.size());
        while (!bounded.empty())
        {
            output.push_back(std::move(bounded.front()));
            bounded.pop_front();
        }
        return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(std::move(output));
    }
    catch (...)
    {
        return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Persisted diagnostics could not be loaded for export."));
    }
}

[[nodiscard]] bool diagnosticFieldsLess(const std::vector<Domain::DiagnosticField> &left,
                                        const std::vector<Domain::DiagnosticField> &right) noexcept
{
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [](const Domain::DiagnosticField &first, const Domain::DiagnosticField &second) noexcept {
            return std::tie(first.name, first.value) < std::tie(second.name, second.value);
        });
}

[[nodiscard]] bool diagnosticEnvelopeLess(const Domain::DiagnosticEnvelope &left,
                                          const Domain::DiagnosticEnvelope &right) noexcept
{
    const auto leftPrefix =
        std::tie(left.timestamp, left.event, left.processId, left.role, left.severity, left.category);
    const auto rightPrefix =
        std::tie(right.timestamp, right.event, right.processId, right.role, right.severity, right.category);
    if (leftPrefix < rightPrefix)
    {
        return true;
    }
    if (rightPrefix < leftPrefix)
    {
        return false;
    }
    return diagnosticFieldsLess(left.fields, right.fields);
}

[[nodiscard]] bool sameDiagnosticEnvelope(const Domain::DiagnosticEnvelope &left,
                                          const Domain::DiagnosticEnvelope &right) noexcept
{
    return !diagnosticEnvelopeLess(left, right) && !diagnosticEnvelopeLess(right, left);
}

[[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> mergeDiagnostics(
    std::vector<Domain::DiagnosticEnvelope> persisted, const std::deque<RetainedDiagnostic> &retained,
    Contracts::IRedactor &redactor) noexcept
{
    try
    {
        persisted.reserve(persisted.size() + retained.size());
        for (const auto &item : retained)
        {
            auto redacted = redactEnvelope(item.envelope, redactor);
            if (!redacted)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(redacted).error());
            }
            persisted.push_back(std::move(redacted).value());
        }
        std::sort(persisted.begin(), persisted.end(), diagnosticEnvelopeLess);
        persisted.erase(std::unique(persisted.begin(), persisted.end(), sameDiagnosticEnvelope), persisted.end());
        return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(std::move(persisted));
    }
    catch (...)
    {
        return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Persisted and in-memory diagnostics could not be merged."));
    }
}
struct ExportDocuments final
{
    std::string json;
    std::string markdown;
};

struct ExportPathPlan final
{
    Domain::PathText directory;
    Domain::PathText json;
    Domain::PathText markdown;
    std::wstring jsonName;
    std::wstring markdownName;
};

struct AuthorizedReplacementCandidates final
{
    std::optional<Contracts::AuthorizedPath> create;
    std::optional<Contracts::AuthorizedPath> write;
};

[[nodiscard]] Domain::Result<ExportPathPlan> planExportPaths(const Domain::DiagnosticExportRequest &request,
                                                             const Domain::PathText &exportRoot,
                                                             const Domain::UtcTimePoint exportedAt) noexcept
{
    try
    {
        const Domain::PathText directory = request.directory.has_value() ? request.directory.value() : exportRoot;
        std::string basename;
        if (request.basename.has_value())
        {
            basename = request.basename.value();
        }
        else
        {
            const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(exportedAt.time_since_epoch()).count();
            basename = "forge-diagnostics-" + std::to_string(milliseconds);
        }
        if (!validBasename(basename))
        {
            return Domain::Result<ExportPathPlan>::failure(
                Domain::makeError(request.basename.has_value() &&
                                          request.basename->size() > WindowsDiagnosticSink::MaximumExportBasenameBytes
                                      ? Domain::ErrorCodes::PayloadTooLarge
                                      : Domain::ErrorCodes::InvalidRequest,
                                  "Diagnostic export basename must be 1-128 ASCII letters, digits, "
                                  "hyphens, or "
                                  "underscores."));
        }

        const auto childPath = [&directory](const std::string_view name) {
            std::string value = directory.value();
            if (!value.ends_with('\\') && !value.ends_with('/'))
            {
                value.push_back('\\');
            }
            value.append(name);
            return Domain::PathText::create(value);
        };
        auto json = childPath(basename + ".json");
        if (!json)
        {
            return Domain::Result<ExportPathPlan>::failure(std::move(json).error());
        }
        auto markdown = childPath(basename + ".md");
        if (!markdown)
        {
            return Domain::Result<ExportPathPlan>::failure(std::move(markdown).error());
        }
        auto basenameUtf16 = Detail::strictUtf8ToUtf16(basename);
        if (!basenameUtf16)
        {
            return Domain::Result<ExportPathPlan>::failure(std::move(basenameUtf16).error());
        }
        return Domain::Result<ExportPathPlan>::success(
            ExportPathPlan{directory, std::move(json).value(), std::move(markdown).value(),
                           basenameUtf16.value() + L".json", basenameUtf16.value() + L".md"});
    }
    catch (...)
    {
        return Domain::Result<ExportPathPlan>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Diagnostic export paths could not be planned."));
    }
}

struct ExportPaths final
{
    Domain::PathText directory;
    Domain::PathText json;
    Domain::PathText markdown;
    std::wstring nativeJson;
    std::wstring nativeMarkdown;
};

[[nodiscard]] Domain::Result<std::wstring> resolveExistingDirectory(const Domain::PathText &directory) noexcept
{
    auto resolved = Detail::WindowsPathResolver::resolveAppOwnedRoot(directory.value());
    if (!resolved)
    {
        return resolved;
    }
    const auto native = extendedPath(resolved.value());
    const DWORD attributes = ::GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD nativeError = ::GetLastError();
        if (nativeError == ERROR_FILE_NOT_FOUND || nativeError == ERROR_PATH_NOT_FOUND)
        {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::RecordNotFound, "The diagnostic export directory does not exist."));
        }
        return Domain::Result<std::wstring>::failure(
            diagnosticFileError("inspect the diagnostic export directory", nativeError));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return Domain::Result<std::wstring>::failure(
            Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority, "The diagnostic export directory is unsafe."));
    }
    return resolved;
}

[[nodiscard]] Domain::Result<ExportPaths> resolveExportPaths(const ExportPathPlan &plan,
                                                             const bool requireExistingDirectory,
                                                             const Domain::OperationContext &context,
                                                             const HANDLE shutdownEvent) noexcept
{
    try
    {
        Domain::Result<std::wstring> nativeDirectory =
            requireExistingDirectory ? resolveExistingDirectory(plan.directory)
                                     : ensureDiagnosticsRoot(plan.directory, context, shutdownEvent);
        if (!nativeDirectory)
        {
            return Domain::Result<ExportPaths>::failure(std::move(nativeDirectory).error());
        }
        auto canonicalDirectory = Detail::WindowsPathResolver::toPathText(nativeDirectory.value());
        if (!canonicalDirectory)
        {
            return Domain::Result<ExportPaths>::failure(std::move(canonicalDirectory).error());
        }
        if (canonicalDirectory.value() != plan.directory)
        {
            return Domain::Result<ExportPaths>::failure(Domain::makeError(Domain::ErrorCodes::PathOutsideAuthority,
                                                                          "The authorized diagnostic export directory "
                                                                          "changed during canonical resolution."));
        }

        auto nativeJson = Detail::WindowsPathResolver::resolveAppOwnedChild(nativeDirectory.value(), plan.jsonName,
                                                                            Detail::MissingPathPolicy::AllowLeaf);
        if (!nativeJson)
        {
            return Domain::Result<ExportPaths>::failure(std::move(nativeJson).error());
        }
        auto nativeMarkdown = Detail::WindowsPathResolver::resolveAppOwnedChild(
            nativeDirectory.value(), plan.markdownName, Detail::MissingPathPolicy::AllowLeaf);
        if (!nativeMarkdown)
        {
            return Domain::Result<ExportPaths>::failure(std::move(nativeMarkdown).error());
        }
        auto jsonPath = Detail::WindowsPathResolver::toPathText(nativeJson.value());
        if (!jsonPath)
        {
            return Domain::Result<ExportPaths>::failure(std::move(jsonPath).error());
        }
        auto markdownPath = Detail::WindowsPathResolver::toPathText(nativeMarkdown.value());
        if (!markdownPath)
        {
            return Domain::Result<ExportPaths>::failure(std::move(markdownPath).error());
        }
        if (jsonPath.value() != plan.json || markdownPath.value() != plan.markdown)
        {
            return Domain::Result<ExportPaths>::failure(Domain::makeError(
                Domain::ErrorCodes::PathOutsideAuthority, "A diagnostic export artifact changed after authorization."));
        }
        return Domain::Result<ExportPaths>::success(ExportPaths{plan.directory, plan.json, plan.markdown,
                                                                std::move(nativeJson).value(),
                                                                std::move(nativeMarkdown).value()});
    }
    catch (...)
    {
        return Domain::Result<ExportPaths>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Diagnostic export paths could not be resolved."));
    }
}

[[nodiscard]] Domain::Result<ExportDocuments> buildExportDocuments(
    const std::vector<Domain::DiagnosticEnvelope> &diagnostics, const Domain::UtcTimePoint exportedAt) noexcept
{
    try
    {
        Json records = Json::array();
        std::map<std::string, std::size_t, std::less<>> severityCounts;
        std::map<std::string, std::size_t, std::less<>> categoryCounts;
        for (const auto &envelope : diagnostics)
        {
            records.push_back(jsonForEnvelope(envelope));
            ++severityCounts[severityName(envelope.severity)];
            ++categoryCounts[categoryName(envelope.category)];
        }

        Json document{{"exported_at", timestampText(exportedAt)},
                      {"product", "Forge Conductor"},
                      {"record_count", diagnostics.size()},
                      {"records", std::move(records)},
                      {"schema_version", 1}};
        std::string json = document.dump(2, ' ', false, Json::error_handler_t::strict);
        json.push_back('\n');

        std::string markdown{"# Forge Conductor Diagnostic Export\n\n"};
        markdown += "- Exported: ";
        markdown += timestampText(exportedAt);
        markdown += "\n- Records: ";
        markdown += std::to_string(diagnostics.size());
        markdown += "\n\n## Summary by severity\n\n";
        for (const auto &[name, count] : severityCounts)
        {
            markdown += "- ";
            markdown += name;
            markdown += ": ";
            markdown += std::to_string(count);
            markdown.push_back('\n');
        }
        markdown += "\n## Summary by category\n\n";
        for (const auto &[name, count] : categoryCounts)
        {
            markdown += "- ";
            markdown += name;
            markdown += ": ";
            markdown += std::to_string(count);
            markdown.push_back('\n');
        }
        markdown += "\n## Timeline\n\n"
                    "| Time (UTC) | Severity | Category | Event | Fields |\n"
                    "|---|---|---|---|---|\n";
        for (const auto &envelope : diagnostics)
        {
            std::map<std::string, std::string, std::less<>> sortedFields;
            for (const auto &field : envelope.fields)
            {
                sortedFields.emplace(field.name, field.value);
            }
            std::string fields;
            for (const auto &[name, value] : sortedFields)
            {
                if (!fields.empty())
                {
                    fields.append("; ");
                }
                fields.append(name);
                fields.push_back('=');
                fields.append(value);
            }
            markdown += "| ";
            markdown += timestampText(envelope.timestamp);
            markdown += " | ";
            markdown += severityName(envelope.severity);
            markdown += " | ";
            markdown += categoryName(envelope.category);
            markdown += " | ";
            markdown += markdownCell(envelope.event);
            markdown += " | ";
            markdown += markdownCell(std::move(fields));
            markdown += " |\n";
        }
        markdown += "\n_End of export._\n";

        if (json.size() > Contracts::IAtomicFileStore::MaximumBytes ||
            markdown.size() > Contracts::IAtomicFileStore::MaximumBytes)
        {
            return Domain::Result<ExportDocuments>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Diagnostic export exceeds the 32 MiB atomic-file cap."));
        }
        return Domain::Result<ExportDocuments>::success(ExportDocuments{std::move(json), std::move(markdown)});
    }
    catch (...)
    {
        return Domain::Result<ExportDocuments>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure, "Diagnostic export encoding failed."));
    }
}
} // namespace

Domain::Result<Detail::AnchoredDiagnosticDirectoryTree> Detail::prepareAnchoredDiagnosticDirectory(
    const Domain::PathText &rootText, const Domain::OperationContext &context, const HANDLE shutdownEvent,
    IDiagnosticDirectoryAnchorObserver *const observer) noexcept
{
    std::vector<Detail::UniqueHandle> handles;
    auto root = ensureDiagnosticsRoot(rootText, context, shutdownEvent, observer, &handles);
    if (!root)
    {
        return Domain::Result<AnchoredDiagnosticDirectoryTree>::failure(std::move(root).error());
    }
    return Domain::Result<AnchoredDiagnosticDirectoryTree>::success(
        AnchoredDiagnosticDirectoryTree{std::move(root).value(), std::move(handles)});
}

class WindowsDiagnosticSink::Impl final
{
  public:
    Impl(WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
         std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
         std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
         std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
         std::shared_ptr<Detail::IDiagnosticRotationPublishObserver> rotationPublishObserver)
        : diagnosticsRoot_{std::move(options.diagnosticsRoot)}, exportRoot_{std::move(options.exportRoot)},
          budgets_{std::move(options.budgets)}, clock_{std::move(clock)}, redactor_{std::move(redactor)},
          hasher_{std::move(hasher)}, workspaceAuthority_{std::move(workspaceAuthority)},
          atomicFileStore_{std::move(atomicFileStore)}, rotationPublishObserver_{std::move(rotationPublishObserver)},
          shutdownEvent_{::CreateEventW(nullptr, TRUE, FALSE, nullptr)}
    {
        if (!clock_ || !redactor_ || !hasher_ || !workspaceAuthority_ || !atomicFileStore_)
        {
            throw std::invalid_argument{"The diagnostic sink requires owned service dependencies."};
        }
        if (!shutdownEvent_)
        {
            throw std::runtime_error{"The diagnostic sink shutdown event could not be created."};
        }
        if (options.enableEtw)
        {
            etw_ = std::make_unique<Detail::EtwProvider>();
        }
    }
    ~Impl() noexcept
    {
        shutdown();
        // The final shared owner exists only after every public call releases
        // its local Impl reference, so this proof wait is already idle.
        executor_.shutdown();
        if (etw_)
            etw_->shutdown();
    }

    [[nodiscard]] Domain::Result<void> record(const Domain::DiagnosticEnvelope &event,
                                              const Domain::OperationContext &context) noexcept
    {
        try
        {
            auto lease = executor_.acquire(context, "Diagnostic record");
            if (!lease)
            {
                return Domain::Result<void>::failure(std::move(lease).error());
            }
            auto contextValid =
                Detail::validateOperationContext(context, clock_->monotonicNow(), "record the diagnostic event");
            if (!contextValid)
            {
                return contextValid;
            }
            auto redacted = redactEnvelope(event, *redactor_);
            if (!redacted)
            {
                return Domain::Result<void>::failure(std::move(redacted).error());
            }
            auto line = encodeJsonLine(redacted.value());
            if (!line)
            {
                return Domain::Result<void>::failure(std::move(line).error());
            }
            if (line.value().size() > budgets_.diagnosticLogFileBytesMaximum)
            {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge, "The redacted diagnostic event exceeds the active log cap."));
            }

            const std::size_t encodedBytes = line.value().size();
            // The deque insertion is the only potentially allocating ring
            // mutation. Stage it while the serialized lease makes the ring
            // unobservable, then roll it back if durable append fails.
            retained_.push_back(RetainedDiagnostic{std::move(redacted).value(), encodedBytes});

            auto persisted = appendJsonLine(diagnosticsRoot_, budgets_, line.value(), context, shutdownEvent_.get(),
                                            rotationPublishObserver_.get());
            if (!persisted)
            {
                retained_.pop_back();
                return persisted;
            }

            // Nothing below this durability point allocates or can turn the
            // committed append into an observable failure.
            ringBytes_ += encodedBytes;
            while (!retained_.empty() && (retained_.size() > WindowsDiagnosticSink::MaximumRetainedRecords ||
                                          ringBytes_ > budgets_.diagnosticLogFileBytesMaximum))
            {
                ringBytes_ -= retained_.front().encodedBytes;
                retained_.pop_front();
            }

            if (etw_)
            {
                (void)etw_->write(retained_.back().envelope);
            }
            return Domain::Result<void>::success();
        }
        catch (...)
        {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, "The diagnostic event could not be retained."));
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::DiagnosticEnvelope>> recent(
        const std::size_t maximumCount, const Domain::OperationContext &context) noexcept
    {
        try
        {
            auto lease = executor_.acquire(context, "Diagnostic recent read");
            if (!lease)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(std::move(lease).error());
            }
            auto contextValid =
                Detail::validateOperationContext(context, clock_->monotonicNow(), "read recent diagnostics");
            if (!contextValid)
            {
                return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                    std::move(contextValid).error());
            }
            const std::size_t count = (std::min)(maximumCount, retained_.size());
            std::vector<Domain::DiagnosticEnvelope> output;
            output.reserve(count);
            const std::size_t first = retained_.size() - count;
            for (std::size_t index = first; index < retained_.size(); ++index)
            {
                output.push_back(retained_[index].envelope);
            }
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::success(std::move(output));
        }
        catch (...)
        {
            return Domain::Result<std::vector<Domain::DiagnosticEnvelope>>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, "Recent diagnostics could not be copied."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::DiagnosticExportResult> exportData(
        const Domain::DiagnosticExportRequest &request, const Contracts::WorkspaceAuthority &authority,
        const Domain::OperationContext &context) noexcept
    {
        try
        {
            auto lease = executor_.acquire(context, "Diagnostic export");
            if (!lease)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(lease).error());
            }
            auto contextValid = Detail::validateOperationContext(context, clock_->monotonicNow(), "export diagnostics");
            if (!contextValid)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(contextValid).error());
            }

            const auto exportedAt = clock_->utcNow();
            auto plan = planExportPaths(request, exportRoot_, exportedAt);
            if (!plan)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(plan).error());
            }

            // Both artifacts are authorized before directory resolution,
            // existence probes, log pruning, or export construction.
            auto jsonCandidates =
                preauthorizeReplacement(plan.value().json, plan.value().directory, authority, context);
            if (!jsonCandidates)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(jsonCandidates).error());
            }
            auto markdownCandidates =
                preauthorizeReplacement(plan.value().markdown, plan.value().directory, authority, context);
            if (!markdownCandidates)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(markdownCandidates).error());
            }

            auto paths = resolveExportPaths(plan.value(), request.directory.has_value(), context, shutdownEvent_.get());
            if (!paths)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(paths).error());
            }
            auto authorizedJson =
                selectReplacementAuthorization(std::move(jsonCandidates).value(), paths.value().nativeJson);
            if (!authorizedJson)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(authorizedJson).error());
            }
            auto authorizedMarkdown =
                selectReplacementAuthorization(std::move(markdownCandidates).value(), paths.value().nativeMarkdown);
            if (!authorizedMarkdown)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(authorizedMarkdown).error());
            }

            auto persisted =
                loadPersistedDiagnostics(diagnosticsRoot_, budgets_, *redactor_, context, shutdownEvent_.get());
            if (!persisted)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(persisted).error());
            }
            auto merged = mergeDiagnostics(std::move(persisted).value(), retained_, *redactor_);
            if (!merged)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(merged).error());
            }
            auto documents = buildExportDocuments(merged.value(), exportedAt);
            if (!documents)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(documents).error());
            }
            auto jsonBytes = bytesFor(documents.value().json);
            auto markdownBytes = bytesFor(documents.value().markdown);
            auto jsonDigest = hasher_->sha256(std::span<const std::byte>{jsonBytes});
            if (!jsonDigest)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(jsonDigest).error());
            }
            auto markdownDigest = hasher_->sha256(std::span<const std::byte>{markdownBytes});
            if (!markdownDigest)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(markdownDigest).error());
            }

            auto wroteMarkdown = atomicFileStore_->replace(authorizedMarkdown.value(),
                                                           std::span<const std::byte>{markdownBytes}, false, context);
            if (!wroteMarkdown)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(wroteMarkdown).error());
            }
            auto wroteJson = atomicFileStore_->replace(authorizedJson.value(), std::span<const std::byte>{jsonBytes},
                                                       false, context);
            if (!wroteJson)
            {
                return Domain::Result<Domain::DiagnosticExportResult>::failure(std::move(wroteJson).error());
            }

            return Domain::Result<Domain::DiagnosticExportResult>::success(Domain::DiagnosticExportResult{
                paths.value().json, merged.value().size(), jsonBytes.size(), std::move(jsonDigest).value(),
                paths.value().markdown, std::move(markdownDigest).value()});
        }
        catch (...)
        {
            return Domain::Result<Domain::DiagnosticExportResult>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, "The diagnostic export failed."));
        }
    }

    void shutdown() noexcept
    {
        const bool firstRequest = !shutdownRequested_.exchange(true, std::memory_order_acq_rel);
        if (firstRequest && shutdownEvent_)
        {
            static_cast<void>(::SetEvent(shutdownEvent_.get()));
        }
        executor_.beginShutdown();
        if (executor_.waitUntilIdle(DiagnosticShutdownDrainTimeout) && etw_)
        {
            etw_->shutdown();
        }
    }

  private:
    [[nodiscard]] Domain::Result<AuthorizedReplacementCandidates> preauthorizeReplacement(
        const Domain::PathText &path, const Domain::PathText &directory, const Contracts::WorkspaceAuthority &authority,
        const Domain::OperationContext &context) noexcept
    {
        AuthorizedReplacementCandidates candidates;
        auto create = workspaceAuthority_->authorize(
            authority, Domain::PathAuthorizationRequest{path, directory, Domain::FileAccess::Create, false}, context);
        if (create)
        {
            candidates.create.emplace(std::move(create).value());
        }
        else if (create.error().code != Domain::ErrorCodes::Unauthorized)
        {
            return Domain::Result<AuthorizedReplacementCandidates>::failure(std::move(create).error());
        }

        auto write = workspaceAuthority_->authorize(
            authority, Domain::PathAuthorizationRequest{path, directory, Domain::FileAccess::Write, false}, context);
        if (write)
        {
            candidates.write.emplace(std::move(write).value());
        }
        else if (write.error().code != Domain::ErrorCodes::Unauthorized)
        {
            return Domain::Result<AuthorizedReplacementCandidates>::failure(std::move(write).error());
        }
        if (!candidates.create.has_value() && !candidates.write.has_value())
        {
            return Domain::Result<AuthorizedReplacementCandidates>::failure(
                Domain::makeError(Domain::ErrorCodes::Unauthorized, "Diagnostic export has neither Create nor Write "
                                                                    "authority for the artifact."));
        }
        return Domain::Result<AuthorizedReplacementCandidates>::success(std::move(candidates));
    }

    [[nodiscard]] static Domain::Result<Contracts::AuthorizedPath> selectReplacementAuthorization(
        AuthorizedReplacementCandidates candidates, const std::wstring_view nativePath) noexcept
    {
        auto existing = inspectFile(nativePath);
        if (!existing)
        {
            return Domain::Result<Contracts::AuthorizedPath>::failure(std::move(existing).error());
        }
        auto &selected = existing.value().exists ? candidates.write : candidates.create;
        if (!selected.has_value())
        {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(Domain::ErrorCodes::Unauthorized, existing.value().exists
                                                                        ? "Diagnostic export requires Write authority "
                                                                          "for an existing artifact."
                                                                        : "Diagnostic export requires Create authority "
                                                                          "for a new artifact."));
        }
        return Domain::Result<Contracts::AuthorizedPath>::success(std::move(selected).value());
    }

    const Domain::PathText diagnosticsRoot_;
    const Domain::PathText exportRoot_;
    const Domain::ResourceBudgets budgets_;
    const std::shared_ptr<Contracts::IClock> clock_;
    const std::shared_ptr<Contracts::IRedactor> redactor_;
    const std::shared_ptr<Contracts::IHasher> hasher_;
    const std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority_;
    const std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore_;
    const std::shared_ptr<Detail::IDiagnosticRotationPublishObserver> rotationPublishObserver_;
    std::atomic<bool> shutdownRequested_{};
    Detail::UniqueHandle shutdownEvent_;
    Detail::BoundedSerialExecutor executor_;
    std::deque<RetainedDiagnostic> retained_;
    std::size_t ringBytes_{};
    std::unique_ptr<Detail::EtwProvider> etw_;
};

WindowsDiagnosticSink::WindowsDiagnosticSink(WindowsDiagnosticSinkOptions options,
                                             std::shared_ptr<Contracts::IClock> clock,
                                             std::shared_ptr<Contracts::IRedactor> redactor,
                                             std::shared_ptr<Contracts::IHasher> hasher,
                                             std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
                                             std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore)
    : WindowsDiagnosticSink(std::move(options), std::move(clock), std::move(redactor), std::move(hasher),
                            std::move(workspaceAuthority), std::move(atomicFileStore), {})
{
}

WindowsDiagnosticSink::WindowsDiagnosticSink(
    WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
    std::shared_ptr<Detail::IDiagnosticRotationPublishObserver> rotationPublishObserver)
    : implementation_{std::make_shared<Impl>(std::move(options), std::move(clock), std::move(redactor),
                                             std::move(hasher), std::move(workspaceAuthority),
                                             std::move(atomicFileStore), std::move(rotationPublishObserver))}
{
}

std::unique_ptr<WindowsDiagnosticSink> Detail::WindowsDiagnosticSinkTestAccess::create(
    WindowsDiagnosticSinkOptions options, std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IRedactor> redactor, std::shared_ptr<Contracts::IHasher> hasher,
    std::shared_ptr<Contracts::IWorkspaceAuthority> workspaceAuthority,
    std::shared_ptr<Contracts::IAtomicFileStore> atomicFileStore,
    std::shared_ptr<IDiagnosticRotationPublishObserver> rotationPublishObserver)
{
    return std::unique_ptr<WindowsDiagnosticSink>{new WindowsDiagnosticSink{
        std::move(options), std::move(clock), std::move(redactor), std::move(hasher), std::move(workspaceAuthority),
        std::move(atomicFileStore), std::move(rotationPublishObserver)}};
}

WindowsDiagnosticSink::~WindowsDiagnosticSink() noexcept
{
    auto implementation = std::move(implementation_);
    if (implementation)
    {
        implementation->shutdown();
    }
}

Domain::Result<void> WindowsDiagnosticSink::record(const Domain::DiagnosticEnvelope &event,
                                                   const Domain::OperationContext &context) noexcept
{
    const auto implementation = implementation_;
    return implementation->record(event, context);
}

Domain::Result<std::vector<Domain::DiagnosticEnvelope>> WindowsDiagnosticSink::recent(
    const std::size_t maximumCount, const Domain::OperationContext &context) noexcept
{
    const auto implementation = implementation_;
    return implementation->recent(maximumCount, context);
}

Domain::Result<Domain::DiagnosticExportResult> WindowsDiagnosticSink::exportData(
    const Domain::DiagnosticExportRequest &request, const Contracts::WorkspaceAuthority &authority,
    const Domain::OperationContext &context) noexcept
{
    const auto implementation = implementation_;
    return implementation->exportData(request, authority, context);
}

void WindowsDiagnosticSink::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation)
    {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
