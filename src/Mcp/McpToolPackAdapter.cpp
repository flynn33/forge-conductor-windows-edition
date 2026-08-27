#include "ForgeConductor/Mcp/McpToolPackAdapter.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Mcp/McpJsonCodec.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

using Json = nlohmann::json;

constexpr std::size_t MaximumTextFileBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumDirectoryEntries = 1'000U;
constexpr std::size_t MaximumGlobMatches = 500U;
constexpr std::size_t MaximumSearchMatches = 200U;
constexpr std::size_t MaximumNativeResponseBytes = 2U * 1024U * 1024U;
constexpr std::size_t MaximumGitOutputBytes = 80'000U;
constexpr std::size_t MaximumGitLogEntries = 200U;
constexpr std::size_t MaximumShellOutputBytes = 80'000U;
constexpr std::size_t MaximumShellErrorBytes = 20'000U;
constexpr std::size_t MaximumMcpTextContentBytes = 96U * 1024U;
constexpr std::int64_t DefaultReadWindowLines = 200;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagate(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

[[nodiscard]] Domain::Result<void> invalid(std::string message)
{
    return Domain::Result<void>::failure(Domain::makeError(
        Domain::ErrorCodes::InvalidRequest, std::move(message), true));
}

[[nodiscard]] const Json* member(const Json& object, const std::string_view key)
{
    const auto found = object.find(std::string{key});
    return found == object.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::string> strictString(
    const Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null() || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

[[nodiscard]] std::optional<std::string> legacyString(
    const Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (value->is_string()) {
        return value->get<std::string>();
    }
    if (value->is_number_unsigned()) {
        return std::to_string(value->get<std::uint64_t>());
    }
    if (value->is_number_integer()) {
        return std::to_string(value->get<std::int64_t>());
    }
    if (value->is_number_float()) {
        return value->dump();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> strictInteger(
    const Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (value->is_number_unsigned()) {
        const auto encoded = value->get<std::uint64_t>();
        if (encoded <= static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return static_cast<std::int64_t>(encoded);
        }
    }
    if (value->is_number_integer()) {
        return value->get<std::int64_t>();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> legacyInteger(
    const Json& object,
    const std::string_view key)
{
    if (const auto strict = strictInteger(object, key)) {
        return strict;
    }
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null() || value->is_boolean()) {
        return std::nullopt;
    }
    if (value->is_number_float()) {
        const double number = value->get<double>();
        if (std::isfinite(number) && std::floor(number) == number &&
            number >= static_cast<double>(
                (std::numeric_limits<std::int64_t>::min)()) &&
            number <= static_cast<double>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return static_cast<std::int64_t>(number);
        }
        return std::nullopt;
    }
    if (value->is_string()) {
        const auto& encoded = value->get_ref<const std::string&>();
        std::int64_t parsed{};
        const auto result = std::from_chars(
            encoded.data(), encoded.data() + encoded.size(), parsed);
        if (result.ec == std::errc{} &&
            result.ptr == encoded.data() + encoded.size()) {
            return parsed;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> strictNumber(
    const Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null() || !value->is_number()) {
        return std::nullopt;
    }
    const double number = value->get<double>();
    return std::isfinite(number) ? std::optional<double>{number} : std::nullopt;
}

[[nodiscard]] std::optional<bool> strictBoolean(
    const Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr || value->is_null() || !value->is_boolean()) {
        return std::nullopt;
    }
    return value->get<bool>();
}

[[nodiscard]] Domain::Result<std::vector<std::string>> strictStrings(
    const Json& object,
    const std::string_view key)
{
    try {
        const auto* value = member(object, key);
        if (value == nullptr || value->is_null()) {
            return Domain::Result<std::vector<std::string>>::success({});
        }
        if (!value->is_array()) {
            return failure<std::vector<std::string>>(
                Domain::ErrorCodes::InvalidRequest,
                std::string{key} + " must be an array of strings.");
        }
        std::vector<std::string> result;
        result.reserve(value->size());
        for (const auto& item : *value) {
            if (!item.is_string()) {
                return failure<std::vector<std::string>>(
                    Domain::ErrorCodes::InvalidRequest,
                    std::string{key} + " must contain only strings.");
            }
            result.push_back(item.get<std::string>());
        }
        return Domain::Result<std::vector<std::string>>::success(
            std::move(result));
    } catch (...) {
        return failure<std::vector<std::string>>(
            Domain::ErrorCodes::InternalFailure,
            "A bounded string array could not be decoded.");
    }
}

[[nodiscard]] std::vector<std::string> legacyStrings(
    const Json& object,
    const std::string_view key)
{
    try {
        const auto* value = member(object, key);
        if (value == nullptr || value->is_null()) {
            return {};
        }
        if (value->is_string()) {
            return {value->get<std::string>()};
        }
        if (!value->is_array()) {
            return {};
        }
        std::vector<std::string> result;
        result.reserve(value->size());
        for (const auto& item : *value) {
            if (item.is_string()) {
                result.push_back(item.get<std::string>());
            }
        }
        return result;
    } catch (...) {
        return {};
    }
}

[[nodiscard]] std::string trimLegacyScalar(std::string value)
{
    const auto whitespace = [](const unsigned char character) {
        return std::isspace(character) != 0;
    };
    const auto first = std::find_if_not(
        value.begin(), value.end(), [&](const char character) {
            return whitespace(static_cast<unsigned char>(character));
        });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), [&](const char character) {
            return whitespace(static_cast<unsigned char>(character));
        }).base();
    if (first >= last) {
        return {};
    }
    return std::string{first, last};
}

void normalizeLegacyStringField(Json& object, const std::string_view key)
{
    if (member(object, key) == nullptr) {
        return;
    }
    const auto value = legacyString(object, key);
    if (!value) {
        object.erase(std::string{key});
        return;
    }
    object[std::string{key}] = trimLegacyScalar(*value);
}

void normalizeLegacyStringFields(
    Json& object,
    const std::initializer_list<std::string_view> keys)
{
    for (const auto key : keys) {
        normalizeLegacyStringField(object, key);
    }
}

void normalizeLegacyIntegerField(Json& object, const std::string_view key)
{
    if (member(object, key) == nullptr) {
        return;
    }
    const auto value = legacyInteger(object, key);
    if (!value) {
        object.erase(std::string{key});
        return;
    }
    object[std::string{key}] = *value;
}

void normalizeLegacyNumberField(Json& object, const std::string_view key)
{
    if (member(object, key) != nullptr && !strictNumber(object, key)) {
        object.erase(std::string{key});
    }
}

void normalizeLegacyBooleanDefault(Json& object, const std::string_view key)
{
    if (member(object, key) != nullptr && !strictBoolean(object, key)) {
        object.erase(std::string{key});
    }
}

void normalizeProjectMemoryStringCollection(
    Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr) {
        return;
    }
    Json normalized = Json::array();
    if (value->is_string()) {
        normalized.push_back(value->get<std::string>());
    } else if (value->is_array()) {
        for (const auto& item : *value) {
            if (item.is_string()) {
                normalized.push_back(item);
            }
        }
    }
    object[std::string{key}] = std::move(normalized);
}

void normalizeContinuityStringCollection(
    Json& object,
    const std::string_view key)
{
    const auto* value = member(object, key);
    if (value == nullptr) {
        return;
    }
    Json normalized = Json::array();
    if (value->is_array()) {
        for (const auto& item : *value) {
            if (item.is_string()) {
                normalized.push_back(item);
            }
        }
    }
    object[std::string{key}] = std::move(normalized);
}

void normalizeProjectMemoryWrite(Json& write)
{
    if (!write.is_object()) {
        return;
    }
    normalizeLegacyStringFields(
        write,
        {"kind",
         "title",
         "summary",
         "body",
         "source_kind",
         "source_reference",
         "session_id",
         "expires_at",
         "idempotency_key"});
    normalizeProjectMemoryStringCollection(write, "tags");
    normalizeProjectMemoryStringCollection(write, "related_ids");
    normalizeLegacyNumberField(write, "importance");
    normalizeLegacyNumberField(write, "confidence");
    normalizeLegacyIntegerField(write, "deadline_ms");
}

void normalizeProjectMemoryArguments(
    Json& arguments,
    const std::string_view name)
{
    normalizeLegacyStringField(arguments, "project_id");
    normalizeLegacyIntegerField(arguments, "deadline_ms");

    if (name == "project_memory.initialize") {
        normalizeLegacyStringField(arguments, "project_path");
        normalizeLegacyStringField(arguments, "path");
        if (member(arguments, "project_path") == nullptr) {
            const auto alias = arguments.find("path");
            if (alias != arguments.end()) {
                arguments["project_path"] = *alias;
            }
        }
        arguments.erase("path");
        normalizeLegacyStringFields(
            arguments,
            {"display_name", "repository_identity", "idempotency_key"});
        return;
    }
    if (name == "project_memory.remember") {
        normalizeProjectMemoryWrite(arguments);
        return;
    }
    if (name == "project_memory.remember_batch") {
        auto items = arguments.find("items");
        if (items != arguments.end() && items->is_array()) {
            for (auto& item : *items) {
                normalizeProjectMemoryWrite(item);
            }
        }
        return;
    }
    if (name == "project_memory.search") {
        normalizeLegacyStringFields(
            arguments, {"query", "session_id", "cursor"});
        normalizeLegacyIntegerField(arguments, "limit");
        normalizeLegacyIntegerField(arguments, "maximum_response_bytes");
        normalizeLegacyBooleanDefault(arguments, "include_body");
        normalizeProjectMemoryStringCollection(arguments, "kinds");
        normalizeProjectMemoryStringCollection(arguments, "tags");
        return;
    }
    if (name == "project_memory.get") {
        normalizeLegacyStringField(arguments, "id");
        normalizeLegacyBooleanDefault(arguments, "include_body");
        normalizeProjectMemoryStringCollection(arguments, "ids");
        return;
    }
    if (name == "project_memory.update") {
        normalizeLegacyStringFields(
            arguments, {"id", "title", "summary", "body"});
        normalizeLegacyIntegerField(arguments, "expected_version");
        normalizeProjectMemoryStringCollection(arguments, "tags");
        return;
    }
    if (name == "project_memory.forget") {
        normalizeLegacyStringField(arguments, "id");
        return;
    }
    if (name == "project_memory.list_recent") {
        normalizeLegacyStringFields(arguments, {"session_id", "cursor"});
        normalizeLegacyIntegerField(arguments, "limit");
        normalizeLegacyIntegerField(arguments, "maximum_response_bytes");
        normalizeLegacyBooleanDefault(arguments, "include_body");
        normalizeProjectMemoryStringCollection(arguments, "kinds");
        return;
    }
    if (name == "project_memory.link") {
        normalizeLegacyStringFields(
            arguments, {"source_id", "target_id", "relation"});
        return;
    }
    if (name == "project_memory.import") {
        normalizeLegacyStringFields(arguments, {"artifact", "merge_policy"});
        normalizeLegacyBooleanDefault(arguments, "preview");
    }
}

void normalizeContinuityArguments(Json& arguments, const std::string_view name)
{
    normalizeLegacyStringField(arguments, "project_id");
    if (name == "continuity.checkpoint" ||
        name == "continuity.prepare_handoff" ||
        name == "continuity.request_rollover") {
        normalizeLegacyStringFields(
            arguments,
            {"operation_id",
             "handoff_id",
             "predecessor_session_id",
             "provider_session_id",
             "model",
             "mission",
             "phase_id",
             "work_item_id",
             "summary",
             "repository_root",
             "branch",
             "commit",
             "adapter_id",
             "idempotency_key",
             "context_budget_source"});
        for (const auto key : {
                 "constraints",
                 "dirty_summary",
                 "active_files",
                 "open_work",
                 "decisions",
                 "passed_gates",
                 "open_gates",
                 "memory_record_ids",
                 "evidence_ids",
                 "next_actions"}) {
            normalizeContinuityStringCollection(arguments, key);
        }
        return;
    }
    if (name == "continuity.acknowledge_handoff") {
        normalizeLegacyStringFields(
            arguments,
            {"operation_id",
             "handoff_id",
             "successor_session_id",
             "adapter_id"});
        return;
    }
    if (name == "continuity.resume") {
        normalizeLegacyStringField(arguments, "operation_id");
    }
}

void normalizeClosedPackArguments(
    Json& arguments,
    const std::string_view name)
{
    if (name.starts_with("project_memory.")) {
        normalizeProjectMemoryArguments(arguments, name);
    } else if (name.starts_with("continuity.")) {
        normalizeContinuityArguments(arguments, name);
    }
}

[[nodiscard]] bool isNativeInvalidUtf8FileSource(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::InvalidRequest &&
        (error.message == "Text is not valid UTF-8." ||
         error.message == "The file content contains NUL.");
}

[[nodiscard]] Domain::Error remapFsReadError(Domain::Error error)
{
    if (error.code == Domain::ErrorCodes::Unauthorized) {
        return error;
    }
    if (error.code == Domain::ErrorCodes::PayloadTooLarge) {
        error.code = "file_too_large";
    } else if (error.code == Domain::ErrorCodes::RecordNotFound ||
               isNativeInvalidUtf8FileSource(error)) {
        error.code = "not_found";
    }
    return error;
}

[[nodiscard]] Domain::Error remapFsEditError(Domain::Error error)
{
    if (error.code == Domain::ErrorCodes::Unauthorized) {
        return error;
    }
    if (error.code == Domain::ErrorCodes::PayloadTooLarge) {
        error.code = "file_too_large";
    } else if (
        error.code == Domain::ErrorCodes::RecordNotFound &&
        error.message == "The text-edit search value was not found.") {
        error.code = "no_match";
    } else if (error.code == Domain::ErrorCodes::RecordNotFound) {
        error.code = "not_found";
    }
    return error;
}

[[nodiscard]] Domain::Error remapPdfFromFileError(Domain::Error error)
{
    if (error.code == Domain::ErrorCodes::Unauthorized) {
        return error;
    }
    if (error.code == Domain::ErrorCodes::RecordNotFound ||
        (error.code == Domain::ErrorCodes::InvalidRequest &&
         error.message ==
             "The PDF source file must contain valid NUL-free UTF-8 text.")) {
        error.code = "not_found";
    }
    return error;
}

[[nodiscard]] Domain::Result<void> validateValueAgainstSchema(
    const Json& value,
    const Json& schema,
    const std::string_view field)
{
    try {
        const auto type = schema.find("type");
        if (type != schema.end() && type->is_string()) {
            const auto& name = type->get_ref<const std::string&>();
            const bool matches =
                (name == "string" && value.is_string()) ||
                (name == "boolean" && value.is_boolean()) ||
                (name == "object" && value.is_object()) ||
                (name == "array" && value.is_array()) ||
                (name == "integer" &&
                 (value.is_number_integer() || value.is_number_unsigned())) ||
                (name == "number" && value.is_number() &&
                 std::isfinite(value.get<double>()));
            if (!matches) {
                return invalid(
                    std::string{field} + " does not match its declared type.");
            }
        }
        if (value.is_array()) {
            const auto maximum = schema.find("maxItems");
            if (maximum != schema.end() && maximum->is_number_unsigned() &&
                value.size() > maximum->get<std::size_t>()) {
                return invalid(
                    std::string{field} + " exceeds its declared item limit.");
            }
            const auto items = schema.find("items");
            if (items != schema.end() && items->is_object()) {
                for (const auto& item : value) {
                    auto valid = validateValueAgainstSchema(item, *items, field);
                    if (!valid) {
                        return valid;
                    }
                }
            }
        }
        if (value.is_number()) {
            const double number = value.get<double>();
            const auto minimum = schema.find("minimum");
            if (minimum != schema.end() && minimum->is_number() &&
                number < minimum->get<double>()) {
                return invalid(
                    std::string{field} + " is below its declared minimum.");
            }
            const auto exclusiveMinimum = schema.find("exclusiveMinimum");
            if (exclusiveMinimum != schema.end() &&
                exclusiveMinimum->is_number() &&
                number <= exclusiveMinimum->get<double>()) {
                return invalid(
                    std::string{field} + " must exceed its declared minimum.");
            }
            const auto maximum = schema.find("maximum");
            if (maximum != schema.end() && maximum->is_number() &&
                number > maximum->get<double>()) {
                return invalid(
                    std::string{field} + " exceeds its declared maximum.");
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The MCP tool schema could not be evaluated."));
    }
}

[[nodiscard]] Domain::Result<void> validateArgumentsAgainstSchema(
    const Json& arguments,
    const Domain::McpToolDescriptor& descriptor)
{
    try {
        auto schema = Json::parse(
            descriptor.inputSchema.begin(),
            descriptor.inputSchema.end(),
            nullptr,
            false,
            false);
        if (schema.is_discarded() || !schema.is_object() ||
            !arguments.is_object()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The MCP tool schema is unavailable."));
        }
        const bool closed = schema.value("additionalProperties", true) == false;
        // The legacy macOS packs intentionally accept documented aliases and
        // coercions beyond their advertised schemas. Enforce exact shape only
        // for the two source-closed typed packs; their handlers then perform
        // the remaining semantic validation through Domain requests.
        if (!closed) {
            return Domain::Result<void>::success();
        }
        const auto required = schema.find("required");
        if (required != schema.end() && required->is_array()) {
            for (const auto& item : *required) {
                if (!item.is_string() ||
                    arguments.find(item.get_ref<const std::string&>()) ==
                        arguments.end()) {
                    return invalid(
                        item.is_string()
                            ? item.get<std::string>() + " is required."
                            : "The tool schema contains an invalid requirement.");
                }
            }
        }
        const auto properties = schema.find("properties");
        if (properties == schema.end() || !properties->is_object()) {
            return Domain::Result<void>::success();
        }
        for (auto item = arguments.begin(); item != arguments.end(); ++item) {
            const auto property = properties->find(item.key());
            if (property == properties->end()) {
                if (closed) {
                    return invalid(
                        "Unknown argument '" + item.key() + "' is not allowed.");
                }
                continue;
            }
            auto valid = validateValueAgainstSchema(
                item.value(), *property, item.key());
            if (!valid) {
                return valid;
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The MCP tool arguments could not be validated."));
    }
}

template <typename T>
[[nodiscard]] Domain::Result<T> parseStrongUuid(
    const Json& object,
    const std::string_view key)
{
    const auto encoded = strictString(object, key);
    if (!encoded || encoded->empty()) {
        return failure<T>(
            Domain::ErrorCodes::InvalidRequest,
            std::string{key} + " is required.",
            true);
    }
    auto parsed = T::parse(*encoded);
    if (!parsed) {
        return Domain::Result<T>::failure(std::move(parsed).error());
    }
    return parsed;
}

template <typename T>
[[nodiscard]] Domain::Result<T> parseOpaque(
    const std::string_view encoded,
    const std::string_view field)
{
    auto parsed = T::parse(encoded);
    if (!parsed) {
        return failure<T>(
            Domain::ErrorCodes::InvalidRequest,
            std::string{field} + " is invalid.",
            true);
    }
    return parsed;
}

[[nodiscard]] Domain::Result<Domain::PathText> pathText(
    const std::string_view encoded,
    const std::string_view field)
{
    auto parsed = Domain::PathText::create(encoded);
    if (!parsed) {
        return failure<Domain::PathText>(
            parsed.error().code,
            std::string{field} + " is invalid: " + parsed.error().message);
    }
    return parsed;
}

[[nodiscard]] bool isAbsoluteToolPath(const std::string_view path) noexcept
{
    return path.starts_with("\\\\") || path.starts_with("//") ||
        path.starts_with("/") ||
        (path.size() >= 3U &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
}

[[nodiscard]] std::string anchoredToolPath(
    const std::string_view path,
    const std::string_view root)
{
    if (path.empty() || root.empty() || isAbsoluteToolPath(path)) {
        return std::string{path};
    }
    std::string result{root};
    if (!result.ends_with('\\') && !result.ends_with('/')) {
        result.push_back('\\');
    }
    result.append(path);
    return result;
}

enum class ContinuityPathRole { Path, WorkingDirectory };

class ToolContinuityObservationBuilder final {
public:
    void observe(
        const Contracts::AuthorizedPath& authorized,
        const ContinuityPathRole role)
    {
        if (!observation_.baseDirectory) {
            observation_.baseDirectory = authorized.authorityRoot();
        }
        if (role == ContinuityPathRole::WorkingDirectory) {
            observation_.workingDirectory = authorized.canonicalPath();
            return;
        }
        if (!observation_.path) {
            observation_.path = authorized.canonicalPath();
        }
    }

    [[nodiscard]] std::optional<Domain::ToolContinuityObservation>
    finish() const
    {
        if (!observation_.path && !observation_.workingDirectory &&
            !observation_.baseDirectory) {
            return std::nullopt;
        }
        return observation_;
    }

private:
    Domain::ToolContinuityObservation observation_;
};

[[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorizePath(
    Contracts::IWorkspaceAuthority& resolver,
    const Contracts::WorkspaceAuthority& authority,
    const std::string_view encoded,
    const Domain::FileAccess access,
    const bool protectAuthorityRoot,
    const Domain::OperationContext& context,
    ToolContinuityObservationBuilder* const observation = nullptr,
    const ContinuityPathRole role = ContinuityPathRole::Path)
{
    std::optional<Domain::PathText> base;
    if (!isAbsoluteToolPath(encoded) && !authority.trustedRoots().empty()) {
        base = authority.trustedRoots().front();
    }
    const auto anchored = anchoredToolPath(
        encoded, base ? base->value() : std::string_view{});
    auto requested = pathText(anchored, "path");
    if (!requested) {
        return propagate<Contracts::AuthorizedPath>(std::move(requested));
    }
    auto authorized = resolver.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            std::move(requested).value(),
            std::move(base),
            access,
            protectAuthorityRoot},
        context);
    if (authorized && observation != nullptr) {
        observation->observe(authorized.value(), role);
    }
    return authorized;
}

[[nodiscard]] std::string defaultRoot(
    const Contracts::WorkspaceAuthority& authority)
{
    return authority.trustedRoots().empty()
        ? std::string{}
        : authority.trustedRoots().front().value();
}

[[nodiscard]] std::string fileNameWithoutExtension(
    const std::string_view path)
{
    const auto separator = path.find_last_of("/\\");
    const auto start = separator == std::string_view::npos ? 0U : separator + 1U;
    const auto extension = path.find_last_of('.');
    const auto end = extension == std::string_view::npos || extension < start
        ? path.size()
        : extension;
    return std::string{path.substr(start, end - start)};
}

[[nodiscard]] std::string pdfPath(std::string path)
{
    if (path.size() >= 4U) {
        const auto offset = path.size() - 4U;
        std::string extension = path.substr(offset);
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (extension == ".pdf") {
            return path;
        }
    }
    path.append(".pdf");
    return path;
}

[[nodiscard]] std::string replaceExtensionWithPdf(std::string path)
{
    const auto separator = path.find_last_of("/\\");
    const auto extension = path.find_last_of('.');
    if (extension != std::string::npos &&
        (separator == std::string::npos || extension > separator)) {
        path.erase(extension);
    }
    path.append(".pdf");
    return path;
}

[[nodiscard]] std::string formatTimestamp(const Domain::UtcTimePoint value)
{
    const auto seconds =
        std::chrono::floor<std::chrono::seconds>(value.time_since_epoch());
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch() - seconds);
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<long long>(milliseconds.count()));
    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return {};
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

[[nodiscard]] Domain::Result<Domain::UtcTimePoint> parseTimestamp(
    const std::string_view value)
{
    if (value.size() < 20U || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':') {
        return failure<Domain::UtcTimePoint>(
            Domain::ErrorCodes::InvalidRequest,
            "expires_at must be an ISO-8601 UTC timestamp.");
    }
    auto parsePart = [&](const std::size_t offset, const std::size_t count)
        -> std::optional<int> {
        int parsed{};
        const auto result = std::from_chars(
            value.data() + offset, value.data() + offset + count, parsed);
        if (result.ec != std::errc{} ||
            result.ptr != value.data() + offset + count) {
            return std::nullopt;
        }
        return parsed;
    };
    const auto year = parsePart(0U, 4U);
    const auto month = parsePart(5U, 2U);
    const auto day = parsePart(8U, 2U);
    const auto hour = parsePart(11U, 2U);
    const auto minute = parsePart(14U, 2U);
    const auto second = parsePart(17U, 2U);
    if (!year || !month || !day || !hour || !minute || !second) {
        return failure<Domain::UtcTimePoint>(
            Domain::ErrorCodes::InvalidRequest,
            "expires_at must be an ISO-8601 UTC timestamp.");
    }
    std::size_t cursor = 19U;
    std::chrono::milliseconds fractional{};
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        std::int64_t digits{};
        std::size_t count{};
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9' && count < 3U) {
            digits = digits * 10 + (value[cursor] - '0');
            ++cursor;
            ++count;
        }
        while (count < 3U) {
            digits *= 10;
            ++count;
        }
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9') {
            ++cursor;
        }
        fractional = std::chrono::milliseconds{digits};
    }
    if (cursor + 1U != value.size() || value[cursor] != 'Z') {
        return failure<Domain::UtcTimePoint>(
            Domain::ErrorCodes::InvalidRequest,
            "expires_at must use the UTC Z suffix.");
    }
    std::tm utc{};
    utc.tm_year = *year - 1900;
    utc.tm_mon = *month - 1;
    utc.tm_mday = *day;
    utc.tm_hour = *hour;
    utc.tm_min = *minute;
    utc.tm_sec = *second;
    const __time64_t encoded = ::_mkgmtime64(&utc);
    if (encoded < 0) {
        return failure<Domain::UtcTimePoint>(
            Domain::ErrorCodes::InvalidRequest,
            "expires_at is outside the supported UTC range.");
    }
    return Domain::Result<Domain::UtcTimePoint>::success(
        Domain::UtcTimePoint{std::chrono::seconds{encoded}} + fractional);
}

[[nodiscard]] Json stringArray(const std::vector<std::string>& values)
{
    Json result = Json::array();
    result.get_ref<Json::array_t&>().reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

template <typename T>
[[nodiscard]] Json identifierArray(const std::vector<T>& values)
{
    Json result = Json::array();
    result.get_ref<Json::array_t&>().reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value.value());
    }
    return result;
}

template <typename T>
void optionalIdentifier(Json& object, const char* key, const std::optional<T>& value)
{
    object[key] = value ? Json(value->value()) : Json(nullptr);
}

void optionalText(
    Json& object,
    const char* key,
    const std::optional<std::string>& value)
{
    object[key] = value ? Json(*value) : Json(nullptr);
}

void optionalTimestamp(
    Json& object,
    const char* key,
    const std::optional<Domain::UtcTimePoint>& value)
{
    object[key] = value ? Json(formatTimestamp(*value)) : Json(nullptr);
}

[[nodiscard]] Json agentSpecJson(
    const Domain::AgentSpec& spec,
    const bool includeBody)
{
    Json result{
        {"id", spec.id.value()},
        {"display_name", spec.displayName},
        {"description", spec.description},
        {"tools", stringArray(spec.tools)},
        {"tools_forbidden", stringArray(spec.toolsForbidden)},
        {"when_to_use", stringArray(spec.whenToUse)},
        {"first_moves", stringArray(spec.firstMoves)},
        {"done_definition", stringArray(spec.doneDefinition)},
        {"output_schema", stringArray(spec.outputSchema)},
        {"handoff", stringArray(spec.handoff)},
        {"quality_bar", stringArray(spec.qualityBar)},
        {"source", spec.source}};
    if (includeBody) {
        result["body"] = spec.body;
    }
    return result;
}

[[nodiscard]] Json agentSessionJson(const Domain::AgentSession& session)
{
    Json result{
        {"id", session.id.value()},
        {"agent_id", session.agentId.value()},
        {"status", Domain::wireName(session.status)},
        {"created_at", formatTimestamp(session.createdAt)},
        {"updated_at", formatTimestamp(session.updatedAt)}};
    optionalIdentifier(result, "client_id", session.clientId);
    optionalText(result, "summary", session.summary);
    return result;
}

[[nodiscard]] Json activeBindingJson(const Domain::ActiveBinding& binding)
{
    Json result{
        {"session_id", binding.sessionId.value()},
        {"agent_id", binding.agentId.value()},
        {"goal", binding.goal},
        {"tools_primary", stringArray(binding.toolsPrimary)},
        {"tools_forbidden", stringArray(binding.toolsForbidden)},
        {"output_schema", stringArray(binding.outputSchema)},
        {"done_definition", stringArray(binding.doneDefinition)}};
    result["cwd"] = binding.workingDirectory
        ? Json(binding.workingDirectory->value())
        : Json(nullptr);
    return result;
}

[[nodiscard]] Json legacyMemoryNoteJson(
    const Domain::LegacyMemoryNoteProjection& note)
{
    Json result{
        {"key", note.key},
        {"tags", stringArray(note.tags)},
        {"body_chars", note.bodyUtf8Bytes},
        {"created_at", formatTimestamp(note.createdAt)},
        {"updated_at", formatTimestamp(note.updatedAt)}};
    if (note.body) {
        result["body"] = *note.body;
    }
    return result;
}

[[nodiscard]] Json memoryNoteJson(const Domain::MemoryNote& note)
{
    return Json{
        {"key", note.key},
        {"body", note.body},
        {"tags", stringArray(note.tags)},
        {"created_at", formatTimestamp(note.createdAt)},
        {"updated_at", formatTimestamp(note.updatedAt)}};
}

[[nodiscard]] Json legacyAgentJson(
    const Domain::LegacyAgentContinuitySnapshot& snapshot)
{
    Json result{
        {"session_id", snapshot.sessionId.value()},
        {"agent_id", snapshot.agentId.value()},
        {"goal", snapshot.goal},
        {"status", snapshot.status},
        {"resume_hint", snapshot.resumeHint}};
    if (snapshot.workingDirectory) {
        result["cwd"] = *snapshot.workingDirectory;
    }
    if (snapshot.updatedAt) {
        result["updated_at"] = formatTimestamp(*snapshot.updatedAt);
    }
    return result;
}

[[nodiscard]] Json legacyPacketJson(
    const Domain::LegacyHandoffPacket& packet)
{
    Json meta{
        {"id", packet.id.value()},
        {"schema_version", packet.schemaVersion},
        {"created_at", formatTimestamp(packet.createdAt)},
        {"updated_at", formatTimestamp(packet.updatedAt)},
        {"source", Domain::wireName(packet.source)},
        {"resume_ready", packet.resumeReady}};
    if (packet.chatLabel) {
        meta["chat_label"] = *packet.chatLabel;
    }
    if (packet.clientId) {
        meta["client_id"] = packet.clientId->value();
    }
    Json task{
        {"goal", packet.goal},
        {"status", packet.status},
        {"blockers", stringArray(packet.blockers)},
        {"next_actions", stringArray(packet.nextActions)}};
    if (packet.projectSlug) {
        task["project_slug"] = *packet.projectSlug;
    }
    if (packet.workingDirectory) {
        task["cwd"] = *packet.workingDirectory;
    }
    Json agents = Json::array();
    for (const auto& agent : packet.agents) {
        agents.push_back(legacyAgentJson(agent));
    }
    return Json{
        {"schema_version", packet.schemaVersion},
        {"meta", std::move(meta)},
        {"task", std::move(task)},
        {"working_set",
         Json{{"key_files", stringArray(packet.keyFiles)},
              {"decisions", stringArray(packet.decisions)}}},
        {"agents", std::move(agents)},
        {"narrative", packet.narrative},
        {"resume",
         Json{{"seed", packet.resumeSeed},
              {"custom", packet.resumeSeedIsCustom},
              {"instructions",
               Json::array({
                   "Call context_get if you need the full packet again",
                   "Pass this handoff id to session_checkpoint/session_handoff when continuing it",
                   "Reattach open agents with agent_run_status(session_id) or complete and restart",
                   "Update memory/current-task.md via session_checkpoint as you progress"})}}}};
}

[[nodiscard]] Json legacyPersistJson(
    const Domain::LegacyContinuityPersistOutcome& outcome,
    const std::string_view action)
{
    const auto& packet = outcome.record.packet;
    Json result{
        {"ok", true},
        {"action", action},
        {"handoff_id", packet.id.value()},
        {"resume_ready", packet.resumeReady},
        {"packet", legacyPacketJson(packet)},
        {"resume_seed", packet.resumeSeed},
        {"projection_ok", outcome.projectionOk},
        {"projection_repair_pending", outcome.projectionRepairPending}};
    Json paths = Json::object();
    optionalText(paths, "json", outcome.projection.packetPath);
    optionalText(paths, "current_task", outcome.projection.currentTaskPath);
    result["paths"] = std::move(paths);
    if (outcome.projectionWarning) {
        result["projection_warning"] = outcome.projectionWarning->message;
    }
    if (outcome.handoffRequired) {
        result["handoff_required"] = true;
        result["message"] =
            "Handoff saved. Start a new LM Studio chat with Forge MCP enabled, then call context_get (or use resume.seed as the first user message).";
    }
    return result;
}

[[nodiscard]] Json legacyGetJson(
    const Domain::LegacyContinuityRecord& record,
    const Domain::ClientWorkspaceAdoption& adoption)
{
    const auto& packet = record.packet;
    Json result{
        {"ok", true},
        {"action", "get"},
        {"found", true},
        {"handoff_id", packet.id.value()},
        {"resume_ready", packet.resumeReady},
        {"packet", legacyPacketJson(packet)},
        {"resume_seed", packet.resumeSeed},
        {"projection_checked", false},
        {"projection_ok", nullptr},
        {"projection_status", "unverified"},
        {"paths", Json::object()}};
    if (adoption.snapshot) {
        result["workspace_adopted"] =
            adoption.snapshot->authorityRoot.value();
        result["workspace_project_id"] =
            adoption.snapshot->projectId.value();
        result["workspace_generation"] = adoption.snapshot->generation;
    } else {
        result["workspace_adopted"] = nullptr;
    }
    if (adoption.warning) {
        result["workspace_adoption_warning"] = adoption.warning->message;
    }
    return result;
}

[[nodiscard]] Json projectDescriptorJson(
    const Domain::ProjectMemoryDescriptor& descriptor)
{
    Json aliases = Json::array();
    for (const auto& alias : descriptor.aliases) {
        aliases.push_back(alias.value());
    }
    Json result{
        {"project_id", descriptor.id.value()},
        {"display_name", descriptor.displayName},
        {"aliases", std::move(aliases)}};
    optionalText(result, "repository_identity", descriptor.repositoryIdentity);
    return result;
}

[[nodiscard]] Json projectMemoryLimitsJson(
    const Domain::ProjectMemoryLimits& limits)
{
    return Json{
        {"title_bytes", limits.maximumTitleBytes},
        {"summary_bytes", limits.maximumSummaryBytes},
        {"body_bytes", limits.maximumBodyBytes},
        {"source_reference_bytes", limits.maximumSourceReferenceBytes},
        {"tag_count", limits.maximumTagCount},
        {"tag_bytes", limits.maximumTagBytes},
        {"batch_count", limits.maximumBatchCount},
        {"batch_bytes", limits.maximumBatchBytes},
        {"page_count", limits.maximumPageCount},
        {"response_bytes", limits.maximumResponseBytes},
        {"open_projects", limits.maximumOpenProjects},
        {"artifact_records", limits.maximumArtifactRecords},
        {"artifact_bytes", limits.maximumArtifactBytes}};
}

[[nodiscard]] Json projectRecordJson(
    const Domain::ProjectMemoryRecord& record,
    const bool includeBody,
    const std::optional<double> score = std::nullopt)
{
    Json result{
        {"id", record.id.value()},
        {"project_id", record.projectId.value()},
        {"version", record.version},
        {"kind", record.kind},
        {"title", record.title},
        {"summary", record.summary},
        {"tags", stringArray(record.tags)},
        {"importance", record.importance},
        {"confidence", record.confidence},
        {"source_kind", record.sourceKind},
        {"created_at", formatTimestamp(record.createdAt)},
        {"updated_at", formatTimestamp(record.updatedAt)},
        {"last_accessed_at", formatTimestamp(record.lastAccessedAt)},
        {"content_hash", record.contentHash.value()},
        {"is_tombstone", record.isTombstone},
        {"schema_version", record.schemaVersion}};
    optionalText(result, "source_reference", record.sourceReference);
    optionalIdentifier(result, "session_id", record.sessionId);
    optionalTimestamp(result, "expires_at", record.expiresAt);
    if (includeBody) {
        optionalText(result, "body", record.body);
    }
    if (score) {
        result["score"] = *score;
    }
    return result;
}

[[nodiscard]] Json memoryWriteJson(const Domain::MemoryWriteOutcome& outcome)
{
    return Json{
        {"ok", true},
        {"project_id", outcome.projectId.value()},
        {"record_id", outcome.recordId.value()},
        {"record_version", outcome.recordVersion},
        {"disposition", Domain::wireName(outcome.disposition)},
        {"content_hash", outcome.contentHash.value()},
        {"schema_version", outcome.schemaVersion},
        {"capability_version", outcome.capabilityVersion}};
}

[[nodiscard]] Json memoryPageJson(
    const Domain::MemoryPage& page,
    const bool includeBody)
{
    Json records = Json::array();
    for (const auto& hit : page.records) {
        records.push_back(projectRecordJson(hit.record, includeBody, hit.score));
    }
    return Json{
        {"ok", true},
        {"project_id", page.projectId.value()},
        {"count", records.size()},
        {"records", std::move(records)},
        {"next_cursor", page.nextCursor ? Json(*page.nextCursor) : Json(nullptr)},
        {"truncated", page.truncated},
        {"encoded_bytes", page.encodedBytes},
        {"maximum_response_bytes", page.maximumResponseBytes},
        {"schema_version", page.schemaVersion},
        {"capability_version", page.capabilityVersion}};
}

[[nodiscard]] Json processJson(
    const Domain::ProcessResult& result,
    const std::string_view workingDirectory)
{
    const bool ok = result.exitCode == 0 && !result.timedOut &&
        !result.cancelled && result.terminationConfirmed;
    return Json{
        {"ok", ok},
        {"exit_code", result.exitCode},
        {"stdout", result.stdoutUtf8},
        {"stderr", result.stderrUtf8},
        {"timed_out", result.timedOut},
        {"cancelled", result.cancelled},
        {"stdout_truncated", result.stdoutTruncated},
        {"stderr_truncated", result.stderrTruncated},
        {"termination_confirmed", result.terminationConfirmed},
        {"elapsed_ms", result.elapsed.count()},
        {"cwd", workingDirectory}};
}

[[nodiscard]] std::optional<Domain::Error> processReceiptError(
    const Json& payload)
{
    if (payload.value("ok", true)) {
        return std::nullopt;
    }
    if (payload.value("cancelled", false)) {
        return Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The native process operation was cancelled.");
    }
    if (payload.value("timed_out", false)) {
        return Domain::makeError(
            Domain::ErrorCodes::ProcessTimeout,
            "The native process exceeded its bounded timeout.",
            true);
    }
    if (!payload.value("termination_confirmed", true)) {
        return Domain::makeError(
            Domain::ErrorCodes::ProcessTerminationUnconfirmed,
            "The native process tree termination could not be confirmed.",
            true);
    }
    return Domain::makeError(
        Domain::ErrorCodes::ProcessExitNonzero,
        "The native process exited with a nonzero status.");
}

[[nodiscard]] bool isUtf8Boundary(
    const std::string_view content,
    const std::size_t offset) noexcept
{
    return offset == 0U || offset == content.size() ||
        (static_cast<unsigned char>(content[offset]) & 0xC0U) != 0x80U;
}

[[nodiscard]] std::size_t boundedUtf8End(
    const std::string_view content,
    const std::size_t start,
    const std::size_t maximumBytes) noexcept
{
    auto end = (std::min)(content.size(), start + maximumBytes);
    while (end > start && !isUtf8Boundary(content, end)) {
        --end;
    }
    return end;
}

[[nodiscard]] Json pdfJson(const Domain::PdfWriteReceipt& receipt)
{
    return Json{
        {"ok", true},
        {"path", receipt.path.value()},
        {"bytes_written", receipt.bytesWritten},
        {"pages", receipt.pages},
        {"engine", receipt.engine},
        {"title", receipt.title}};
}

[[nodiscard]] Json continuityOperationJson(
    const Domain::ContinuityOperation& operation)
{
    Json result{
        {"operation_id", operation.operationId.value()},
        {"project_id", operation.projectId.value()},
        {"predecessor_session_id", operation.predecessorSessionId.value()},
        {"handoff_id", operation.handoffId.value()},
        {"state", Domain::wireName(operation.state)},
        {"attempt", operation.attempt},
        {"adapter_id", operation.adapterId.value()},
        {"idempotency_key", operation.idempotencyKey.value()},
        {"created_at", formatTimestamp(operation.createdAt)},
        {"updated_at", formatTimestamp(operation.updatedAt)},
        {"state_checksum", operation.stateChecksum.value()}};
    optionalIdentifier(result, "successor_session_id", operation.successorSessionId);
    optionalIdentifier(result, "acknowledged_session_id", operation.acknowledgedSessionId);
    optionalIdentifier(result, "acknowledged_handoff_id", operation.acknowledgedHandoffId);
    optionalText(result, "last_error", operation.lastError);
    optionalTimestamp(result, "retry_at", operation.retryAt);
    result["retry_resume_state"] = operation.retryResumeState
        ? Json(Domain::wireName(*operation.retryResumeState))
        : Json(nullptr);
    return result;
}

[[nodiscard]] Json continuitySessionJson(
    const Domain::ContinuitySession& session)
{
    Json result{{"session_id", session.sessionId.value()}};
    optionalIdentifier(result, "provider_session_id", session.providerSessionId);
    optionalText(result, "model", session.model);
    optionalText(result, "provider", session.provider);
    return result;
}

[[nodiscard]] Json continuityHandoffJson(
    const Domain::ContinuityHandoff& handoff)
{
    Json completed = Json::array();
    for (const auto& entry : handoff.completedWork) {
        Json item{{"summary", entry.summary}};
        optionalText(item, "work_item_id", entry.workItemId);
        optionalText(item, "status", entry.status);
        completed.push_back(std::move(item));
    }
    Json open = Json::array();
    for (const auto& entry : handoff.openWork) {
        Json item{{"summary", entry.summary}};
        optionalText(item, "work_item_id", entry.workItemId);
        optionalText(item, "status", entry.status);
        open.push_back(std::move(item));
    }
    Json decisions = Json::array();
    for (const auto& decision : handoff.decisions) {
        Json item{{"decision", decision.decision}};
        optionalText(item, "rationale", decision.rationale);
        decisions.push_back(std::move(item));
    }
    Json commands = Json::array();
    for (const auto& command : handoff.validation.commands) {
        Json item{{"command", command.command}, {"exit_code", command.exitCode}};
        optionalIdentifier(item, "evidence_id", command.evidenceId);
        commands.push_back(std::move(item));
    }
    Json evidence = Json::array();
    for (const auto& reference : handoff.evidenceReferences) {
        Json item = Json::object();
        optionalIdentifier(item, "evidence_id", reference.evidenceId);
        item["path"] = reference.path
            ? Json(reference.path->value())
            : Json(nullptr);
        evidence.push_back(std::move(item));
    }
    Json actions = Json::array();
    for (const auto& action : handoff.nextActions) {
        actions.push_back(Json{
            {"order", action.order},
            {"action", action.action},
            {"command", action.command},
            {"success_condition", action.successCondition}});
    }
    Json activeFiles = Json::array();
    for (const auto& path : handoff.currentWork.activeFiles) {
        activeFiles.push_back(path.value());
    }
    Json result{
        {"schema_version", Domain::ContinuityHandoffSchemaVersion},
        {"handoff_id", handoff.handoffId.value()},
        {"operation_id", handoff.operationId.value()},
        {"created_at", formatTimestamp(handoff.createdAt)},
        {"project",
         Json{{"project_id", handoff.project.projectId.value()},
              {"display_name", handoff.project.displayName},
              {"repository_root", handoff.project.repositoryRoot.value()},
              {"branch", handoff.project.branch},
              {"commit", handoff.project.commit},
              {"dirty_summary", stringArray(handoff.project.dirtySummary)}}},
        {"predecessor_session", continuitySessionJson(handoff.predecessorSession)},
        {"successor_session",
         handoff.successorSession
             ? continuitySessionJson(*handoff.successorSession)
             : Json(nullptr)},
        {"mission", handoff.mission},
        {"constraints", stringArray(handoff.constraints)},
        {"current_work",
         Json{{"phase_id", handoff.currentWork.phaseId},
              {"work_item_id", handoff.currentWork.workItemId},
              {"summary", handoff.currentWork.summary},
              {"active_files", std::move(activeFiles)}}},
        {"completed_work", std::move(completed)},
        {"open_work", std::move(open)},
        {"decisions", std::move(decisions)},
        {"validation",
         Json{{"passed_gates", stringArray(handoff.validation.passedGates)},
              {"open_gates", stringArray(handoff.validation.openGates)},
              {"commands", std::move(commands)}}},
        {"memory_references", identifierArray(handoff.memoryReferences)},
        {"evidence_references", std::move(evidence)},
        {"next_actions", std::move(actions)},
        {"host_state",
         Json{{"adapter_id", handoff.hostState.adapterId.value()},
              {"continuity_state", Domain::wireName(handoff.hostState.continuityState)},
              {"context_budget_source", handoff.hostState.contextBudgetSource},
              {"remaining_budget_estimate",
               handoff.hostState.remainingBudgetEstimate
                   ? Json(*handoff.hostState.remainingBudgetEstimate)
                   : Json(nullptr)},
              {"retry",
               Json{{"attempt", handoff.hostState.retry.attempt},
                    {"last_error",
                     handoff.hostState.retry.lastError
                         ? Json(*handoff.hostState.retry.lastError)
                         : Json(nullptr)},
                    {"retry_at",
                     handoff.hostState.retry.retryAt
                         ? Json(formatTimestamp(*handoff.hostState.retry.retryAt))
                         : Json(nullptr)},
                    {"retry_resume_state",
                     handoff.hostState.retry.retryResumeState
                         ? Json(Domain::wireName(
                               *handoff.hostState.retry.retryResumeState))
                         : Json(nullptr)}}}}},
        {"integrity",
         Json{{"content_sha256", handoff.contentSha256.value()},
              {"redaction_complete", handoff.redactionComplete}}}};
    return result;
}

[[nodiscard]] Json hostSessionJson(const Domain::HostSession& session)
{
    static constexpr std::array<std::string_view, 7> names{
        "creating", "active", "bootstrapping", "ready", "sealed",
        "failed", "cancelled"};
    const auto index = static_cast<std::size_t>(session.status);
    Json result{
        {"session_id", session.id.value()},
        {"project_id", session.projectId.value()},
        {"operation_id", session.operationId.value()},
        {"predecessor_session_id", session.predecessorSessionId.value()},
        {"idempotency_key", session.idempotencyKey.value()},
        {"status", index < names.size() ? names[index] : "failed"}};
    optionalIdentifier(result, "provider_session_id", session.providerSessionId);
    optionalText(result, "model", session.model);
    return result;
}

} // namespace

class McpToolPackAdapter::Impl final {
public:
    explicit Impl(McpToolPackDependencies dependencies) noexcept
        : dependencies_{std::move(dependencies)}
    {
    }

    [[nodiscard]] std::span<const Domain::McpToolDescriptor>
    tools() const noexcept
    {
        return dependencies_.catalog.tools();
    }

    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> handle(
        const Contracts::AuthorizedToolCall& authorizedCall,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept
    {
        const auto started = dependencies_.clock.monotonicNow();
        try {
            if (!authorizedCall.matches(authority, context)) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::Unauthorized,
                    "The MCP tool capability does not match workspace authority.");
            }
            const auto* selected = descriptor(authorizedCall.toolName());
            if (selected == nullptr ||
                selected->tool.effect != authorizedCall.effect()) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::Unauthorized,
                    "The MCP tool capability has a mismatched descriptor effect.");
            }
            if (selected->tool.requiresProject &&
                !authorizedCall.projectId()) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The MCP tool requires an explicit project scope.");
            }
            McpJsonCodec codec;
            auto canonical = codec.canonicalize(
                authorizedCall.canonicalRequest());
            if (!canonical) {
                return propagate<Domain::ToolCallOutcome>(std::move(canonical));
            }
            auto arguments = Json::parse(
                canonical.value().begin(),
                canonical.value().end(),
                nullptr,
                false,
                false);
            if (arguments.is_discarded() || !arguments.is_object()) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::InvalidRequest,
                    "MCP tool arguments must be one JSON object.");
            }
            normalizeClosedPackArguments(arguments, authorizedCall.toolName());
            auto schema = validateArgumentsAgainstSchema(arguments, *selected);
            if (!schema) {
                return propagate<Domain::ToolCallOutcome>(std::move(schema));
            }
            auto operationContext = derivedContext(
                authorizedCall.toolName(), arguments, context);
            if (!operationContext) {
                return propagate<Domain::ToolCallOutcome>(
                    std::move(operationContext));
            }
            ToolContinuityObservationBuilder continuityObservation;
            std::optional<Domain::ContextRecoveryReceipt> contextRecovery;
            auto payload = dispatch(
                authorizedCall,
                authority,
                arguments,
                operationContext.value(),
                continuityObservation,
                contextRecovery);
            if (!payload) {
                return propagate<Domain::ToolCallOutcome>(std::move(payload));
            }
            if (!payload.value().is_object()) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::InternalFailure,
                    "The MCP tool adapter produced a non-object payload.");
            }
            if (authorizedCall.toolName() == "context_get" &&
                payload.value().value("found", false)) {
                const auto handoffId = payload.value().find("handoff_id");
                if (handoffId == payload.value().end() ||
                    !handoffId->is_string()) {
                    return failure<Domain::ToolCallOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The recovered context payload has no handoff id.");
                }
                auto parsed = Domain::LegacyHandoffId::parse(
                    handoffId->get_ref<const std::string&>());
                if (!parsed) {
                    return propagate<Domain::ToolCallOutcome>(
                        std::move(parsed));
                }
                if (!contextRecovery ||
                    contextRecovery->clientId != authorizedCall.clientId() ||
                    contextRecovery->handoffId != parsed.value()) {
                    return failure<Domain::ToolCallOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The recovered context metadata does not match its payload.");
                }
            } else if (contextRecovery) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Unexpected recovered context metadata was emitted.");
            }
            auto encoded = codec.canonicalize(payload.value().dump());
            if (!encoded) {
                return propagate<Domain::ToolCallOutcome>(std::move(encoded));
            }
            const bool ok = payload.value().value("ok", true);
            const bool continuityTool =
                selected->tool.pack == "ContinuityToolPack" ||
                selected->tool.pack == "ContinuityLifecycleToolPack";
            auto observation = !continuityTool
                ? continuityObservation.finish()
                : std::optional<Domain::ToolContinuityObservation>{};
            auto receiptError = processReceiptError(payload.value());
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                dependencies_.clock.monotonicNow() - started);
            if (elapsed < std::chrono::milliseconds::zero()) {
                elapsed = std::chrono::milliseconds::zero();
            }
            return Domain::Result<Domain::ToolCallOutcome>::success(
                Domain::ToolCallOutcome{
                    Domain::ToolExecutionReceipt{
                        authorizedCall.requestId(),
                        authorizedCall.toolName(),
                        ok,
                        std::move(receiptError),
                        elapsed},
                    std::move(encoded).value(),
                    std::move(contextRecovery),
                    std::move(observation)});
        } catch (...) {
            return failure<Domain::ToolCallOutcome>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP tool adapter failed at its exception boundary.");
        }
    }

private:
    [[nodiscard]] const Domain::McpToolDescriptor* descriptor(
        const std::string_view name) const noexcept
    {
        const auto descriptors = dependencies_.catalog.tools();
        const auto found = std::find_if(
            descriptors.begin(), descriptors.end(),
            [&](const Domain::McpToolDescriptor& candidate) {
                return candidate.tool.name == name;
            });
        return found == descriptors.end() ? nullptr : &*found;
    }

    [[nodiscard]] Domain::Result<Domain::OperationContext> derivedContext(
        const std::string_view toolName,
        const Json& arguments,
        const Domain::OperationContext& parent) const
    {
        const auto deadline = strictInteger(arguments, "deadline_ms");
        if (!deadline) {
            if (!toolName.starts_with("project_memory.")) {
                return Domain::Result<Domain::OperationContext>::success(parent);
            }
            return Domain::Result<Domain::OperationContext>::success(
                Domain::OperationContext{
                    parent.operationId,
                    (std::min)(
                        parent.deadline,
                        dependencies_.clock.monotonicNow() +
                            Domain::MaximumProjectMemoryDeadline),
                    parent.cancellation,
                    parent.correlationId});
        }
        if (*deadline < Domain::MinimumProjectMemoryDeadline.count() ||
            *deadline > Domain::MaximumProjectMemoryDeadline.count()) {
            return failure<Domain::OperationContext>(
                Domain::ErrorCodes::InvalidRequest,
                "deadline_ms must be within 1...60000.");
        }
        const auto requested = dependencies_.clock.monotonicNow() +
            std::chrono::milliseconds{*deadline};
        return Domain::Result<Domain::OperationContext>::success(
            Domain::OperationContext{
                parent.operationId,
                (std::min)(parent.deadline, requested),
                parent.cancellation,
                parent.correlationId});
    }

    [[nodiscard]] Domain::Result<Json> dispatch(
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation,
        std::optional<Domain::ContextRecoveryReceipt>& contextRecovery)
    {
        const auto& name = call.toolName();
        if (name == "forge_status" || name.starts_with("agent_")) {
            return agents(
                name, call, authority, arguments, context, observation);
        }
        if (name == "session_checkpoint" || name == "session_handoff" ||
            name == "context_get" || name == "context_list") {
            return legacyContinuity(
                name,
                call,
                authority,
                arguments,
                context,
                contextRecovery);
        }
        if (name.starts_with("fs_")) {
            return fileSystem(
                name, authority, arguments, context, observation);
        }
        if (name.starts_with("git_")) {
            return git(name, authority, arguments, context, observation);
        }
        if (name.starts_with("memory_")) {
            return legacyMemory(name, arguments, context);
        }
        if (name.starts_with("pdf_")) {
            return pdf(name, authority, arguments, context, observation);
        }
        if (name == "search_text") {
            return search(authority, arguments, context, observation);
        }
        if (name == "shell_exec") {
            return shell(authority, arguments, context, observation);
        }
        if (name.starts_with("project_memory.")) {
            return projectMemory(
                name, call, authority, arguments, context, observation);
        }
        if (name.starts_with("continuity.")) {
            return continuity(name, call, authority, arguments, context);
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested MCP tool has no registered adapter.");
    }

    [[nodiscard]] Domain::Result<Json> agents(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "forge_status") {
            auto home = dependencies_.applicationPaths.dataRoot(context);
            if (!home) {
                return propagate<Json>(std::move(home));
            }
            auto catalog = dependencies_.agentCatalog.all(context);
            if (!catalog) {
                return propagate<Json>(std::move(catalog));
            }
            Domain::LegacyMemoryListRequest memoryRequest;
            memoryRequest.includeSystem = false;
            memoryRequest.includeBody = false;
            memoryRequest.requestedLimit = 1;
            auto memory = dependencies_.legacyMemory.list(memoryRequest, context);
            auto status = dependencies_.forgeStatus.snapshot(context);
            if (!status) {
                return propagate<Json>(std::move(status));
            }
            auto validStatus =
                Domain::validateForgeStatusProjection(status.value());
            if (!validStatus) {
                return propagate<Json>(std::move(validStatus));
            }
            Json agents = Json::array();
            for (const auto& spec : catalog.value()) {
                agents.push_back(spec.id.value());
            }
            Json tools = Json::array();
            for (const auto& item : dependencies_.catalog.tools()) {
                tools.push_back(item.tool.name);
            }
            Json openIds = Json::array();
            for (const auto& sessionId : status.value().openSessionIds) {
                openIds.push_back(sessionId.value());
            }
            const std::size_t memoryCount = memory
                ? memory.value().visibleTotal
                : 0U;
            Json continuityStatus = Json::object();
            auto continuity =
                dependencies_.legacyContinuity.statusSummary(context);
            if (continuity) {
                const auto& summary = continuity.value();
                continuityStatus = Json{
                    {"latest_id",
                     summary.latestId
                         ? Json(summary.latestId->value())
                         : Json(nullptr)},
                    {"latest_updated_at",
                     summary.latestUpdatedAt
                         ? Json(formatTimestamp(*summary.latestUpdatedAt))
                         : Json(nullptr)},
                    {"resume_ready", summary.resumeReady},
                    {"resume_id",
                     summary.resumeId
                         ? Json(summary.resumeId->value())
                         : Json(nullptr)},
                    {"open_agent_sessions", summary.openAgentSessions},
                    {"tools", stringArray(summary.tools)},
                    {"note", summary.note},
                    {"auto",
                     Json{
                         {"checkpoint_every_tools",
                          summary.automatic.checkpointEveryTools},
                         {"handoff_every_tools",
                          summary.automatic.handoffEveryTools},
                         {"note", summary.automatic.note}}}};
            }

            const auto automatic =
                dependencies_.continuityAutomationStatus.snapshot(
                    call.clientId());
            Json implicitRoots = Json::array();
            for (const auto& root : automatic.implicitRoots) {
                implicitRoots.push_back(root.value());
            }
            Json automaticStatus{
                {"enabled", automatic.enabled},
                {"checkpoint_every_tools", automatic.checkpointEveryTools},
                {"handoff_every_tools", automatic.handoffEveryTools},
                {"progress_count", automatic.progressCount},
                {"blocked", automatic.blocked},
                {"handoff_id",
                 automatic.handoffId
                     ? Json(*automatic.handoffId)
                     : Json(nullptr)},
                {"implicit_roots", std::move(implicitRoots)}};
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"version", dependencies_.productVersion},
                {"runtime", dependencies_.runtimeName},
                {"home", home.value().value()},
                {"client_id", call.clientId().value()},
                {"agents", std::move(agents)},
                {"tools", std::move(tools)},
                {"memory_note_count", memoryCount},
                {"presence_count", status.value().presenceCount},
                {"open_sessions", openIds.size()},
                {"open_session_ids", std::move(openIds)},
                {"continuity", std::move(continuityStatus)},
                {"auto_continuity", std::move(automaticStatus)},
                {"pid", dependencies_.processId}});
        }
        if (name == "agent_list") {
            auto listed = dependencies_.agentCatalog.all(context);
            if (!listed) {
                return propagate<Json>(std::move(listed));
            }
            Json agents = Json::array();
            for (const auto& spec : listed.value()) {
                agents.push_back(agentSpecJson(spec, false));
            }
            return Domain::Result<Json>::success(
                Json{{"ok", true}, {"agents", std::move(agents)}});
        }
        if (name == "agent_get" || name == "agent_context") {
            auto id = legacyString(arguments, "agent_id");
            if (!id) {
                id = legacyString(arguments, "id");
            }
            if (!id) {
                id = legacyString(arguments, "name");
            }
            if (!id || id->empty()) {
                return failure<Json>(
                    Domain::ErrorCodes::AgentNotFound,
                    "Unknown agent",
                    true);
            }
            auto parsed = parseOpaque<Domain::AgentId>(*id, "agent_id");
            if (!parsed) {
                return propagate<Json>(std::move(parsed));
            }
            auto found = dependencies_.agentCatalog.get(parsed.value(), context);
            if (!found) {
                return propagate<Json>(std::move(found));
            }
            if (!found.value()) {
                return failure<Json>(
                    Domain::ErrorCodes::AgentNotFound,
                    "Unknown agent",
                    true);
            }
            auto result = agentSpecJson(*found.value(), true);
            result["ok"] = true;
            return Domain::Result<Json>::success(std::move(result));
        }
        if (name == "agent_recommend") {
            const auto task = legacyString(arguments, "task").value_or("");
            auto recommended = dependencies_.agentCatalog.recommend(task, context);
            if (!recommended) {
                return propagate<Json>(std::move(recommended));
            }
            const auto& spec = recommended.value();
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"agent_id", spec.id.value()},
                {"call",
                 "agent_run_start(agent_id: '" + spec.id.value() +
                     "', goal: ...)"},
                {"card", agentSpecJson(spec, false)}});
        }
        if (name == "agent_run_start") {
            auto id = legacyString(arguments, "agent_id");
            if (!id) {
                id = legacyString(arguments, "id");
            }
            if (!id) {
                id = legacyString(arguments, "name");
            }
            const auto selected = id.value_or("explore");
            auto agentId = parseOpaque<Domain::AgentId>(selected, "agent_id");
            if (!agentId) {
                return propagate<Json>(std::move(agentId));
            }
            std::optional<Domain::PathText> cwd;
            if (const auto encoded = legacyString(arguments, "cwd");
                encoded && !encoded->empty()) {
                auto authorized = authorizePath(
                    dependencies_.workspaceAuthority,
                    authority,
                    *encoded,
                    Domain::FileAccess::Write,
                    false,
                    context,
                    &observation,
                    ContinuityPathRole::WorkingDirectory);
                if (!authorized) {
                    return propagate<Json>(std::move(authorized));
                }
                cwd = authorized.value().canonicalPath();
            }
            Domain::AgentRunStartRequest request{
                std::move(agentId).value(),
                call.clientId(),
                call.projectId(),
                legacyString(arguments, "goal").value_or(""),
                cwd};
            auto started = dependencies_.agentSessions.startRun(request, context);
            if (!started) {
                return propagate<Json>(std::move(started));
            }
            const auto& outcome = started.value();
            Json next = Json::array({
                "Adopt agent.body as role instructions",
                "Execute first_moves",
                "Prefer tools_primary",
                "REQUIRED: agent_run_complete(session_id: '" +
                    outcome.run.session.id.value() +
                    "', report: {...output_schema})"});
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"session", agentSessionJson(outcome.run.session)},
                {"session_id", outcome.run.session.id.value()},
                {"goal", outcome.run.goal.value_or("")},
                {"cwd",
                 outcome.run.workingDirectory
                     ? Json(outcome.run.workingDirectory->value())
                     : Json(nullptr)},
                {"agent", agentSpecJson(outcome.agent, true)},
                {"first_moves", stringArray(outcome.run.firstMoves)},
                {"done_definition", stringArray(outcome.agent.doneDefinition)},
                {"output_schema", stringArray(outcome.run.outputSchema)},
                {"tools_primary", stringArray(outcome.agent.tools)},
                {"tools_forbidden", stringArray(outcome.agent.toolsForbidden)},
                {"superseded_sessions", outcome.supersededSessions},
                {"must_complete", outcome.mustComplete},
                {"next", std::move(next)},
                {"token_policy",
                 "Large context host: do not skip specialists to save tokens."}});
        }
        if (name == "agent_run_status") {
            const auto encodedSessionId =
                strictString(arguments, "session_id");
            if (!encodedSessionId) {
                return failure<Json>(
                    "missing_session_id",
                    "session_id required",
                    true);
            }
            auto sessionId = Domain::SessionId::parse(*encodedSessionId);
            if (!sessionId) {
                return propagate<Json>(std::move(sessionId));
            }
            auto status = dependencies_.agentSessions.runStatus(
                Domain::AgentRunStatusRequest{
                    std::move(sessionId).value(), call.clientId()},
                authority,
                call,
                context);
            if (!status) {
                return propagate<Json>(std::move(status));
            }
            const auto& outcome = status.value();
            Json result{
                {"ok", true},
                {"session",
                 outcome.run
                     ? agentSessionJson(outcome.run->session)
                     : Json(nullptr)},
                {"must_complete", outcome.mustComplete},
                {"idle_sec",
                 outcome.idleSeconds
                     ? Json(*outcome.idleSeconds)
                     : Json(nullptr)},
                {"abandon_risk", outcome.abandonRisk},
                {"reattached", outcome.reattached},
                {"active_binding",
                 outcome.activeBinding
                     ? activeBindingJson(*outcome.activeBinding)
                     : Json(nullptr)}};
            if (outcome.run && outcome.mustComplete) {
                std::string reminder =
                    "Session " + outcome.run->session.id.value() +
                    " is still OPEN. You MUST call agent_run_complete before finishing.";
                if (outcome.abandonRisk && outcome.idleSeconds) {
                    reminder += " Idle ~" +
                        std::to_string(*outcome.idleSeconds) +
                        "s - high risk of auto-close.";
                }
                result["reminder"] = std::move(reminder);
            }
            return Domain::Result<Json>::success(std::move(result));
        }
        if (name == "agent_run_complete") {
            const auto encodedSessionId =
                strictString(arguments, "session_id");
            if (!encodedSessionId) {
                return failure<Json>(
                    "missing_session_id",
                    "session_id required",
                    true);
            }
            auto sessionId = Domain::SessionId::parse(*encodedSessionId);
            if (!sessionId) {
                return propagate<Json>(std::move(sessionId));
            }
            Json report = Json::object();
            if (const auto* supplied = member(arguments, "report")) {
                if (!supplied->is_object()) {
                    return failure<Json>(
                        Domain::ErrorCodes::InvalidRequest,
                        "report must be an object.");
                }
                report = *supplied;
            }
            McpJsonCodec codec;
            auto canonical = codec.canonicalize(report.dump());
            if (!canonical) {
                return propagate<Json>(std::move(canonical));
            }
            auto fields = dependencies_.reportInspector.inspect(
                canonical.value(), context);
            if (!fields) {
                return propagate<Json>(std::move(fields));
            }
            auto completed = dependencies_.agentSessions.completeRun(
                Domain::AgentRunCompleteRequest{
                    std::move(sessionId).value(),
                    call.clientId(),
                    Domain::AgentCompletionReport{
                        canonical.value(), std::move(fields).value()}},
                context);
            if (!completed) {
                return propagate<Json>(std::move(completed));
            }
            const auto& outcome = completed.value();
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"session", agentSessionJson(outcome.run.session)},
                {"report", std::move(report)},
                {"schema_complete", outcome.schemaComplete},
                {"missing_schema_keys",
                 stringArray(outcome.missingSchemaKeys)},
                {"message",
                 outcome.schemaComplete
                     ? "Run complete."
                     : "Run complete with missing report keys: [" +
                           [&]() {
                               std::string joined;
                               for (std::size_t index{};
                                    index < outcome.missingSchemaKeys.size();
                                    ++index) {
                                   if (index != 0U) {
                                       joined.append(", ");
                                   }
                                   joined.append(outcome.missingSchemaKeys[index]);
                               }
                               return joined;
                           }() +
                           "]. Fill output_schema next time."}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested agent tool is not supported.");
    }

    [[nodiscard]] Domain::Result<Json> fileSystem(
        const std::string_view name,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        const auto suppliedPath = legacyString(arguments, "path");
        if (name != "fs_list" && name != "fs_glob" && name != "fs_move" &&
            !suppliedPath) {
            if (name == "fs_write") {
                return failure<Json>(
                    "missing_args",
                    "path and content required");
            }
            if (name == "fs_edit") {
                return failure<Json>(
                    "missing_args",
                    "path, old, new required");
            }
            return failure<Json>(
                "missing_path",
                "path required");
        }
        const auto encodedPath = suppliedPath.value_or(defaultRoot(authority));
        if (encodedPath.empty()) {
            return failure<Json>(
                Domain::ErrorCodes::InvalidRequest,
                "A workspace path is required.");
        }
        if (name == "fs_read") {
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!authorized) {
                return Domain::Result<Json>::failure(remapFsReadError(
                    std::move(authorized).error()));
            }
            auto bytes = dependencies_.fileSystem.readFile(
                authorized.value(), MaximumTextFileBytes, context);
            if (!bytes) {
                return Domain::Result<Json>::failure(remapFsReadError(
                    std::move(bytes).error()));
            }
            const auto* data = reinterpret_cast<const char*>(bytes.value().data());
            std::string content{data, bytes.value().size()};
            if (!Domain::isValidUtf8(content)) {
                return failure<Json>(
                    "not_found",
                    "The requested file is not valid UTF-8 text.");
            }
            std::vector<std::string_view> lines;
            if (!content.empty()) {
                std::size_t start{};
                while (start <= content.size()) {
                    const auto newline = content.find('\n', start);
                    const auto end = newline == std::string::npos
                        ? content.size()
                        : newline;
                    auto line = std::string_view{content}.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1U);
                    }
                    lines.push_back(line);
                    if (newline == std::string::npos) {
                        break;
                    }
                    start = newline + 1U;
                }
            }
            const auto byteOffset = legacyInteger(arguments, "byte_offset");
            if (byteOffset) {
                if (*byteOffset < 0 ||
                    static_cast<std::uint64_t>(*byteOffset) > content.size()) {
                    return failure<Json>(
                        Domain::ErrorCodes::InvalidRequest,
                        "byte_offset must identify a byte within the file.");
                }
                const auto pageStart = static_cast<std::size_t>(*byteOffset);
                if (!isUtf8Boundary(content, pageStart)) {
                    return failure<Json>(
                        Domain::ErrorCodes::InvalidRequest,
                        "byte_offset must be aligned to a UTF-8 code-point boundary.");
                }
                const auto pageEnd = boundedUtf8End(
                    content, pageStart, MaximumMcpTextContentBytes);
                std::string selected{
                    content.data() + pageStart, pageEnd - pageStart};
                const auto priorNewlines = static_cast<std::size_t>(
                    std::count(content.begin(), content.begin() +
                        static_cast<std::ptrdiff_t>(pageStart), '\n'));
                const auto pageNewlines = static_cast<std::size_t>(
                    std::count(selected.begin(), selected.end(), '\n'));
                const auto startLine = priorNewlines + 1U;
                const auto endLine = selected.empty()
                    ? startLine - 1U
                    : startLine + pageNewlines -
                        (selected.ends_with('\n') ? 1U : 0U);
                const bool hasMore = pageEnd < content.size();
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"path", authorized.value().canonicalPath().value()},
                    {"content", std::move(selected)},
                    {"size", bytes.value().size()},
                    {"total_lines", lines.size()},
                    {"start_line", startLine},
                    {"end_line", endLine},
                    {"line_count", endLine >= startLine
                         ? endLine - startLine + 1U
                         : 0U},
                    {"has_more", hasMore},
                    {"next_offset", Json(nullptr)},
                    {"byte_offset", pageStart},
                    {"next_byte_offset",
                     hasMore ? Json(pageEnd) : Json(nullptr)},
                    {"note",
                     hasMore
                         ? "Partial UTF-8 byte page. Continue with byte_offset=" +
                               std::to_string(pageEnd) +
                               ". Do not repeat the same byte_offset."
                         : "Reached end of file. Stop paginating this path."}});
            }

            auto offset = legacyInteger(arguments, "offset");
            if (!offset) {
                offset = legacyInteger(arguments, "start_line");
            }
            auto length = legacyInteger(arguments, "length");
            if (!length) {
                length = legacyInteger(arguments, "limit");
            }
            if (!length) {
                length = legacyInteger(arguments, "max_lines");
            }
            const auto startLineSigned = offset ? (std::max)(*offset, 1LL) : 1LL;
            const auto requestedSigned = offset
                ? (std::max)(length.value_or(DefaultReadWindowLines), 0LL)
                : length
                    ? (std::max)(*length, 0LL)
                    : static_cast<std::int64_t>(lines.size());
            const auto first = static_cast<std::size_t>(startLineSigned - 1LL);
            const auto requested = static_cast<std::size_t>(requestedSigned);
            const std::size_t requestedLast = first >= lines.size()
                ? first
                : first + (std::min)(requested, lines.size() - first);
            std::string selected;
            std::size_t last = first;
            std::optional<std::size_t> pageByteStart;
            std::optional<std::size_t> nextByteOffset;
            for (std::size_t index = first; index < requestedLast; ++index) {
                const auto separatorBytes = index == first ? 0U : 1U;
                const auto requiredBytes = separatorBytes + lines[index].size();
                if (requiredBytes >
                    MaximumMcpTextContentBytes - selected.size()) {
                    if (selected.empty()) {
                        pageByteStart = static_cast<std::size_t>(
                            lines[index].data() - content.data());
                        const auto pageEnd = boundedUtf8End(
                            content,
                            *pageByteStart,
                            MaximumMcpTextContentBytes);
                        selected.assign(
                            content.data() + *pageByteStart,
                            pageEnd - *pageByteStart);
                        nextByteOffset = pageEnd;
                    }
                    break;
                }
                if (separatorBytes != 0U) {
                    selected.push_back('\n');
                }
                selected.append(lines[index]);
                last = index + 1U;
            }
            const bool bytePage = pageByteStart.has_value();
            const bool hasMore = bytePage
                ? nextByteOffset.value_or(content.size()) < content.size()
                : last < lines.size();
            const auto endLine = bytePage
                ? static_cast<std::size_t>(startLineSigned)
                : last;
            std::string note;
            if (bytePage) {
                note = hasMore
                    ? "Oversized UTF-8 line returned as a bounded byte page. Continue with byte_offset=" +
                          std::to_string(*nextByteOffset) +
                          ". Do not repeat the same byte_offset."
                    : "Reached end of file. Stop paginating this path.";
            } else if (hasMore) {
                note = "Partial read (lines " + std::to_string(startLineSigned) +
                    "-" + std::to_string(endLine) + " of " +
                    std::to_string(lines.size()) + "). Continue with offset=" +
                    std::to_string(endLine + 1U) +
                    " (1-based) and a new length. Do not repeat the same offset/length.";
            } else if (lines.empty()) {
                note = "File is empty.";
            } else if (first >= lines.size()) {
                note = "offset " + std::to_string(startLineSigned) +
                    " is past end of file (" + std::to_string(lines.size()) +
                    " lines). Stop paginating this path.";
            } else if (startLineSigned == 1LL) {
                note = "Complete file contents (" + std::to_string(lines.size()) +
                    " lines). Do not re-read this path unless the file changes.";
            } else {
                note = "Reached end of file at line " +
                    std::to_string(lines.size()) +
                    ". Stop paginating this path.";
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"content", std::move(selected)},
                {"size", bytes.value().size()},
                {"total_lines", lines.size()},
                {"start_line", startLineSigned},
                {"end_line", endLine},
                {"line_count", bytePage
                     ? (selected.empty() ? 0U : 1U)
                     : last - first},
                {"has_more", hasMore},
                {"next_offset", !bytePage && hasMore
                     ? Json(last + 1U)
                     : Json(nullptr)},
                {"byte_offset",
                 bytePage ? Json(*pageByteStart) : Json(nullptr)},
                {"next_byte_offset",
                 bytePage && hasMore ? Json(*nextByteOffset) : Json(nullptr)},
                {"note", std::move(note)}});
        }
        if (name == "fs_write") {
            const auto suppliedContent = legacyString(arguments, "content");
            if (!suppliedContent) {
                return failure<Json>(
                    "missing_args",
                    "path and content required");
            }
            const auto& content = *suppliedContent;
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Write,
                false,
                context,
                &observation);
            if (!authorized) {
                return propagate<Json>(std::move(authorized));
            }
            const auto bytes = std::as_bytes(std::span{content.data(), content.size()});
            auto written = dependencies_.fileSystem.writeFile(
                authorized.value(), bytes, context);
            if (!written &&
                written.error().code == Domain::ErrorCodes::RecordNotFound) {
                auto creatable = authorizePath(
                    dependencies_.workspaceAuthority,
                    authority,
                    encodedPath,
                    Domain::FileAccess::Create,
                    false,
                    context,
                    &observation);
                if (!creatable) {
                    return propagate<Json>(std::move(creatable));
                }
                auto created = dependencies_.fileSystem.writeFile(
                    creatable.value(), bytes, context);
                if (!created) {
                    return propagate<Json>(std::move(created));
                }
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"path", creatable.value().canonicalPath().value()},
                    {"bytes_written", content.size()}});
            }
            if (!written) {
                return propagate<Json>(std::move(written));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"bytes_written", content.size()}});
        }
        if (name == "fs_edit") {
            const auto oldText = legacyString(arguments, "old");
            const auto replacement = legacyString(arguments, "new");
            if (!oldText || !replacement) {
                return failure<Json>(
                    "missing_args",
                    "path, old, new required");
            }
            auto readable = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!readable) {
                return Domain::Result<Json>::failure(remapFsEditError(
                    std::move(readable).error()));
            }
            auto writable = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Write,
                false,
                context,
                &observation);
            if (!writable) {
                return Domain::Result<Json>::failure(remapFsEditError(
                    std::move(writable).error()));
            }
            auto edited = dependencies_.textFileEditor.replaceAll(
                readable.value(), writable.value(), *oldText, *replacement, context);
            if (!edited) {
                return Domain::Result<Json>::failure(remapFsEditError(
                    std::move(edited).error()));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", writable.value().canonicalPath().value()},
                {"replacements", edited.value().replacements},
                {"bytes_written", edited.value().bytesWritten}});
        }
        if (name == "fs_list") {
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!authorized) {
                return propagate<Json>(std::move(authorized));
            }
            auto listing = dependencies_.fileSystem.list(
                authorized.value(), MaximumDirectoryEntries, context);
            if (!listing) {
                return propagate<Json>(std::move(listing));
            }
            Json entries = Json::array();
            for (const auto& entry : listing.value().entries) {
                const auto& value = entry.value();
                const auto separator = value.find_last_of("/\\");
                entries.push_back(separator == std::string::npos
                    ? value
                    : value.substr(separator + 1U));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"entries", std::move(entries)},
                {"truncated", listing.value().truncated},
                {"maximum_entries", MaximumDirectoryEntries}});
        }
        if (name == "fs_glob") {
            const auto pattern = legacyString(arguments, "pattern").value_or("*");
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!authorized) {
                return propagate<Json>(std::move(authorized));
            }
            auto matches = dependencies_.pathGlob.glob(
                authorized.value(),
                pattern,
                MaximumGlobMatches,
                MaximumNativeResponseBytes,
                context);
            if (!matches) {
                return propagate<Json>(std::move(matches));
            }
            Json paths = Json::array();
            for (const auto& path : matches.value()) {
                paths.push_back(path.value());
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"pattern", pattern},
                {"matches", std::move(paths)}});
        }
        if (name == "fs_mkdir") {
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Create,
                false,
                context,
                &observation);
            if (!authorized) {
                return propagate<Json>(std::move(authorized));
            }
            auto created = dependencies_.fileSystem.createDirectory(
                authorized.value(), context);
            if (!created) {
                return propagate<Json>(std::move(created));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"created", true}});
        }
        if (name == "fs_delete") {
            auto authorized = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                encodedPath,
                Domain::FileAccess::Delete,
                true,
                context,
                &observation);
            if (!authorized) {
                return propagate<Json>(std::move(authorized));
            }
            auto removed = dependencies_.fileSystem.remove(
                authorized.value(), true, context);
            if (!removed) {
                return propagate<Json>(std::move(removed));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"deleted", true}});
        }
        if (name == "fs_move") {
            auto source = legacyString(arguments, "path");
            if (!source) {
                source = legacyString(arguments, "src");
            }
            if (!source) {
                source = legacyString(arguments, "source");
            }
            auto destination = legacyString(arguments, "dest");
            if (!destination) {
                destination = legacyString(arguments, "destination");
            }
            if (!source || !destination) {
                return failure<Json>(
                    "missing_args",
                    "path/src and dest required");
            }
            auto authorizedSource = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                *source,
                Domain::FileAccess::Delete,
                true,
                context,
                &observation);
            if (!authorizedSource) {
                return propagate<Json>(std::move(authorizedSource));
            }
            auto authorizedDestination = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                *destination,
                Domain::FileAccess::Create,
                false,
                context,
                &observation);
            if (!authorizedDestination) {
                return propagate<Json>(std::move(authorizedDestination));
            }
            auto moved = dependencies_.fileSystem.move(
                authorizedSource.value(), authorizedDestination.value(), context);
            if (!moved) {
                return propagate<Json>(std::move(moved));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"src", authorizedSource.value().canonicalPath().value()},
                {"dest", authorizedDestination.value().canonicalPath().value()}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested filesystem tool is not supported.");
    }

    [[nodiscard]] Domain::Result<Json> git(
        const std::string_view name,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        const auto repository = legacyString(arguments, "cwd").value_or(
            defaultRoot(authority));
        const auto access = name == "git_add" || name == "git_commit"
            ? Domain::FileAccess::Write
            : Domain::FileAccess::Read;
        auto authorizedRepository = authorizePath(
            dependencies_.workspaceAuthority,
            authority,
            repository,
            access,
            false,
            context,
            &observation,
            ContinuityPathRole::WorkingDirectory);
        if (!authorizedRepository) {
            return propagate<Json>(std::move(authorizedRepository));
        }
        Domain::Result<Domain::ProcessResult> result =
            failure<Domain::ProcessResult>(
                Domain::ErrorCodes::InvalidRequest,
                "The requested Git tool is not supported.");
        if (name == "git_status") {
            result = dependencies_.git.status(
                authorizedRepository.value(), authority,
                MaximumGitOutputBytes, context);
        } else if (name == "git_diff") {
            std::vector<std::string> extra;
            if (strictBoolean(arguments, "staged").value_or(false)) {
                extra.emplace_back("--cached");
            }
            result = dependencies_.git.diff(
                authorizedRepository.value(), authority, extra,
                MaximumGitOutputBytes, context);
        } else if (name == "git_log") {
            const auto encodedLimit = legacyInteger(arguments, "limit").value_or(20);
            if (encodedLimit < 1) {
                return failure<Json>(
                    Domain::ErrorCodes::InvalidRequest,
                    "Git log limit must be positive.");
            }
            const auto limit = (std::min)(
                static_cast<std::size_t>(encodedLimit), MaximumGitLogEntries);
            result = dependencies_.git.log(
                authorizedRepository.value(), authority, limit,
                MaximumGitOutputBytes, context);
        } else if (name == "git_add") {
            std::vector<Contracts::AuthorizedPath> paths;
            const auto requested = legacyString(arguments, "path");
            if (requested && *requested != "-A") {
                const auto anchored = anchoredToolPath(
                    *requested,
                    authorizedRepository.value().canonicalPath().value());
                auto authorizedPath = authorizePath(
                    dependencies_.workspaceAuthority,
                    authority,
                    anchored,
                    Domain::FileAccess::Read,
                    false,
                    context,
                    &observation);
                if (!authorizedPath) {
                    return propagate<Json>(std::move(authorizedPath));
                }
                paths.push_back(std::move(authorizedPath).value());
            }
            result = dependencies_.git.add(
                authorizedRepository.value(), authority, paths, context);
        } else if (name == "git_commit") {
            result = dependencies_.git.commit(
                authorizedRepository.value(),
                authority,
                legacyString(arguments, "message").value_or(
                    "chore: forge-conductor commit"),
                context);
        }
        if (!result) {
            return propagate<Json>(std::move(result));
        }
        return Domain::Result<Json>::success(processJson(
            result.value(), authorizedRepository.value().canonicalPath().value()));
    }

    [[nodiscard]] Domain::Result<Json> pdf(
        const std::string_view name,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "pdf_write") {
            const auto requestedPath = legacyString(arguments, "path");
            const auto content = legacyString(arguments, "content");
            if (!requestedPath || !content) {
                return failure<Json>(
                    "missing_args",
                    "path and content required");
            }
            const auto& requested = *requestedPath;
            const auto destinationText = pdfPath(requested);
            auto destination = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                destinationText,
                Domain::FileAccess::Create,
                false,
                context,
                &observation);
            if (!destination) {
                return propagate<Json>(std::move(destination));
            }
            const auto title = legacyString(arguments, "title").value_or(
                fileNameWithoutExtension(destinationText));
            auto written = dependencies_.pdf.write(
                title,
                *content,
                destination.value(),
                context);
            if (!written) {
                return propagate<Json>(std::move(written));
            }
            return Domain::Result<Json>::success(pdfJson(written.value()));
        }
        if (name == "pdf_from_file") {
            const auto requestedSource = legacyString(arguments, "source_path");
            if (!requestedSource) {
                return failure<Json>(
                    "missing_source",
                    "source_path required");
            }
            const auto& sourceText = *requestedSource;
            auto source = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                sourceText,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!source) {
                return Domain::Result<Json>::failure(remapPdfFromFileError(
                    std::move(source).error()));
            }
            auto destinationText = legacyString(arguments, "dest_path");
            if (!destinationText || destinationText->empty()) {
                destinationText = replaceExtensionWithPdf(sourceText);
            }
            auto destination = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                *destinationText,
                Domain::FileAccess::Create,
                false,
                context,
                &observation);
            if (!destination) {
                return propagate<Json>(std::move(destination));
            }
            const auto title = legacyString(arguments, "title").value_or(
                fileNameWithoutExtension(sourceText));
            auto written = dependencies_.pdf.fromTextFile(
                title, source.value(), destination.value(), context);
            if (!written) {
                return Domain::Result<Json>::failure(remapPdfFromFileError(
                    std::move(written).error()));
            }
            auto payload = pdfJson(written.value());
            payload["source_path"] = source.value().canonicalPath().value();
            return Domain::Result<Json>::success(std::move(payload));
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested PDF tool is not supported.");
    }

    [[nodiscard]] Domain::Result<Json> search(
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        const auto query = legacyString(arguments, "pattern");
        if (!query) {
            return failure<Json>(
                "missing_pattern",
                "pattern required");
        }
        const auto rootText = legacyString(arguments, "path").value_or(
            defaultRoot(authority));
        auto root = authorizePath(
            dependencies_.workspaceAuthority,
            authority,
            rootText,
            Domain::FileAccess::Read,
            false,
            context,
            &observation);
        if (!root) {
            return propagate<Json>(std::move(root));
        }
        auto matches = dependencies_.textSearch.search(
            root.value(),
            *query,
            MaximumSearchMatches,
            MaximumNativeResponseBytes,
            context);
        if (!matches) {
            return propagate<Json>(std::move(matches));
        }
        return Domain::Result<Json>::success(Json{
            {"ok", true},
            {"pattern", *query},
            {"matches", stringArray(matches.value())},
            {"count", matches.value().size()}});
    }

    [[nodiscard]] Domain::Result<Json> shell(
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        const auto command = legacyString(arguments, "command").value_or("");
        if (command.empty()) {
            return failure<Json>(
                "missing_command",
                "command required");
        }
        const auto cwdText = legacyString(arguments, "cwd").value_or(
            defaultRoot(authority));
        auto cwd = authorizePath(
            dependencies_.workspaceAuthority,
            authority,
            cwdText,
            Domain::FileAccess::Execute,
            false,
            context,
            &observation,
            ContinuityPathRole::WorkingDirectory);
        if (!cwd) {
            return propagate<Json>(std::move(cwd));
        }
        auto timeoutSeconds = strictNumber(arguments, "timeout_sec").value_or(
            static_cast<double>(dependencies_.shellDefaultTimeout.count()));
        if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0.0) {
            return failure<Json>(
                "invalid_timeout",
                "timeout_sec must be finite and positive");
        }
        timeoutSeconds = (std::min)(timeoutSeconds, 120.0);
        const auto timeout = std::chrono::milliseconds{
            static_cast<std::int64_t>(std::ceil(timeoutSeconds * 1000.0))};
        Domain::ProcessRequest request{
            dependencies_.shellExecutable,
            {command},
            cwd.value().canonicalPath(),
            {},
            false,
            timeout,
            MaximumShellOutputBytes,
            MaximumShellErrorBytes};
        auto result = dependencies_.shell.execute(request, authority, context);
        if (!result) {
            return propagate<Json>(std::move(result));
        }
        auto payload = processJson(
            result.value(), cwd.value().canonicalPath().value());
        payload["command"] = command;
        return Domain::Result<Json>::success(std::move(payload));
    }

    template <typename T>
    [[nodiscard]] Domain::Result<std::optional<T>> optionalStrongUuid(
        const Json& arguments,
        const std::string_view key)
    {
        const auto encoded = strictString(arguments, key);
        if (!encoded) {
            return Domain::Result<std::optional<T>>::success(std::nullopt);
        }
        auto parsed = T::parse(*encoded);
        if (!parsed) {
            return propagate<std::optional<T>>(std::move(parsed));
        }
        return Domain::Result<std::optional<T>>::success(
            std::move(parsed).value());
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::IdempotencyKey>>
    optionalIdempotencyKey(
        const Json& arguments,
        const std::string_view key)
    {
        const auto encoded = strictString(arguments, key);
        if (!encoded) {
            return Domain::Result<std::optional<Domain::IdempotencyKey>>::success(
                std::nullopt);
        }
        auto parsed = Domain::IdempotencyKey::create(*encoded);
        if (!parsed) {
            return propagate<std::optional<Domain::IdempotencyKey>>(
                std::move(parsed));
        }
        return Domain::Result<std::optional<Domain::IdempotencyKey>>::success(
            std::move(parsed).value());
    }

    [[nodiscard]] Domain::Result<Domain::ProjectId> requiredProject(
        const Contracts::AuthorizedToolCall& call,
        const Json& arguments)
    {
        auto parsed = parseStrongUuid<Domain::ProjectId>(arguments, "project_id");
        if (!parsed) {
            return parsed;
        }
        if (!call.matchesProject(parsed.value())) {
            return failure<Domain::ProjectId>(
                Domain::ErrorCodes::ProjectScopeMismatch,
                "The project_id argument does not match the authorized project.");
        }
        return parsed;
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryWrite> projectWrite(
        const Json& arguments)
    {
        try {
            Domain::ProjectMemoryWrite write;
            write.kind = strictString(arguments, "kind").value_or("");
            write.title = strictString(arguments, "title").value_or("");
            write.summary = strictString(arguments, "summary").value_or("");
            write.body = strictString(arguments, "body");
            auto tags = strictStrings(arguments, "tags");
            if (!tags) {
                return propagate<Domain::ProjectMemoryWrite>(std::move(tags));
            }
            write.tags = std::move(tags).value();
            write.importance = strictNumber(arguments, "importance").value_or(0.5);
            write.confidence = strictNumber(arguments, "confidence").value_or(1.0);
            write.sourceKind = strictString(arguments, "source_kind").value_or(
                "external_integration");
            write.sourceReference = strictString(arguments, "source_reference");
            auto session = optionalStrongUuid<Domain::SessionId>(
                arguments, "session_id");
            if (!session) {
                return propagate<Domain::ProjectMemoryWrite>(std::move(session));
            }
            write.sessionId = std::move(session).value();
            if (const auto expires = strictString(arguments, "expires_at")) {
                auto parsed = parseTimestamp(*expires);
                if (!parsed) {
                    return propagate<Domain::ProjectMemoryWrite>(
                        std::move(parsed));
                }
                write.expiresAt = std::move(parsed).value();
            }
            const auto* related = member(arguments, "related_ids");
            if (related != nullptr) {
                if (!related->is_array()) {
                    return failure<Domain::ProjectMemoryWrite>(
                        Domain::ErrorCodes::InvalidRequest,
                        "related_ids must be an array of record UUIDs.");
                }
                for (const auto& value : *related) {
                    if (!value.is_string()) {
                        return failure<Domain::ProjectMemoryWrite>(
                            Domain::ErrorCodes::InvalidRequest,
                            "related_ids must contain only record UUIDs.");
                    }
                    auto parsed = Domain::MemoryRecordId::parse(
                        value.get_ref<const std::string&>());
                    if (!parsed) {
                        return propagate<Domain::ProjectMemoryWrite>(
                            std::move(parsed));
                    }
                    write.relatedIds.push_back(std::move(parsed).value());
                }
            }
            auto idempotency = optionalIdempotencyKey(
                arguments, "idempotency_key");
            if (!idempotency) {
                return propagate<Domain::ProjectMemoryWrite>(
                    std::move(idempotency));
            }
            write.idempotencyKey = std::move(idempotency).value();
            return Domain::validateProjectMemoryWrite(
                std::move(write), dependencies_.projectMemoryLimits);
        } catch (...) {
            return failure<Domain::ProjectMemoryWrite>(
                Domain::ErrorCodes::InternalFailure,
                "The project-memory write could not be decoded.");
        }
    }

    [[nodiscard]] Domain::Result<Json> projectMemory(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "project_memory.initialize") {
            const auto projectPath = strictString(arguments, "project_path").value_or("");
            auto authorizedPath = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                projectPath,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!authorizedPath) {
                return propagate<Json>(std::move(authorizedPath));
            }
            auto requestedProject = optionalStrongUuid<Domain::ProjectId>(
                arguments, "project_id");
            if (!requestedProject) {
                return propagate<Json>(std::move(requestedProject));
            }
            if (requestedProject.value() && call.projectId() &&
                *requestedProject.value() != *call.projectId()) {
                return failure<Json>(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The requested project identifier does not match authorization.");
            }
            auto idempotency = optionalIdempotencyKey(
                arguments, "idempotency_key");
            if (!idempotency) {
                return propagate<Json>(std::move(idempotency));
            }
            auto initialized = dependencies_.projectMemory.initialize(
                Domain::InitializeProjectRequest{
                    authorizedPath.value().canonicalPath(),
                    std::move(requestedProject).value(),
                    strictString(arguments, "display_name"),
                    strictString(arguments, "repository_identity"),
                    std::move(idempotency).value()},
                context);
            if (!initialized) {
                return propagate<Json>(std::move(initialized));
            }
            const auto& outcome = initialized.value();
            Json capabilities = Json::array({
                "lexical_search",
                "transactions",
                "redaction",
                "exports",
                outcome.fullTextSearchAvailable
                    ? "fts5"
                    : "bounded_sql_fallback"});
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.project.id.value()},
                {"project", projectDescriptorJson(outcome.project)},
                {"schema_version", outcome.schemaVersion},
                {"capability_version", outcome.capabilityVersion},
                {"capabilities", std::move(capabilities)},
                {"limits", projectMemoryLimitsJson(outcome.limits)},
                {"lexical_search_available", outcome.lexicalSearchAvailable},
                {"full_text_search_available", outcome.fullTextSearchAvailable},
                {"migration_current", outcome.migrationCurrent},
                {"migration_status",
                 outcome.migrationCurrent ? "current" : "required"}});
        }

        auto project = requiredProject(call, arguments);
        if (!project) {
            return propagate<Json>(std::move(project));
        }
        if (name == "project_memory.remember") {
            auto write = projectWrite(arguments);
            if (!write) {
                return propagate<Json>(std::move(write));
            }
            auto outcome = dependencies_.projectMemory.remember(
                Domain::RememberProjectMemoryRequest{
                    project.value(), std::move(write).value()},
                context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(memoryWriteJson(outcome.value()));
        }
        if (name == "project_memory.remember_batch") {
            const auto* items = member(arguments, "items");
            if (items == nullptr || !items->is_array()) {
                return failure<Json>(
                    Domain::ErrorCodes::InvalidRequest,
                    "items must be an array of project-memory writes.");
            }
            std::vector<Domain::ProjectMemoryWrite> writes;
            writes.reserve(items->size());
            for (const auto& item : *items) {
                if (!item.is_object()) {
                    return failure<Json>(
                        Domain::ErrorCodes::InvalidRequest,
                        "Each batch item must be an object.");
                }
                auto write = projectWrite(item);
                if (!write) {
                    return propagate<Json>(std::move(write));
                }
                writes.push_back(std::move(write).value());
            }
            auto valid = Domain::validateProjectMemoryBatch(
                std::move(writes), dependencies_.projectMemoryLimits);
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            auto outcome = dependencies_.projectMemory.rememberBatch(
                Domain::RememberProjectMemoryBatchRequest{
                    project.value(), std::move(valid).value()},
                context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            Json results = Json::array();
            for (const auto& item : outcome.value().results) {
                results.push_back(memoryWriteJson(item));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"count", results.size()},
                {"results", std::move(results)},
                {"schema_version", outcome.value().schemaVersion},
                {"capability_version", outcome.value().capabilityVersion}});
        }
        if (name == "project_memory.search") {
            auto kinds = strictStrings(arguments, "kinds");
            auto tags = strictStrings(arguments, "tags");
            auto session = optionalStrongUuid<Domain::SessionId>(
                arguments, "session_id");
            if (!kinds) {
                return propagate<Json>(std::move(kinds));
            }
            if (!tags) {
                return propagate<Json>(std::move(tags));
            }
            if (!session) {
                return propagate<Json>(std::move(session));
            }
            const auto requestedLimit = strictInteger(arguments, "limit").value_or(
                static_cast<std::int64_t>(
                    dependencies_.projectMemoryLimits.defaultPageCount));
            const auto requestedBytes = strictInteger(
                arguments, "maximum_response_bytes").value_or(
                    static_cast<std::int64_t>(
                        dependencies_.projectMemoryLimits.defaultResponseBytes));
            if (requestedLimit < 0 || requestedBytes < 0) {
                return failure<Json>(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory response limits may not be negative.");
            }
            Domain::SearchProjectMemoryRequest request{
                project.value(),
                strictString(arguments, "query").value_or(""),
                std::move(kinds).value(),
                std::move(tags).value(),
                std::move(session).value(),
                Domain::normalizeProjectMemoryPageLimit(
                    static_cast<std::size_t>(requestedLimit),
                    dependencies_.projectMemoryLimits),
                strictString(arguments, "cursor"),
                strictBoolean(arguments, "include_body").value_or(false),
                Domain::normalizeProjectMemoryResponseLimit(
                    static_cast<std::size_t>(requestedBytes),
                    dependencies_.projectMemoryLimits)};
            auto valid = Domain::validateSearchProjectMemoryRequest(
                std::move(request), dependencies_.projectMemoryLimits);
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            const bool includeBody = valid.value().includeBody;
            auto outcome = dependencies_.projectMemory.search(
                valid.value(), context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            auto payload = memoryPageJson(outcome.value(), includeBody);
            payload["query"] = valid.value().query;
            payload["ranking"] = Json::array({
                "exact_id",
                "exact_title",
                "lexical_title",
                "summary",
                "body",
                "importance",
                "confidence"});
            return Domain::Result<Json>::success(std::move(payload));
        }
        if (name == "project_memory.get") {
            std::vector<Domain::MemoryRecordId> ids;
            if (const auto encoded = strictString(arguments, "id")) {
                auto parsed = Domain::MemoryRecordId::parse(*encoded);
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                ids.push_back(std::move(parsed).value());
            }
            const auto* encodedIds = member(arguments, "ids");
            if (encodedIds != nullptr) {
                if (!encodedIds->is_array()) {
                    return failure<Json>(
                        Domain::ErrorCodes::InvalidRequest,
                        "ids must be an array of record UUIDs.");
                }
                for (const auto& item : *encodedIds) {
                    if (!item.is_string()) {
                        return failure<Json>(
                            Domain::ErrorCodes::InvalidRequest,
                            "ids must contain only record UUIDs.");
                    }
                    auto parsed = Domain::MemoryRecordId::parse(
                        item.get_ref<const std::string&>());
                    if (!parsed) {
                        return propagate<Json>(std::move(parsed));
                    }
                    ids.push_back(std::move(parsed).value());
                }
            }
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            const bool includeBody =
                strictBoolean(arguments, "include_body").value_or(false);
            Domain::GetProjectMemoryRequest request{
                project.value(),
                std::move(ids),
                includeBody,
                dependencies_.projectMemoryLimits.defaultResponseBytes};
            auto valid = Domain::validateGetProjectMemoryRequest(
                request, dependencies_.projectMemoryLimits);
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            auto outcome = dependencies_.projectMemory.get(request, context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            Json records = Json::array();
            for (const auto& record : outcome.value().records) {
                records.push_back(projectRecordJson(record, includeBody));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"count", records.size()},
                {"records", std::move(records)},
                {"encoded_bytes", outcome.value().encodedBytes},
                {"maximum_response_bytes", outcome.value().maximumResponseBytes},
                {"schema_version", outcome.value().schemaVersion},
                {"capability_version", outcome.value().capabilityVersion}});
        }

        return projectMemoryMutation(
            name,
            call,
            authority,
            project.value(),
            arguments,
            context,
            observation);
    }

    [[nodiscard]] Domain::Result<Json> projectMemoryMutation(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::ProjectId& project,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "project_memory.update") {
            auto id = parseStrongUuid<Domain::MemoryRecordId>(arguments, "id");
            if (!id) {
                return propagate<Json>(std::move(id));
            }
            const auto encodedVersion = strictInteger(
                arguments, "expected_version").value_or(-1);
            if (encodedVersion < 0 ||
                static_cast<std::uint64_t>(encodedVersion) >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                return failure<Json>(
                    Domain::ErrorCodes::InvalidRequest,
                    "expected_version is outside the uint32 range.");
            }
            std::optional<std::vector<std::string>> tags;
            if (member(arguments, "tags") != nullptr) {
                auto decoded = strictStrings(arguments, "tags");
                if (!decoded) {
                    return propagate<Json>(std::move(decoded));
                }
                tags = std::move(decoded).value();
            }
            Domain::UpdateProjectMemoryRequest request{
                project,
                std::move(id).value(),
                static_cast<std::uint32_t>(encodedVersion),
                strictString(arguments, "title"),
                strictString(arguments, "summary"),
                strictString(arguments, "body"),
                std::move(tags)};
            auto valid = Domain::validateUpdateProjectMemoryRequest(
                std::move(request), dependencies_.projectMemoryLimits);
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            auto outcome = dependencies_.projectMemory.update(
                valid.value(), context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"record", projectRecordJson(outcome.value(), true)},
                {"schema_version", outcome.value().schemaVersion}});
        }
        if (name == "project_memory.forget") {
            auto id = parseStrongUuid<Domain::MemoryRecordId>(arguments, "id");
            if (!id) {
                return propagate<Json>(std::move(id));
            }
            auto outcome = dependencies_.projectMemory.forget(
                Domain::ForgetProjectMemoryRequest{
                    project, std::move(id).value()},
                context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"record_id", outcome.value().recordId.value()},
                {"disposition", Domain::wireName(outcome.value().disposition)}});
        }
        if (name == "project_memory.list_recent") {
            auto kinds = strictStrings(arguments, "kinds");
            auto session = optionalStrongUuid<Domain::SessionId>(
                arguments, "session_id");
            if (!kinds) {
                return propagate<Json>(std::move(kinds));
            }
            if (!session) {
                return propagate<Json>(std::move(session));
            }
            const auto requestedLimit = strictInteger(arguments, "limit").value_or(
                static_cast<std::int64_t>(
                    dependencies_.projectMemoryLimits.defaultPageCount));
            const auto requestedBytes = strictInteger(
                arguments, "maximum_response_bytes").value_or(
                    static_cast<std::int64_t>(
                        dependencies_.projectMemoryLimits.defaultResponseBytes));
            if (requestedLimit < 0 || requestedBytes < 0) {
                return failure<Json>(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project-memory response limits may not be negative.");
            }
            Domain::ListRecentProjectMemoryRequest request{
                project,
                std::move(kinds).value(),
                std::move(session).value(),
                Domain::normalizeProjectMemoryPageLimit(
                    static_cast<std::size_t>(requestedLimit),
                    dependencies_.projectMemoryLimits),
                strictString(arguments, "cursor"),
                strictBoolean(arguments, "include_body").value_or(false),
                Domain::normalizeProjectMemoryResponseLimit(
                    static_cast<std::size_t>(requestedBytes),
                    dependencies_.projectMemoryLimits)};
            auto valid = Domain::validateListRecentProjectMemoryRequest(
                std::move(request), dependencies_.projectMemoryLimits);
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            const bool includeBody = valid.value().includeBody;
            auto outcome = dependencies_.projectMemory.listRecent(
                valid.value(), context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(
                memoryPageJson(outcome.value(), includeBody));
        }

        return projectMemoryArtifact(
            name, call, authority, project, arguments, context, observation);
    }

    [[nodiscard]] Domain::Result<Json> projectMemoryArtifact(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::ProjectId& project,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "project_memory.link") {
            auto source = parseStrongUuid<Domain::MemoryRecordId>(
                arguments, "source_id");
            auto target = parseStrongUuid<Domain::MemoryRecordId>(
                arguments, "target_id");
            if (!source) {
                return propagate<Json>(std::move(source));
            }
            if (!target) {
                return propagate<Json>(std::move(target));
            }
            Domain::LinkProjectMemoryRequest request{
                project,
                std::move(source).value(),
                std::move(target).value(),
                strictString(arguments, "relation").value_or("")};
            auto valid = Domain::validateLinkProjectMemoryRequest(
                std::move(request));
            if (!valid) {
                return propagate<Json>(std::move(valid));
            }
            auto outcome = dependencies_.projectMemory.link(
                valid.value(), context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"disposition", Domain::wireName(outcome.value().disposition)}});
        }
        if (name == "project_memory.export") {
            auto outcome = dependencies_.projectMemory.exportMemory(
                Domain::ExportProjectMemoryRequest{project},
                authority,
                call,
                context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"artifact", outcome.value().artifact.value()},
                {"checksum", outcome.value().checksum.value()},
                {"record_count", outcome.value().recordCount}});
        }
        if (name == "project_memory.import") {
            const auto artifactText = strictString(arguments, "artifact").value_or("");
            auto artifact = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                artifactText,
                Domain::FileAccess::Read,
                false,
                context,
                &observation);
            if (!artifact) {
                return propagate<Json>(std::move(artifact));
            }
            const auto policy = strictString(arguments, "merge_policy").value_or("");
            Domain::ImportProjectMemoryRequest request{
                project,
                artifact.value().canonicalPath(),
                strictBoolean(arguments, "preview").value_or(true),
                policy == "merge"};
            auto outcome = dependencies_.projectMemory.importMemory(
                request, context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            Json imported = Json::array();
            for (const auto& item : outcome.value().imported) {
                imported.push_back(memoryWriteJson(item));
            }
            if (outcome.value().disposition !=
                Domain::ImportDisposition::Preview) {
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"project_id", outcome.value().projectId.value()},
                    {"count", imported.size()},
                    {"results", std::move(imported)},
                    {"schema_version", Domain::ProjectMemorySchemaVersion},
                    {"capability_version",
                     Domain::ProjectMemoryCapabilityVersion}});
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", outcome.value().projectId.value()},
                {"disposition", Domain::wireName(outcome.value().disposition)},
                {"preview",
                 outcome.value().disposition ==
                     Domain::ImportDisposition::Preview},
                {"record_count", outcome.value().recordCount},
                {"importable_count", outcome.value().importableCount},
                {"checksum", outcome.value().checksum.value()},
                {"imported", std::move(imported)}});
        }
        if (name == "project_memory.status") {
            auto outcome = dependencies_.projectMemory.status(
                Domain::ProjectMemoryStatusRequest{project}, context);
            if (!outcome) {
                return propagate<Json>(std::move(outcome));
            }
            const auto& status = outcome.value();
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", status.projectId.value()},
                {"schema_version", status.schemaVersion},
                {"capability_version", status.capabilityVersion},
                {"record_count", status.recordCount},
                {"tombstone_count", status.tombstoneCount},
                {"event_count", status.eventCount},
                {"database_bytes", status.databaseBytes},
                {"write_ahead_log_bytes", status.writeAheadLogBytes},
                {"full_text_search_available", status.fullTextSearchAvailable},
                {"integrity_ok", status.integrityOk},
                {"open_repositories", status.openRepositories},
                {"cache",
                 Json{{"open_repositories", status.openRepositories},
                      {"maximum", status.limits.maximumOpenProjects}}},
                {"limits", projectMemoryLimitsJson(status.limits)}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested project-memory tool is not supported.");
    }

    template <typename T>
    [[nodiscard]] Domain::Result<T> generatedStrongUuid()
    {
        auto generated = dependencies_.uuidGenerator.next();
        if (!generated) {
            return propagate<T>(std::move(generated));
        }
        return Domain::Result<T>::success(T{std::move(generated).value()});
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityHandoff> continuityHandoff(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::ProjectId& projectId,
        const Json& arguments,
        const Domain::OperationContext& context)
    {
        try {
            auto descriptor = dependencies_.projectRegistry.descriptor(
                projectId, context);
            if (!descriptor) {
                return propagate<Domain::ContinuityHandoff>(
                    std::move(descriptor));
            }
            auto operationId = optionalStrongUuid<Domain::ContinuityOperationId>(
                arguments, "operation_id");
            auto handoffId = optionalStrongUuid<Domain::ContinuityHandoffId>(
                arguments, "handoff_id");
            auto predecessor = parseStrongUuid<Domain::SessionId>(
                arguments, "predecessor_session_id");
            if (!operationId) {
                return propagate<Domain::ContinuityHandoff>(
                    std::move(operationId));
            }
            if (!handoffId) {
                return propagate<Domain::ContinuityHandoff>(std::move(handoffId));
            }
            if (!predecessor) {
                return propagate<Domain::ContinuityHandoff>(
                    std::move(predecessor));
            }
            if (!operationId.value()) {
                auto generated = generatedStrongUuid<Domain::ContinuityOperationId>();
                if (!generated) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(generated));
                }
                operationId.value() = std::move(generated).value();
            }
            if (!handoffId.value()) {
                auto generated = generatedStrongUuid<Domain::ContinuityHandoffId>();
                if (!generated) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(generated));
                }
                handoffId.value() = std::move(generated).value();
            }
            std::optional<Domain::ProviderSessionId> providerSession;
            if (const auto encoded = strictString(arguments, "provider_session_id")) {
                auto parsed = parseOpaque<Domain::ProviderSessionId>(
                    *encoded, "provider_session_id");
                if (!parsed) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(parsed));
                }
                providerSession = std::move(parsed).value();
            }
            const auto repositoryText = strictString(
                arguments, "repository_root").value_or(
                    descriptor.value().aliases.empty()
                        ? std::string{}
                        : descriptor.value().aliases.back().value());
            auto repositoryRoot = authorizePath(
                dependencies_.workspaceAuthority,
                authority,
                repositoryText,
                Domain::FileAccess::Read,
                false,
                context);
            if (!repositoryRoot) {
                return propagate<Domain::ContinuityHandoff>(
                    std::move(repositoryRoot));
            }
            auto constraints = strictStrings(arguments, "constraints");
            auto dirty = strictStrings(arguments, "dirty_summary");
            auto activeFiles = strictStrings(arguments, "active_files");
            auto openWork = strictStrings(arguments, "open_work");
            auto decisions = strictStrings(arguments, "decisions");
            auto passedGates = strictStrings(arguments, "passed_gates");
            auto openGates = strictStrings(arguments, "open_gates");
            auto memoryIds = strictStrings(arguments, "memory_record_ids");
            auto evidenceIds = strictStrings(arguments, "evidence_ids");
            auto nextActions = strictStrings(arguments, "next_actions");
            if (!constraints || !dirty || !activeFiles || !openWork ||
                !decisions || !passedGates || !openGates || !memoryIds ||
                !evidenceIds || !nextActions) {
                return failure<Domain::ContinuityHandoff>(
                    Domain::ErrorCodes::InvalidRequest,
                    "A continuity list argument is invalid.");
            }
            std::vector<Domain::PathText> typedActiveFiles;
            typedActiveFiles.reserve(activeFiles.value().size());
            for (const auto& path : activeFiles.value()) {
                auto parsed = pathText(path, "active_files");
                if (!parsed) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(parsed));
                }
                typedActiveFiles.push_back(std::move(parsed).value());
            }
            std::vector<Domain::ContinuityWorkEntry> typedOpenWork;
            typedOpenWork.reserve(openWork.value().size());
            for (auto& summary : openWork.value()) {
                typedOpenWork.push_back(Domain::ContinuityWorkEntry{
                    std::nullopt,
                    std::move(summary),
                    std::optional<std::string>{"open"}});
            }
            std::vector<Domain::ContinuityDecision> typedDecisions;
            typedDecisions.reserve(decisions.value().size());
            for (auto& decision : decisions.value()) {
                typedDecisions.push_back(Domain::ContinuityDecision{
                    std::move(decision), std::nullopt});
            }
            std::vector<Domain::MemoryRecordId> typedMemoryIds;
            typedMemoryIds.reserve(memoryIds.value().size());
            for (const auto& encoded : memoryIds.value()) {
                auto parsed = Domain::MemoryRecordId::parse(encoded);
                if (!parsed) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(parsed));
                }
                typedMemoryIds.push_back(std::move(parsed).value());
            }
            std::vector<Domain::ContinuityEvidenceReference> typedEvidence;
            typedEvidence.reserve(evidenceIds.value().size());
            for (const auto& encoded : evidenceIds.value()) {
                auto parsed = parseOpaque<Domain::EvidenceId>(
                    encoded, "evidence_ids");
                if (!parsed) {
                    return propagate<Domain::ContinuityHandoff>(
                        std::move(parsed));
                }
                typedEvidence.push_back(Domain::ContinuityEvidenceReference{
                    std::move(parsed).value(), std::nullopt});
            }
            std::vector<Domain::ContinuityNextAction> typedActions;
            auto actionTexts = std::move(nextActions).value();
            if (actionTexts.empty()) {
                actionTexts.emplace_back("Continue current work");
            }
            typedActions.reserve(actionTexts.size());
            for (std::size_t index{}; index < actionTexts.size(); ++index) {
                typedActions.emplace_back(
                    static_cast<std::uint32_t>(index + 1U),
                    std::move(actionTexts[index]),
                    std::string{},
                    "Action completed and checkpointed");
            }
            auto adapter = parseOpaque<Domain::AdapterId>(
                strictString(arguments, "adapter_id").value_or("external-mcp"),
                "adapter_id");
            if (!adapter) {
                return propagate<Domain::ContinuityHandoff>(
                    std::move(adapter));
            }
            auto digest = Domain::Sha256Digest::parse(
                "0000000000000000000000000000000000000000000000000000000000000000");
            if (!digest) {
                return propagate<Domain::ContinuityHandoff>(std::move(digest));
            }
            const auto phase = strictString(arguments, "phase_id").value_or("unknown");
            const auto mission = strictString(arguments, "mission").value_or("");
            Domain::ContinuityHandoff handoff{
                std::move(*handoffId.value()),
                std::move(*operationId.value()),
                dependencies_.clock.utcNow(),
                Domain::ContinuityProject{
                    projectId,
                    descriptor.value().displayName,
                    repositoryRoot.value().canonicalPath(),
                    strictString(arguments, "branch").value_or("unknown"),
                    strictString(arguments, "commit").value_or("0000000"),
                    std::move(dirty).value()},
                Domain::ContinuitySession{
                    std::move(predecessor).value(),
                    std::move(providerSession),
                    strictString(arguments, "model"),
                    std::optional<std::string>{"external-mcp"}},
                std::nullopt,
                mission,
                std::move(constraints).value(),
                Domain::ContinuityCurrentWork{
                    phase,
                    strictString(arguments, "work_item_id").value_or(phase),
                    strictString(arguments, "summary").value_or(mission),
                    std::move(typedActiveFiles)},
                {},
                std::move(typedOpenWork),
                std::move(typedDecisions),
                Domain::ContinuityValidation{
                    std::move(passedGates).value(),
                    std::move(openGates).value(),
                    {}},
                std::move(typedMemoryIds),
                std::move(typedEvidence),
                std::move(typedActions),
                Domain::ContinuityHostState{
                    std::move(adapter).value(),
                    Domain::ContinuityState::CheckpointPreparing,
                    strictString(arguments, "context_budget_source").value_or(
                        "caller_reported"),
                    {},
                    std::nullopt,
                    strictNumber(arguments, "remaining_budget_estimate")},
                std::move(digest).value(),
                true};
            auto encoded = dependencies_.continuityCodec.encode(handoff, context);
            if (!encoded) {
                return propagate<Domain::ContinuityHandoff>(std::move(encoded));
            }
            return Domain::Result<Domain::ContinuityHandoff>::success(
                std::move(encoded).value().handoff);
        } catch (...) {
            return failure<Domain::ContinuityHandoff>(
                Domain::ErrorCodes::InternalFailure,
                "The continuity lifecycle handoff could not be assembled.");
        }
    }

    [[nodiscard]] Domain::Result<Json> continuity(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context)
    {
        auto project = requiredProject(call, arguments);
        if (!project) {
            return propagate<Json>(std::move(project));
        }
        if (name == "continuity.checkpoint" ||
            name == "continuity.prepare_handoff" ||
            name == "continuity.request_rollover") {
            auto idempotency = optionalIdempotencyKey(
                arguments, "idempotency_key");
            if (!idempotency) {
                return propagate<Json>(std::move(idempotency));
            }
            auto handoff = continuityHandoff(
                authority, project.value(), arguments, context);
            if (!handoff) {
                return propagate<Json>(std::move(handoff));
            }
            Domain::CheckpointRequest checkpointRequest{
                handoff.value(), std::move(idempotency).value()};
            auto prepared = name == "continuity.checkpoint"
                ? dependencies_.continuity.checkpoint(
                      checkpointRequest, context)
                : dependencies_.continuity.prepareHandoff(
                      checkpointRequest, context);
            if (!prepared) {
                return propagate<Json>(std::move(prepared));
            }
            if (name != "continuity.request_rollover") {
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"disposition", "memory_only_handoff_ready"},
                    {"operation", continuityOperationJson(
                         prepared.value().operation)},
                    {"handoff", continuityHandoffJson(
                         prepared.value().handoff)},
                    {"host_capability",
                     "external_session_creation_unconfirmed"}});
            }
            auto rollover = dependencies_.continuity.requestRollover(
                Domain::RolloverRequest{
                    project.value(), prepared.value().operation.operationId},
                context);
            if (!rollover) {
                return propagate<Json>(std::move(rollover));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"operation", continuityOperationJson(rollover.value().operation)},
                {"handoff", continuityHandoffJson(prepared.value().handoff)},
                {"successor",
                 rollover.value().successor
                     ? hostSessionJson(*rollover.value().successor)
                     : Json(nullptr)},
                {"successor_created", rollover.value().successor.has_value()},
                {"acknowledged", rollover.value().acknowledged},
                {"predecessor_sealed", rollover.value().predecessorSealed}});
        }
        if (name == "continuity.get_pending_handoff") {
            auto pending = dependencies_.continuity.getPendingHandoff(
                project.value(), context);
            if (!pending) {
                return propagate<Json>(std::move(pending));
            }
            std::optional<Domain::ContinuityOperation> activeOperation;
            if (pending.value()) {
                auto status = dependencies_.continuity.status(
                    project.value(), context);
                if (!status) {
                    return propagate<Json>(std::move(status));
                }
                activeOperation = std::move(status).value().activeOperation;
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", project.value().value()},
                {"found", pending.value().has_value()},
                {"operation",
                 activeOperation
                     ? continuityOperationJson(*activeOperation)
                     : Json(nullptr)},
                {"handoff",
                 pending.value()
                     ? continuityHandoffJson(*pending.value())
                     : Json(nullptr)}});
        }
        if (name == "continuity.acknowledge_handoff") {
            auto operation = parseStrongUuid<Domain::ContinuityOperationId>(
                arguments, "operation_id");
            auto handoffId = parseStrongUuid<Domain::ContinuityHandoffId>(
                arguments, "handoff_id");
            auto successor = parseStrongUuid<Domain::SessionId>(
                arguments, "successor_session_id");
            if (!operation) {
                return propagate<Json>(std::move(operation));
            }
            if (!handoffId) {
                return propagate<Json>(std::move(handoffId));
            }
            if (!successor) {
                return propagate<Json>(std::move(successor));
            }
            auto pending = dependencies_.continuity.getPendingHandoff(
                project.value(), context);
            if (!pending) {
                return propagate<Json>(std::move(pending));
            }
            if (!pending.value() ||
                pending.value()->handoffId != handoffId.value() ||
                pending.value()->operationId != operation.value()) {
                return failure<Json>(
                    Domain::ErrorCodes::Conflict,
                    "The acknowledgment does not match the pending handoff.");
            }
            Domain::AdapterId adapter = pending.value()->hostState.adapterId;
            if (const auto supplied = strictString(arguments, "adapter_id")) {
                auto parsed = parseOpaque<Domain::AdapterId>(
                    *supplied, "adapter_id");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                adapter = std::move(parsed).value();
            }
            auto acknowledged = dependencies_.continuity.acknowledgeHandoff(
                project.value(),
                operation.value(),
                Domain::HandoffAcknowledgement{
                    handoffId.value(),
                    successor.value(),
                    std::move(adapter),
                    pending.value()->contentSha256},
                context);
            if (!acknowledged) {
                return propagate<Json>(std::move(acknowledged));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"operation", continuityOperationJson(acknowledged.value())},
                {"acknowledged", true}});
        }
        if (name == "continuity.resume") {
            auto requestedOperation =
                parseStrongUuid<Domain::ContinuityOperationId>(
                    arguments, "operation_id");
            if (!requestedOperation) {
                return propagate<Json>(std::move(requestedOperation));
            }
            auto status = dependencies_.continuity.status(project.value(), context);
            if (!status) {
                return propagate<Json>(std::move(status));
            }
            if (!status.value().activeOperation ||
                status.value().activeOperation->operationId !=
                    requestedOperation.value()) {
                return failure<Json>(
                    Domain::ErrorCodes::Conflict,
                    "The requested continuity operation is not active.");
            }
            const auto& operation = *status.value().activeOperation;
            if (!operation.successorSessionId) {
                return failure<Json>(
                    Domain::ErrorCodes::Conflict,
                    "The active continuity operation has no successor session.");
            }
            auto resumed = dependencies_.continuity.resume(
                Domain::HandoffResumeRequest{
                    project.value(),
                    operation.handoffId,
                    *operation.successorSessionId},
                context);
            if (!resumed) {
                return propagate<Json>(std::move(resumed));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"operation", continuityOperationJson(resumed.value().operation)},
                {"handoff", continuityHandoffJson(resumed.value().handoff)},
                {"session", hostSessionJson(resumed.value().session)},
                {"resumed", true}});
        }
        if (name == "continuity.status") {
            auto status = dependencies_.continuity.status(project.value(), context);
            if (!status) {
                return propagate<Json>(std::move(status));
            }
            const auto state = status.value().activeOperation
                ? Domain::wireName(status.value().activeOperation->state)
                : std::string_view{"active"};
            const auto pendingHandoff = status.value().activeOperation
                ? Json(status.value().activeOperation->handoffId.value())
                : Json(nullptr);
            const auto activeSession = status.value().activeOperation &&
                    status.value().activeOperation->acknowledgedSessionId
                ? Json(status.value().activeOperation->acknowledgedSessionId->value())
                : Json(nullptr);
            Json retry{
                {"error",
                 status.value().activeOperation &&
                         status.value().activeOperation->lastError
                     ? Json(*status.value().activeOperation->lastError)
                     : Json(nullptr)},
                {"retry_at",
                 status.value().activeOperation &&
                         status.value().activeOperation->retryAt
                     ? Json(formatTimestamp(
                           *status.value().activeOperation->retryAt))
                     : Json(nullptr)}};
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"project_id", status.value().projectId.value()},
                {"state", state},
                {"operation",
                 status.value().activeOperation
                     ? continuityOperationJson(*status.value().activeOperation)
                     : Json(nullptr)},
                {"pending_handoff_id", std::move(pendingHandoff)},
                {"active_session_id", std::move(activeSession)},
                {"retry", std::move(retry)},
                {"health", "ok"},
                {"schema_version", 1},
                {"active_operation",
                 status.value().activeOperation
                     ? continuityOperationJson(*status.value().activeOperation)
                     : Json(nullptr)},
                {"operation_count", status.value().operationCount},
                {"handoff_count", status.value().handoffCount},
                {"recovery_required", status.value().recoveryRequired}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested continuity lifecycle tool is not supported.");
    }

    [[nodiscard]] Domain::Result<Json> legacyMemory(
        const std::string_view name,
        const Json& arguments,
        const Domain::OperationContext& context)
    {
        if (name == "memory_set") {
            const auto key = legacyString(arguments, "key");
            auto body = legacyString(arguments, "body");
            if (!body) {
                body = legacyString(arguments, "content");
            }
            if (!body) {
                body = legacyString(arguments, "value");
            }
            std::vector<std::string> tags;
            const auto* suppliedTags = member(arguments, "tags");
            if (suppliedTags != nullptr && suppliedTags->is_string()) {
                const auto& encoded = suppliedTags->get_ref<const std::string&>();
                std::size_t start{};
                while (start <= encoded.size()) {
                    const auto comma = encoded.find(',', start);
                    const auto end = comma == std::string::npos
                        ? encoded.size()
                        : comma;
                    tags.push_back(encoded.substr(start, end - start));
                    if (comma == std::string::npos) {
                        break;
                    }
                    start = comma + 1U;
                }
            } else {
                tags = legacyStrings(arguments, "tags");
            }
            auto result = dependencies_.legacyMemory.set(
                Domain::LegacyMemorySetRequest{
                    key.value_or(""), body, std::move(tags)},
                context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            auto home = dependencies_.applicationPaths.dataRoot(context);
            if (!home) {
                return propagate<Json>(std::move(home));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"stored", result.value().stored},
                {"note", memoryNoteJson(result.value().note)},
                {"home", home.value().value()}});
        }
        if (name == "memory_get") {
            auto result = dependencies_.legacyMemory.get(
                Domain::LegacyMemoryGetRequest{
                    legacyString(arguments, "key").value_or("")},
                context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            if (!result.value().note) {
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"found", false},
                    {"key", result.value().key},
                    {"note", nullptr}});
            }
            const auto& note = *result.value().note;
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"found", true},
                {"key", note.key},
                {"body", note.body},
                {"tags", stringArray(note.tags)},
                {"created_at", formatTimestamp(note.createdAt)},
                {"updated_at", formatTimestamp(note.updatedAt)},
                {"note", memoryNoteJson(note)}});
        }
        if (name == "memory_list") {
            Domain::LegacyMemoryListRequest request;
            request.prefix = legacyString(arguments, "prefix");
            request.tag = legacyString(arguments, "tag");
            request.includeSystem =
                strictBoolean(arguments, "include_system").value_or(false);
            request.includeBody =
                strictBoolean(arguments, "include_body").value_or(false);
            request.requestedLimit = legacyInteger(arguments, "limit").value_or(
                static_cast<std::int64_t>(
                    Domain::LegacyMemoryLimits::DefaultQueryLimit));
            auto result = dependencies_.legacyMemory.list(request, context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            Json notes = Json::array();
            for (const auto& note : result.value().notes) {
                notes.push_back(legacyMemoryNoteJson(note));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"count", notes.size()},
                {"total", result.value().visibleTotal},
                {"prefix",
                 request.prefix ? Json(*request.prefix) : Json(nullptr)},
                {"tag", request.tag ? Json(*request.tag) : Json(nullptr)},
                {"include_system", request.includeSystem},
                {"notes", std::move(notes)}});
        }
        if (name == "memory_delete") {
            auto result = dependencies_.legacyMemory.remove(
                Domain::LegacyMemoryRemoveRequest{
                    legacyString(arguments, "key").value_or("")},
                context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"key", result.value().key},
                {"deleted", result.value().deleted},
                {"existed", result.value().existed},
                {"system_key", result.value().systemKey}});
        }
        if (name == "memory_search") {
            auto query = legacyString(arguments, "query");
            if (!query) {
                query = legacyString(arguments, "q");
            }
            if (!query) {
                query = legacyString(arguments, "pattern");
            }
            Domain::LegacyMemorySearchRequest request;
            request.query = query;
            request.includeSystem =
                strictBoolean(arguments, "include_system").value_or(false);
            request.includeBody =
                strictBoolean(arguments, "include_body").value_or(true);
            request.requestedLimit = legacyInteger(arguments, "limit").value_or(
                static_cast<std::int64_t>(
                    Domain::LegacyMemoryLimits::DefaultQueryLimit));
            auto result = dependencies_.legacyMemory.search(request, context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            Json notes = Json::array();
            for (const auto& note : result.value().notes) {
                notes.push_back(legacyMemoryNoteJson(note));
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"query", result.value().query},
                {"count", notes.size()},
                {"include_system", request.includeSystem},
                {"notes", std::move(notes)}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested memory tool is not supported.");
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPatch>
    legacyPatch(const Json& arguments)
    {
        try {
            Domain::LegacyContinuityPatch patch;
            patch.goal = legacyString(arguments, "goal");
            patch.status = legacyString(arguments, "status");
            patch.projectSlug = legacyString(arguments, "project_slug");
            if (!patch.projectSlug) {
                patch.projectSlug = legacyString(arguments, "project");
            }
            patch.workingDirectory = legacyString(arguments, "cwd");
            patch.chatLabel = legacyString(arguments, "chat_label");
            if (!patch.chatLabel) {
                patch.chatLabel = legacyString(arguments, "chat");
            }
            patch.narrative = legacyString(arguments, "narrative");
            if (!patch.narrative) {
                patch.narrative = legacyString(arguments, "summary");
            }
            patch.resumeSeed = legacyString(arguments, "resume_seed");
            const auto decodeList = [&](const std::string_view key,
                                        const char separator)
                -> std::optional<std::vector<std::string>> {
                const auto* value = member(arguments, key);
                if (value == nullptr) {
                    return std::nullopt;
                }
                if (value->is_array()) {
                    return legacyStrings(arguments, key);
                }
                if (!value->is_string()) {
                    return std::vector<std::string>{};
                }
                std::vector<std::string> result;
                const auto& encoded = value->get_ref<const std::string&>();
                std::size_t start{};
                while (start <= encoded.size()) {
                    const auto delimiter = encoded.find(separator, start);
                    const auto end = delimiter == std::string::npos
                        ? encoded.size()
                        : delimiter;
                    auto item = encoded.substr(start, end - start);
                    const auto first = item.find_first_not_of(" \t\r\n");
                    const auto last = item.find_last_not_of(" \t\r\n");
                    if (first != std::string::npos) {
                        result.push_back(item.substr(first, last - first + 1U));
                    }
                    if (delimiter == std::string::npos) {
                        break;
                    }
                    start = delimiter + 1U;
                }
                return result;
            };
            patch.blockers = decodeList("blockers", '\n');
            patch.nextActions = decodeList("next_actions", '\n');
            patch.keyFiles = decodeList("key_files", ',');
            patch.decisions = decodeList("decisions", '\n');
            auto valid = Domain::validateLegacyContinuityPatch(patch);
            if (!valid) {
                return propagate<Domain::LegacyContinuityPatch>(
                    std::move(valid));
            }
            return Domain::Result<Domain::LegacyContinuityPatch>::success(
                std::move(patch));
        } catch (...) {
            return failure<Domain::LegacyContinuityPatch>(
                Domain::ErrorCodes::InternalFailure,
                "The legacy continuity patch could not be decoded.");
        }
    }

    [[nodiscard]] Domain::Result<Json> legacyContinuity(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        std::optional<Domain::ContextRecoveryReceipt>& contextRecovery)
    {
        if (name == "session_checkpoint" || name == "session_handoff") {
            auto patch = legacyPatch(arguments);
            if (!patch) {
                return propagate<Json>(std::move(patch));
            }
            std::optional<Domain::LegacyHandoffId> handoffId;
            auto encodedId = legacyString(arguments, "handoff_id");
            if (!encodedId) {
                encodedId = legacyString(arguments, "id");
            }
            if (encodedId && !encodedId->empty()) {
                auto parsed = parseOpaque<Domain::LegacyHandoffId>(
                    *encodedId, "handoff_id");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                handoffId.emplace(std::move(parsed).value());
            }
            Domain::LegacyContinuityWriteRequest request{
                std::move(handoffId), std::move(patch).value()};
            auto result = name == "session_checkpoint"
                ? dependencies_.legacyContinuity.checkpoint(
                      request,
                      call.clientId(),
                      Domain::LegacyHandoffSource::Model,
                      context)
                : dependencies_.legacyContinuity.handoff(
                      request,
                      call.clientId(),
                      Domain::LegacyHandoffSource::Model,
                      context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            return Domain::Result<Json>::success(legacyPersistJson(
                result.value(),
                name == "session_checkpoint" ? "checkpoint" : "handoff"));
        }
        if (name == "context_get") {
            auto encodedId = legacyString(arguments, "handoff_id");
            if (!encodedId) {
                encodedId = legacyString(arguments, "id");
            }
            std::optional<Domain::LegacyHandoffId> handoffId;
            if (encodedId && !encodedId->empty()) {
                auto parsed = parseOpaque<Domain::LegacyHandoffId>(
                    *encodedId, "handoff_id");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                handoffId.emplace(std::move(parsed).value());
            }
            auto result = dependencies_.legacyContinuity.get(
                Domain::LegacyContinuityGetRequest{
                    std::move(handoffId),
                    strictBoolean(arguments, "resume_ready").value_or(false)},
                context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            if (!result.value().record) {
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"found", false},
                    {"message",
                     result.value().explicitIdRequested
                         ? "No handoff packet found for the requested id."
                         : "No handoff packet yet. Call session_checkpoint or session_handoff during work."},
                    {"bootstrap",
                     Json::array({"forge_status", "session_checkpoint when you have a goal"})}});
            }
            if (authority.callerId() != call.clientId()) {
                return failure<Json>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The recovered context caller does not match workspace authority.");
            }
            const auto automation =
                dependencies_.continuityAutomationStatus.snapshot(
                    call.clientId());
            if (automation.blocked &&
                (!automation.handoffId ||
                 *automation.handoffId !=
                     result.value().record->packet.id.value())) {
                return failure<Json>(
                    Domain::ErrorCodes::Conflict,
                    "The recovered handoff does not match the client's active context block.",
                    true);
            }
            auto adoption = dependencies_.clientWorkspaceContext.adopt(
                call.clientId(), *result.value().record, context);
            if (!adoption) {
                return propagate<Json>(std::move(adoption));
            }
            if (adoption.value().superseded) {
                if (adoption.value().warning) {
                    return Domain::Result<Json>::failure(
                        *adoption.value().warning);
                }
                return failure<Json>(
                    Domain::ErrorCodes::Conflict,
                    "A newer recovered workspace superseded this context.",
                    true);
            }
            std::optional<Domain::PathText> recoveredWorkingDirectory;
            if (result.value().record->packet.workingDirectory) {
                auto parsed = pathText(
                    *result.value().record->packet.workingDirectory,
                    "recovered cwd");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                recoveredWorkingDirectory.emplace(
                    std::move(parsed).value());
            }
            std::vector<Domain::PathText> recoveredKeyFiles;
            recoveredKeyFiles.reserve(
                result.value().record->packet.keyFiles.size());
            for (const auto& encoded :
                 result.value().record->packet.keyFiles) {
                auto parsed = pathText(encoded, "recovered key_files");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                recoveredKeyFiles.push_back(std::move(parsed).value());
            }
            contextRecovery.emplace(Domain::ContextRecoveryReceipt{
                call.clientId(),
                result.value().record->packet.id,
                std::move(recoveredWorkingDirectory),
                std::move(recoveredKeyFiles)});
            return Domain::Result<Json>::success(legacyGetJson(
                *result.value().record,
                adoption.value()));
        }
        if (name == "context_list") {
            auto result = dependencies_.legacyContinuity.list(
                Domain::LegacyContinuityListRequest{
                    legacyInteger(arguments, "limit").value_or(
                        static_cast<std::int64_t>(
                            Domain::LegacyContinuityLimits::DefaultListLimit))},
                context);
            if (!result) {
                return propagate<Json>(std::move(result));
            }
            Json handoffs = Json::array();
            for (const auto& item : result.value().handoffs) {
                handoffs.push_back(Json{
                    {"id", item.id.value()},
                    {"updated_at", formatTimestamp(item.updatedAt)},
                    {"source", Domain::wireName(item.source)},
                    {"resume_ready", item.resumeReady},
                    {"goal", item.goal},
                    {"status", item.status},
                    {"agent_count", item.agentCount}});
            }
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"count", handoffs.size()},
                {"handoffs", std::move(handoffs)}});
        }
        return failure<Json>(
            Domain::ErrorCodes::InvalidRequest,
            "The requested continuity compatibility tool is not supported.");
    }

    McpToolPackDependencies dependencies_;
};

McpToolPackAdapter::McpToolPackAdapter(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

McpToolPackAdapter::~McpToolPackAdapter() noexcept = default;

Domain::Result<std::unique_ptr<McpToolPackAdapter>>
McpToolPackAdapter::create(McpToolPackDependencies dependencies) noexcept
{
    try {
        auto descriptors = McpToolCatalog::validateDescriptors(
            dependencies.catalog.tools(), McpToolCatalog::ExpectedToolCount);
        if (!descriptors) {
            return propagate<std::unique_ptr<McpToolPackAdapter>>(
                std::move(descriptors));
        }
        if (dependencies.productVersion.empty() ||
            dependencies.runtimeName.empty()) {
            return failure<std::unique_ptr<McpToolPackAdapter>>(
                Domain::ErrorCodes::InvalidRequest,
                "The MCP tool-pack dependencies are incomplete.");
        }
        return Domain::Result<std::unique_ptr<McpToolPackAdapter>>::success(
            std::unique_ptr<McpToolPackAdapter>{new McpToolPackAdapter{
                std::make_unique<Impl>(std::move(dependencies))}});
    } catch (...) {
        return failure<std::unique_ptr<McpToolPackAdapter>>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP tool-pack adapter could not be allocated.");
    }
}

std::span<const Domain::McpToolDescriptor>
McpToolPackAdapter::tools() const noexcept
{
    return implementation_ ? implementation_->tools()
                           : std::span<const Domain::McpToolDescriptor>{};
}

Domain::Result<Domain::ToolCallOutcome> McpToolPackAdapter::handle(
    const Contracts::AuthorizedToolCall& authorizedCall,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ToolCallOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP tool-pack adapter is unavailable.");
    }
    return implementation_->handle(authorizedCall, authority, context);
}

} // namespace ForgeConductor::Mcp
