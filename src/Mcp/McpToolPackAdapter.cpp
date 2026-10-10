#include "ForgeConductor/Mcp/McpToolPackAdapter.h"

#include "McpAgentWorkerTools.h"

#include "ForgeConductor/Application/LegacyInstructionPackageMigration.h"
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
#include <thread>
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
constexpr std::size_t MaximumBoundedReadResponseBytes = 32U * 1024U;
constexpr std::size_t MaximumStatusInstructionPackages = 100U;
constexpr std::size_t MaximumBootstrapInstructionPackages = 16U;
constexpr std::size_t MaximumComfyJobPayloadBytes = 128U * 1024U;
constexpr std::size_t MaximumComfyPosterBase64Bytes = 32U * 1024U;
constexpr std::int64_t DefaultReadWindowLines = 200;

[[nodiscard]] bool hasLeadingFillerMarker(const std::string_view text)
{
    std::size_t start{};
    for (std::size_t end = 0U; end <= text.size(); ++end) {
        const bool boundary = end == text.size() || text[end] == '\n' || text[end] == '\r' ||
            ((text[end] == '.' || text[end] == '!' || text[end] == '?') &&
                (end + 1U == text.size() || std::isspace(static_cast<unsigned char>(text[end + 1U])) != 0));
        if (!boundary) {
            continue;
        }
        auto sentence = text.substr(start, end - start);
        const auto first = sentence.find_first_not_of(" \t\r\n\f\v");
        if (first != std::string_view::npos) {
            sentence.remove_prefix(first);
            for (const auto marker : {std::string_view{"lorem ipsum"}, std::string_view{"placeholder"}}) {
                if (sentence.starts_with(marker) && (sentence.size() == marker.size() ||
                    std::isalnum(static_cast<unsigned char>(sentence[marker.size()])) == 0)) {
                    return true;
                }
            }
        }
        start = end + 1U;
    }
    return false;
}

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

[[nodiscard]] Domain::Result<Json> promoteComfyJobPreview(Json payload)
{
    if (!payload.is_object()) return failure<Json>(Domain::ErrorCodes::MalformedMessage,
        "The ComfyUI job result must be an object.");
    if (payload.dump().size() > MaximumComfyJobPayloadBytes)
        return failure<Json>(Domain::ErrorCodes::PayloadTooLarge,
            "The ComfyUI job result exceeds its bounded delivery size.");
    if (!strictBoolean(payload, "ok").value_or(false) ||
        (payload.contains("publication_suppressed") && !strictBoolean(payload, "publication_suppressed")) ||
        strictBoolean(payload, "publication_suppressed").value_or(false) ||
        payload.contains("image_base64") ||
        (payload.contains("error") && !payload.at("error").is_null()))
        return Domain::Result<Json>::success(std::move(payload));
    for (const auto group : {"artifacts", "preview_artifacts"}) {
        const auto* artifacts = member(payload, group);
        if (!artifacts || !artifacts->is_array()) continue;
        for (std::size_t index{}; index < artifacts->size(); ++index) {
            const auto& artifact = artifacts->at(index);
            if (!artifact.is_object()) continue;
            const auto* metadata = member(artifact, "metadata");
            const auto* preview = member(artifact, "preview");
            const auto artifactSeal = strictString(artifact, "sha256");
            if (!metadata || !metadata->is_object() ||
                !strictBoolean(*metadata, "decoded").value_or(false) ||
                !artifactSeal || !Domain::Sha256Digest::parse(*artifactSeal) ||
                !preview || !preview->is_object()) continue;
            const auto mime = strictString(*preview, "mime_type");
            const auto encoded = strictString(*preview, "base64");
            const auto seal = strictString(*preview, "sha256");
            const auto width = strictInteger(*preview, "width"), height = strictInteger(*preview, "height");
            if (!mime || *mime != "image/png" || !seal || !Domain::Sha256Digest::parse(*seal) ||
                !width || !height || *width < 1 || *height < 1 || *width > 2048 || *height > 2048 ||
                !encoded || encoded->empty() || encoded->size() > MaximumComfyPosterBase64Bytes ||
                encoded->size() % 4U != 0U || !encoded->starts_with("iVBORw0KGgo") ||
                encoded->find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != std::string::npos) continue;
            const auto padding = encoded->ends_with("==") ? 2U : encoded->ends_with("=") ? 1U : 0U;
            const auto firstPadding = encoded->find('=');
            if (firstPadding != std::string::npos && firstPadding != encoded->size() - padding) continue;
            auto promoted = payload;
            promoted["image_base64"] = *encoded; promoted["image_mime_type"] = *mime;
            promoted["preview_width"] = *width; promoted["preview_height"] = *height;
            promoted["preview_png_sha256"] = *seal;
            for (const auto repeatedGroup : {"artifacts", "preview_artifacts"}) {
                auto repeated = promoted.find(repeatedGroup);
                if (repeated == promoted.end() || !repeated->is_array()) continue;
                for (auto& item : *repeated) {
                    if (!item.is_object() || strictString(item, "sha256") != artifactSeal) continue;
                    auto thumbnail = item.find("preview");
                    if (thumbnail == item.end() || !thumbnail->is_object() ||
                        strictString(*thumbnail, "sha256") != seal || strictString(*thumbnail, "base64") != encoded) continue;
                    thumbnail->erase("base64"); (*thumbnail)["delivered_as_image_content"] = true;
                }
            }
            // Keep the provider's public job bound even when promotion adds
            // metadata. A full receipt stays available if the poster cannot fit.
            if (promoted.dump().size() <= MaximumComfyJobPayloadBytes)
                return Domain::Result<Json>::success(std::move(promoted));
        }
    }
    return Domain::Result<Json>::success(std::move(payload));
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
        normalizeLegacyStringFields(arguments, {"artifact", "merge_policy", "expected_checksum"});
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
    void seedWorkspace(const Contracts::WorkspaceAuthority& authority,
        const std::optional<Domain::PathText>& defaultPath = std::nullopt)
    {
        if (defaultPath) {
            observation_.baseDirectory = *defaultPath;
            pinnedBase_ = true;
        } else if (!authority.trustedRoots().empty()) {
            observation_.baseDirectory = authority.trustedRoots().front();
        }
        defaultDirectory_ = observation_.baseDirectory ? observation_.baseDirectory->value() : std::string{};
    }

    [[nodiscard]] std::string defaultDirectory() const
    { return defaultDirectory_; }

    void observe(
        const Contracts::AuthorizedPath& authorized,
        const ContinuityPathRole role)
    {
        if (!pinnedBase_ && !observation_.path && !observation_.workingDirectory) {
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
    bool pinnedBase_{};
    std::string defaultDirectory_;
};

[[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorizePath(
    Contracts::IWorkspaceAuthority& resolver,
    const Contracts::WorkspaceAuthority& authority,
    const std::string_view encoded,
    const Domain::FileAccess access,
    const bool protectAuthorityRoot,
    const Domain::OperationContext& context,
    ToolContinuityObservationBuilder* const observation = nullptr,
    const ContinuityPathRole role = ContinuityPathRole::Path,
    std::optional<Domain::PathText> excludedSubtree = std::nullopt)
{
    std::optional<Domain::PathText> base;
    std::string anchored{encoded};
    if (!isAbsoluteToolPath(encoded) && !authority.trustedRoots().empty()) {
        if (resolver.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host) {
            auto root = resolver.defaultWorkspacePath(authority, context);
            if (!root) return propagate<Contracts::AuthorizedPath>(std::move(root));
            anchored = anchoredToolPath(encoded, root.value().value());
        } else {
            base = authority.trustedRoots().front();
            anchored = anchoredToolPath(encoded, base->value());
        }
    }
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
            protectAuthorityRoot,
            std::move(excludedSubtree)},
        context);
    if (authorized && observation != nullptr) {
        observation->observe(authorized.value(), role);
    }
    return authorized;
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

[[nodiscard]] Json shellJobJson(const Domain::ShellJobSnapshot& job, const bool includeOutput = true)
{
    const auto state = [&] {
        switch (job.state) {
        case Domain::ShellJobState::Running: return "running";
        case Domain::ShellJobState::Completed: return "completed";
        case Domain::ShellJobState::Failed: return "failed";
        case Domain::ShellJobState::Cancelled: return "cancelled";
        case Domain::ShellJobState::TimedOut: return "timed_out";
        }
        return "failed";
    }();
    Json value{{"ok", true}, {"job_id", job.jobId}, {"state", state},
        {"command", job.command}, {"cwd", job.cwd}, {"timeout_sec", job.timeoutSeconds},
        {"elapsed_ms", job.elapsed.count()}, {"done", job.state != Domain::ShellJobState::Running},
        {"poll_after_sec", job.state == Domain::ShellJobState::Running ? 5 : 0}};
    value["pid"] = job.processId ? Json(job.processId) : Json(nullptr);
    value["process_creation_time"] = job.processCreationTime ? Json(job.processCreationTime) : Json(nullptr);
    value["launch_pending"] = job.state == Domain::ShellJobState::Running && job.processId == 0U;
    value["alive"] = job.processAlive ? Json(*job.processAlive) : Json(nullptr);
    value["exit_code"] = job.result ? Json(job.result->exitCode) : Json(nullptr);
    value["stdout_path"] = job.stdoutPath;
    value["stderr_path"] = job.stderrPath;
    value["receipt_path"] = job.receiptPath;
    value["log_sha256"] = job.logHash.empty() ? Json(nullptr) : Json(job.logHash);
    value["log_truncated"] = job.logTruncated;
    if (includeOutput) {
        value["args"] = job.arguments;
    } else {
        constexpr std::size_t MaximumSummaryCommandBytes = 256U;
        auto end = (std::min)(job.command.size(), MaximumSummaryCommandBytes);
        while (end < job.command.size() &&
               (static_cast<unsigned char>(job.command[end]) & 0xc0U) == 0x80U) {
            --end;
        }
        value["command"] = job.command.substr(0U, end);
        value["command_truncated"] = end < job.command.size();
    }
    value["memory_attached"] = job.memoryAttached;
    value["memory_attach_error"] = job.memoryAttachError
        ? Json{{"code", job.memoryAttachError->code}, {"message", job.memoryAttachError->message}} : Json(nullptr);
    value["result"] = job.result ? processJson(*job.result, job.cwd) : Json(nullptr);
    if (job.result && includeOutput) {
        const auto projectOutput = [&](const char* field, const char* truncatedField,
            const std::size_t maximumBytes) {
            auto& output = value["result"][field].get_ref<std::string&>();
            const auto capturedBytes = output.size();
            auto end = (std::min)(capturedBytes, maximumBytes);
            while (end < capturedBytes &&
                (static_cast<unsigned char>(output[end]) & 0xc0U) == 0x80U) --end;
            const bool responseTruncated = end < capturedBytes;
            const bool captureTruncated = value["result"][truncatedField].get<bool>();
            value["result"][std::string{field} + "_captured_bytes"] = capturedBytes;
            value["result"][std::string{field} + "_response_truncated"] = responseTruncated;
            value["result"][std::string{field} + "_capture_truncated"] = captureTruncated;
            value["result"][truncatedField] = captureTruncated || responseTruncated;
            output.resize(end);
        };
        // Durable logs retain their independent byte limit. Keep status previews
        // below the complete MCP envelope limit even with JSON control escaping.
        projectOutput("stdout", "stdout_truncated", 16U * 1024U);
        projectOutput("stderr", "stderr_truncated", 4U * 1024U);
    }
    if (job.result && !includeOutput) {
        value["result"].erase("stdout");
        value["result"].erase("stderr");
    }
    value["error"] = job.error ? Json{{"code", job.error->code},
        {"message", job.error->message}, {"retryable", job.error->retryable}} : Json(nullptr);
    if (job.cmakeTest) {
        const auto& test = *job.cmakeTest;
        const auto phase = [&](const std::optional<Domain::ProcessResult>& result) {
            if (!result) return Json(nullptr);
            auto value = processJson(*result, test.buildDirectory);
            value.erase("stdout");
            value.erase("stderr");
            return value;
        };
        value["cmake_test"] = Json{
            {"build_dir", test.buildDirectory}, {"mode", test.buildRequested ? "build_and_test" : "test"},
            {"target", test.target ? Json(*test.target) : Json(nullptr)},
            {"filter", test.filter ? Json(*test.filter) : Json(nullptr)},
            {"config", test.configuration ? Json(*test.configuration) : Json(nullptr)},
            {"build_result", phase(test.buildResult)}, {"test_result", phase(test.testResult)},
            {"counts", test.counts ? Json{{"tests", test.counts->tests}, {"passed", test.counts->passed},
                {"failed", test.counts->failed}, {"skipped", test.counts->skipped}, {"disabled", test.counts->disabled}}
                : Json(nullptr)},
            {"report_path", test.reportPath},
            {"report_sha256", test.reportSha256.empty() ? Json(nullptr) : Json(test.reportSha256)},
            {"report_bytes", test.reportBytes},
            {"report_unverified", test.reportUnverified},
            {"report_error", test.reportError ? Json{{"code", test.reportError->code},
                {"message", test.reportError->message}, {"retryable", test.reportError->retryable}} : Json(nullptr)},
            {"logs_tool", "process_read_log"}};
    }
    return value;
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


struct PolicyIndexWindow final {
    std::string projectId;
    std::string revision;
    std::string digest;
    std::size_t coverageOffset{};
    std::size_t guidanceOffset{};
};

[[nodiscard]] Domain::Result<PolicyIndexWindow> policyIndexWindow(
    const Json& index, const Json& arguments, const Domain::ProjectId& projectId,
    Contracts::IHasher& hasher)
{
    const auto coverage = index.value("coverage", Json::array());
    const auto guidance = index.value("agent_guidance", Json::array());
    if (!coverage.is_array() || !guidance.is_array())
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::IntegrityFailure,
            "The adopted policy index contains invalid coverage or guidance.");
    auto inspected = index;
    inspected.erase("clu_governance_notifications");
    inspected.erase("clu_governance_notifications_deferred");
    inspected.erase("clu_governance_notifications_read_tool");
    const auto identity = Json{{"project_id", projectId.value()}, {"index", std::move(inspected)}}.dump();
    auto digest = hasher.sha256(std::as_bytes(std::span{identity.data(), identity.size()}));
    if (!digest) return propagate<PolicyIndexWindow>(std::move(digest));
    PolicyIndexWindow window{projectId.value(), index.value("revision", std::string{}),
        digest.value().value()};
    if (!arguments.contains("cursor"))
        return Domain::Result<PolicyIndexWindow>::success(std::move(window));
    const auto cursorText = arguments.at("cursor").get<std::string>();
    if (cursorText.empty() || cursorText.size() > 2U * 1024U)
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::InvalidRequest,
            "The policy index cursor is empty or exceeds its byte limit.");
    auto canonical = McpJsonCodec{}.canonicalize(cursorText);
    if (!canonical) return failure<PolicyIndexWindow>(Domain::ErrorCodes::InvalidRequest,
        "The policy index cursor is malformed.");
    const auto cursor = Json::parse(canonical.value());
    const auto integer = [&](const char* field) {
        return cursor.contains(field) &&
            (cursor.at(field).is_number_unsigned() ||
             (cursor.at(field).is_number_integer() && cursor.at(field).get<std::int64_t>() >= 0));
    };
    if (!cursor.is_object() || cursor.size() != 6U || !integer("version") ||
        cursor.at("version") != 1U || !integer("coverage_offset") || !integer("guidance_offset") ||
        !cursor.contains("project_id") || !cursor.at("project_id").is_string() ||
        !cursor.contains("revision") || !cursor.at("revision").is_string() ||
        !cursor.contains("index_sha256") || !cursor.at("index_sha256").is_string())
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::InvalidRequest,
            "The policy index cursor has invalid fields or offsets.");
    if (cursor.at("project_id") != window.projectId)
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::ProjectScopeMismatch,
            "The policy index cursor belongs to another project.");
    if (cursor.at("revision") != window.revision || cursor.at("index_sha256") != window.digest)
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::Conflict,
            "The adopted policy index changed. Restart the index read without a cursor.");
    window.coverageOffset = cursor.at("coverage_offset").get<std::size_t>();
    window.guidanceOffset = cursor.at("guidance_offset").get<std::size_t>();
    if (window.coverageOffset > coverage.size() || window.guidanceOffset > guidance.size())
        return failure<PolicyIndexWindow>(Domain::ErrorCodes::InvalidRequest,
            "The policy index cursor is outside the coverage or guidance array.");
    return Domain::Result<PolicyIndexWindow>::success(std::move(window));
}

[[nodiscard]] Domain::Result<Json> boundedPolicyIndex(
    Json index, const Json& arguments, const Domain::ProjectId& projectId,
    Contracts::IHasher& hasher)
{
    auto selected = policyIndexWindow(index, arguments, projectId, hasher);
    if (!selected) return propagate<Json>(std::move(selected));
    const auto& window = selected.value();
    const auto coverage = index.value("coverage", Json::array());
    const auto guidance = index.value("agent_guidance", Json::array());
    index["coverage"] = Json::array();
    index["agent_guidance"] = Json::array();
    index["coverage_offset"] = window.coverageOffset;
    index["guidance_offset"] = window.guidanceOffset;
    index["index_sha256"] = window.digest;
    index["instruction"] = "Read every index page by passing next_cursor as cursor until complete, then read required documents by path and next_offset.";
    auto coverageEnd = window.coverageOffset;
    auto guidanceEnd = window.guidanceOffset;
    const auto updateCursor = [&] {
        const bool complete = coverageEnd == coverage.size() && guidanceEnd == guidance.size();
        index["complete"] = complete;
        index["next_cursor"] = complete ? Json(nullptr) : Json(Json{
            {"version", 1U}, {"project_id", window.projectId}, {"revision", window.revision},
            {"index_sha256", window.digest}, {"coverage_offset", coverageEnd},
            {"guidance_offset", guidanceEnd}}.dump());
    };
    updateCursor();
    if (index.dump().size() > MaximumBoundedReadResponseBytes)
        return failure<Json>(Domain::ErrorCodes::PayloadTooLarge,
            "Policy index metadata exceeds the bounded reader limit.");
    const auto append = [&](const char* field, const Json& source, std::size_t& end) {
        while (end < source.size()) {
            index[field].push_back(source.at(end));
            ++end;
            updateCursor();
            if (index.dump().size() <= MaximumBoundedReadResponseBytes) continue;
            auto alone = index;
            alone["coverage"] = Json::array();
            alone["agent_guidance"] = Json::array();
            alone[field].push_back(source.at(end - 1U));
            alone["coverage_offset"] = coverageEnd - (std::string_view{field} == "coverage" ? 1U : 0U);
            alone["guidance_offset"] = guidanceEnd - (std::string_view{field} == "agent_guidance" ? 1U : 0U);
            if (alone.dump().size() > MaximumBoundedReadResponseBytes)
                return failure<void>(Domain::ErrorCodes::PayloadTooLarge,
                    "A policy index item exceeds the bounded reader limit.");
            index[field].erase(index[field].end() - 1);
            --end;
            updateCursor();
            break;
        }
        return Domain::Result<void>::success();
    };
    auto appended = append("coverage", coverage, coverageEnd);
    if (!appended) return propagate<Json>(std::move(appended));
    appended = append("agent_guidance", guidance, guidanceEnd);
    if (!appended) return propagate<Json>(std::move(appended));
    if (!index.at("complete").get<bool>() &&
        coverageEnd == window.coverageOffset && guidanceEnd == window.guidanceOffset)
        return failure<Json>(Domain::ErrorCodes::PayloadTooLarge,
            "A policy index item exceeds the bounded reader limit.");
    return Domain::Result<Json>::success(std::move(index));
}

[[nodiscard]] Domain::Result<void> boundReadContent(
    Json& payload, const Json& arguments, const bool package, const bool file = false)
{
    struct TextWindow final {
        Json* entry;
        std::string text;
        std::size_t offset;
        bool complete;
    };
    std::vector<TextWindow> windows;
    const auto update = [&](TextWindow& window, const std::size_t end) {
        auto& entry = *window.entry;
        const auto selected = window.text.substr(0U, end);
        entry["content"] = selected;
        if (file) {
            const auto startLine = entry.at("start_line").get<std::size_t>();
            const auto newlines = static_cast<std::size_t>(std::count(selected.begin(), selected.end(), '\n'));
            const auto endLine = selected.empty() ? startLine - 1U :
                startLine + newlines - (selected.ends_with('\n') ? 1U : 0U);
            const bool more = window.offset + end < entry.at("size").get<std::size_t>();
            entry["end_line"] = endLine;
            entry["line_count"] = selected.empty() ? 0U : endLine - startLine + 1U;
            entry["has_more"] = more;
            entry["next_offset"] = nullptr;
            entry["next_byte_offset"] = more ? Json(window.offset + end) : Json(nullptr);
            entry["note"] = more ? "Partial UTF-8 byte page. Continue with byte_offset=" +
                std::to_string(window.offset + end) + ". Do not repeat the same byte_offset." :
                "Reached end of file. Stop paginating this path.";
        } else {
            entry["next_offset"] = window.offset + end;
            entry["complete"] = window.complete && end == window.text.size();
        }
    };
    if (file) {
        TextWindow current{&payload, payload.at("content").get<std::string>(),
            payload.at("byte_offset").get<std::size_t>(), false};
        update(current, current.text.size());
    }
    if (payload.dump().size() <= MaximumBoundedReadResponseBytes)
        return Domain::Result<void>::success();
    const auto select = [&](Json& entry) {
        if (!entry.contains("content") || !entry.at("content").is_string()) return;
        auto content = entry.at("content").get<std::string>();
        if (content.empty()) return;
        const auto offset = file ? entry.at("byte_offset").get<std::size_t>() :
            entry.value("offset", arguments.value("offset", std::size_t{}));
        windows.push_back({&entry, std::move(content), offset, entry.value("complete", false)});
        update(windows.back(), 0U);
    };
    if (package) {
        for (auto& entry : payload.at("entries")) select(entry);
    } else {
        select(payload);
    }
    if (payload.dump().size() > MaximumBoundedReadResponseBytes)
        return failure<void>(Domain::ErrorCodes::PayloadTooLarge,
            "Reader metadata exceeds the bounded response limit.");
    for (auto& window : windows) {
        if (!Domain::isValidUtf8(window.text))
            return failure<void>(Domain::ErrorCodes::InvalidRequest,
                "Reader text is not aligned to a UTF-8 character boundary.");
        std::size_t low{};
        auto high = window.text.size();
        std::size_t kept{};
        while (low <= high) {
            const auto middle = low + (high - low) / 2U;
            const auto end = boundedUtf8End(window.text, 0U, middle);
            update(window, end);
            if (payload.dump().size() <= MaximumBoundedReadResponseBytes) {
                kept = end;
                low = middle + 1U;
            } else {
                if (middle == 0U) break;
                high = middle - 1U;
            }
        }
        if (kept == 0U)
            return failure<void>(Domain::ErrorCodes::PayloadTooLarge,
                "Reader metadata leaves no room for a complete UTF-8 character.");
        update(window, kept);
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> boundFileReadContent(
    Json& payload, const Json& arguments, const std::optional<std::size_t> firstByteOffset)
{
    if (payload.dump().size() <= MaximumBoundedReadResponseBytes)
        return Domain::Result<void>::success();
    if (payload.at("byte_offset").is_null()) {
        const auto original = payload.at("content").get<std::string>();
        std::vector<std::size_t> lineEnds;
        for (std::size_t index = 0U; index < original.size(); ++index)
            if (original[index] == '\n') lineEnds.push_back(index);
        lineEnds.push_back(original.size());
        const auto startLine = payload.at("start_line").get<std::size_t>();
        const auto originalHasMore = payload.at("has_more").get<bool>();
        const auto update = [&](const std::size_t count) {
            payload["content"] = count == 0U ? "" : original.substr(0U, lineEnds.at(count - 1U));
            payload["end_line"] = startLine + count - 1U;
            payload["line_count"] = count;
            const bool more = count < lineEnds.size() || originalHasMore;
            payload["has_more"] = more;
            payload["next_offset"] = more ? Json(startLine + count) : Json(nullptr);
            payload["note"] = more ? "Partial line page. Continue with offset=" +
                std::to_string(startLine + count) + " (1-based) and a new length." :
                "Reached end of file. Stop paginating this path.";
        };
        std::size_t low{};
        auto high = lineEnds.size();
        std::size_t kept{};
        while (low <= high) {
            const auto middle = low + (high - low) / 2U;
            update(middle);
            if (payload.dump().size() <= MaximumBoundedReadResponseBytes) {
                kept = middle;
                low = middle + 1U;
            } else {
                if (middle == 0U) break;
                high = middle - 1U;
            }
        }
        if (kept != 0U) {
            update(kept);
            return Domain::Result<void>::success();
        }
        if (!firstByteOffset)
            return failure<void>(Domain::ErrorCodes::IntegrityFailure,
                "The file reader has no source byte offset for its first line.");
        payload["content"] = original.substr(0U, lineEnds.front());
        payload["byte_offset"] = *firstByteOffset;
    }
    return boundReadContent(payload, arguments, false, true);
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

    [[nodiscard]] Json durableManagerStatus() const
    {
        const bool available = static_cast<bool>(dependencies_.durableToolBroker) ||
            (dependencies_.workerRuns && dependencies_.workerRuns() != nullptr) ||
            (dependencies_.reviewerRuns && dependencies_.reviewerRuns() != nullptr);
        return Json{{"available", available}, {"startup_error",
            dependencies_.managerStartupError.empty() ? Json(nullptr) : Json(dependencies_.managerStartupError)}};
    }

    [[nodiscard]] Domain::Result<Json> workspaceContext(
        const Domain::ProjectId& projectId,
        const std::optional<Domain::PathText>& preferredRoot,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto descriptor = dependencies_.projectRegistry.descriptor(
                projectId, context);
            if (!descriptor) {
                return propagate<Json>(std::move(descriptor));
            }

            Json roots = Json::array();
            for (const auto& alias : descriptor.value().aliases) {
                roots.push_back(alias.value());
            }
            const auto projectRoot = preferredRoot
                ? preferredRoot->value()
                : descriptor.value().aliases.empty()
                    ? std::string{}
                    : descriptor.value().aliases.front().value();
            if (projectRoot.empty()) {
                return failure<Json>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The registered MCP project has no project folder.");
            }

            struct PackageRow final {
                std::uint64_t order{};
                Json value;
            };
            std::vector<PackageRow> packageRows;
            std::size_t invalidPackageRows{};
            bool packageReadAvailable{};
            bool packagesTruncated{};
            Json packageReadError = nullptr;
            const Domain::OperationContext projectMemoryContext{
                context.operationId,
                (std::min)(
                    context.deadline,
                    dependencies_.clock.monotonicNow() +
                        std::chrono::seconds{30}),
                context.cancellation,
                context.correlationId};
            auto packages = dependencies_.projectMemory.listRecent(
                Domain::ListRecentProjectMemoryRequest{
                    projectId,
                    {"instruction_package_queue"},
                    std::nullopt,
                    MaximumStatusInstructionPackages,
                    std::nullopt,
                    true,
                    256U * 1024U},
                projectMemoryContext);
            if (packages && packages.value().records.empty()) {
                auto savedOrder = dependencies_.projectMemory.listRecent(
                    Domain::ListRecentProjectMemoryRequest{
                        projectId, {"instruction_package_queue_order"}, std::nullopt,
                        1U, std::nullopt, true, 64U * 1024U}, projectMemoryContext);
                if (!savedOrder) return propagate<Json>(std::move(savedOrder));
                if (savedOrder.value().records.empty()) {
                    auto migrated = Application::migrateLegacyInstructionPackage(
                        dependencies_.projectMemory, dependencies_.hasher,
                        dependencies_.clock, projectId, projectMemoryContext);
                    if (!migrated) return propagate<Json>(std::move(migrated));
                    if (migrated.value()) {
                        packages.value().records.push_back(
                            Domain::MemorySearchHit{std::move(*migrated.value()), 1.0});
                    }
                }
            }
            if (packages) {
                packageReadAvailable = true;
                packagesTruncated = packages.value().truncated ||
                    packages.value().nextCursor.has_value();
                for (const auto& hit : packages.value().records) {
                    if (!hit.record.body) {
                        ++invalidPackageRows;
                        continue;
                    }
                    try {
                        const auto saved = Json::parse(*hit.record.body);
                        if (saved.value("schema", std::string{}) !=
                                "forge-instruction-package-queue-v2" ||
                            saved.value("project_id", std::string{}) !=
                                projectId.value() ||
                            !saved.contains("package_path") ||
                            !saved.at("package_path").is_string()) {
                            ++invalidPackageRows;
                            continue;
                        }
                        packageRows.push_back(PackageRow{
                            saved.value("order", std::uint64_t{}),
                            Json{
                                {"queue_row_id", saved.at("queue_row_id")},
                                {"name", saved.value(
                                    "package_name", hit.record.title)},
                                {"path", saved.at("package_path")},
                                {"revision", saved.value(
                                    "revision", std::string{})},
                                {"state", saved.value(
                                    "state", std::string{"unknown"})},
                                {"order", saved.value(
                                    "order", std::uint64_t{})}}});
                    } catch (...) {
                        ++invalidPackageRows;
                    }
                }
                auto orderPage = dependencies_.projectMemory.listRecent(
                    Domain::ListRecentProjectMemoryRequest{
                        projectId, {"instruction_package_queue_order"}, std::nullopt,
                        1U, std::nullopt, true, 64U * 1024U}, projectMemoryContext);
                if (!orderPage) return propagate<Json>(std::move(orderPage));
                if (!orderPage.value().records.empty() &&
                    orderPage.value().records.front().record.body) {
                    const auto order = Json::parse(*orderPage.value().records.front().record.body);
                    if (order.at("schema") != "forge-instruction-package-order-v1" ||
                        order.at("project_id") != projectId.value()) {
                        return failure<Json>(Domain::ErrorCodes::IntegrityFailure,
                            "The instruction package order is invalid.");
                    }
                    std::uint64_t position{};
                    for (const auto& id : order.at("rows")) {
                        position += 1024U;
                        for (auto& row : packageRows) {
                            if (row.value.at("queue_row_id") == id) {
                                row.order = position;
                                row.value["order"] = position;
                            }
                        }
                    }
                }
                std::ranges::sort(
                    packageRows,
                    [](const PackageRow& left, const PackageRow& right) {
                        return left.order != right.order ? left.order < right.order :
                            left.value.at("queue_row_id") < right.value.at("queue_row_id");
                    });
            } else {
                packageReadError = Json{
                    {"code", packages.error().code},
                    {"message", packages.error().message},
                    {"retryable", packages.error().retryable}};
            }
            Json packageValues = Json::array();
            for (auto& row : packageRows) {
                packageValues.push_back(std::move(row.value));
            }

            Json policy{
                {"available", dependencies_.projectPolicy != nullptr},
                {"active", false},
                {"state", "not_configured"},
                {"source", nullptr},
                {"revision", nullptr},
                {"entry_count", 0U},
                {"coverage_gap_count", 0U},
                {"read_and_follow_required", false},
                {"instruction", "No development policy is configured."},
                {"read_tool", "project_policy.read"}};
            if (dependencies_.projectPolicy != nullptr) {
                auto inspected = dependencies_.projectPolicy->execute(
                    {projectId, Contracts::ProjectPolicyAction::Inspect},
                    context);
                if (!inspected) {
                    policy["available"] = false;
                    policy["state"] = "unavailable";
                } else {
                    try {
                        const auto value = Json::parse(inspected.value());
                        const bool active = value.value("active", false);
                        policy["active"] = active;
                        policy["state"] = value.value(
                            "state", active ? std::string{"enforcing"}
                                             : std::string{"not_configured"});
                        policy["source"] = active && value.contains("source")
                            ? value.at("source")
                            : Json(nullptr);
                        policy["revision"] = active && value.contains("revision")
                            ? value.at("revision")
                            : Json(nullptr);
                        policy["read_and_follow_required"] = active;
                        policy["instruction"] = active
                            ? "Read and follow the development policy before project work."
                            : "No development policy is configured.";
                        policy["entry_count"] = value.value(
                            "entry_count", std::size_t{});
                        policy["coverage_gap_count"] = value.value(
                            "coverage_gap_count", std::size_t{});
                    } catch (...) {
                        policy["available"] = false;
                        policy["state"] = "integrity_failure";
                    }
                }
            }

            return Domain::Result<Json>::success(Json{
                {"workspace",
                 Json{
                     {"project_id", descriptor.value().id.value()},
                     {"display_name", descriptor.value().displayName},
                     {"project_root", projectRoot},
                     {"binding_source", "registered_project"},
                     {"startup_binding_source", dependencies_.startupBindingSource},
                     {"continuity_packet_independent", true},
                     {"authorized_roots", std::move(roots)}}},
                {"instruction_packages",
                 Json{
                     {"available", packageReadAvailable},
                     {"read_in_order", true},
                     {"read_tool", "instruction_package.read"},
                     {"instruction",
                      "Read and follow these folders in the listed order before project work."},
                     {"count", packageValues.size()},
                     {"truncated", packagesTruncated},
                     {"invalid_rows", invalidPackageRows},
                     {"error", std::move(packageReadError)},
                     {"packages", std::move(packageValues)}}},
                {"development_policy", std::move(policy)}});
        } catch (...) {
            return failure<Json>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP workspace context could not be projected.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> projectHandoff(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context)
    {
        auto pointer = dependencies_.legacyMemory.get(
            {"continuity/project/" + projectId.value()}, context);
        if (!pointer) {
            return propagate<Domain::LegacyContinuityGetOutcome>(std::move(pointer));
        }
        if (!pointer.value().note) {
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success({});
        }
        auto id = parseOpaque<Domain::LegacyHandoffId>(
            pointer.value().note->body, "project handoff id");
        if (!id) {
            return propagate<Domain::LegacyContinuityGetOutcome>(std::move(id));
        }
        auto packet = dependencies_.legacyContinuity.get(
            {std::move(id).value(), true}, context);
        if (packet && packet.value().record &&
            (!packet.value().record->packet.resumeReady ||
             packet.value().record->packet.source != Domain::LegacyHandoffSource::Model)) {
            packet.value().record.reset();
        }
        return packet;
    }

    [[nodiscard]] Domain::Result<std::string> bootstrapInstructions(
        const Domain::ProjectId& projectId,
        const Domain::PathText& projectRoot,
        const Domain::OperationContext& context) noexcept
    {
        auto projected = workspaceContext(projectId, projectRoot, context);
        if (!projected) {
            return propagate<std::string>(std::move(projected));
        }
        try {
            const auto& value = projected.value();
            const auto& workspace = value.at("workspace");
            const auto& packages = value.at("instruction_packages");
            const auto& policy = value.at("development_policy");
            std::string instructions =
                "Forge Conductor has bound this MCP session to an explicit project.\n"
                "Project folder: " + workspace.at("project_root").get<std::string>() +
                "\nProject ID: " + workspace.at("project_id").get<std::string>() +
                "\nRead and follow the instruction package folders in the listed order "
                "before project work. Call instruction_package.read with each queue_row_id from get_forge_status.\nInstruction package folders (ordered):";
            const auto& rows = packages.at("packages");
            if (rows.empty()) {
                instructions += " none configured";
            } else {
                const auto count = (std::min)(
                    rows.size(), MaximumBootstrapInstructionPackages);
                for (std::size_t index{}; index < count; ++index) {
                    const auto& package = rows.at(index);
                    instructions += "\n- " + package.at("path").get<std::string>() +
                        " [" + package.value("state", std::string{"unknown"}) + "]";
                }
                if (rows.size() > count || packages.value("truncated", false)) {
                    instructions += "\n- Additional package folders: call forge_status.";
                }
            }
            instructions += "\nDevelopment policy source: ";
            if (policy.value("active", false) &&
                policy.at("source").is_string()) {
                instructions += policy.at("source").get<std::string>();
                if (policy.at("revision").is_string()) {
                    instructions += "\nDevelopment policy revision: " +
                        policy.at("revision").get<std::string>();
                }
                instructions +=
                    "\nRead and follow the development policy. Use project_policy.read "
                    "for its exact adopted index or document.";
            } else {
                instructions += "none configured";
            }
            instructions +=
                "\nCall forge_status for this complete structured context. The Forge home "
                "path is application data only, not the project folder.";
            instructions +=
                "\nCall host_capabilities before claiming that a tool category is missing. "
                "Native tools include web_search/web_fetch/http_request; document_write (.docx), "
                "spreadsheet_write (.xlsx), presentation_write (.pptx), PDF, image_read/image_write/image_analyze, "
                "desktop_list/read/capture/click/type/key and browser_open. image_write draws shapes and text. "
                "image_analyze starts a read-only interpretation of an existing image; reviewer_status reads that analysis result. "
                "Drawing and analysis do not start generative image jobs. Image previews may be displayed without supplying pixels to the chat model. "
                "comfy_status inspects full local ComfyUI automation. Discover node/model/workflow inventories with comfy_catalog; "
                "import or patch graphs with comfy_workflow, preflight with comfy_validate, and prepare dependencies with comfy_prepare. "
                "When comfy_status reports configuration.enabled=true, route every new image or video creation request through comfy_run, including ordinary images. "
                "After comfy_control, do not switch to legacy image_generate. First discover installed starter plans with comfy_catalog kind=templates, "
                "import their returned preview and final workflow paths with comfy_workflow action=import, and retain the executable graphs returned by action=export. "
                "Before choosing a creation workflow, read comfy_status for effective limits and configuration.quality_preference. "
                "When the operator omits quality, honor that configured default; prefer Wan 2.2 5B for balanced video. "
                "Start from installed models and catalog templates, choosing other quality workflows only from actual contracts and measured resources. "
                "Never apply a universal resolution, frame-count or sampling rewrite to unknown graphs. "
                "When video duration is omitted, propose about five seconds. Derive draft and final settings from actual node contracts and measured resources. "
                "For final video delivery, choose playable MP4 with H.264 from the discovered output node contracts unless the operator requests a supported alternative format or codec. "
                "Preserve a supported requested alternative and save the selected format in the proposed final graph before preview approval; a later format change requires a new preview and approval. "
                "Do not apply a universal output-format rewrite to unknown graphs. "
                "Use comfy_run stage=preview with media_kind=image|video|mixed and explicit preview/final graphs; "
                "every literal file input marked image_upload, audio_upload or video_upload by its actual node schema requires an explicit authorized inputs binding in both graphs; source and private provider-copy bytes are sealed. "
                "a motion preview must decode as video, GIF, APNG or animated WebP with positive duration and at least two frames; every intended motion output node must have its own decoded playable artifact. Poll comfy_job_status until awaiting_preview_approval. "
                "For an image preview, call image_read with the verified preview image artifact.path to display its larger bounded image before requesting approval; the job-status thumbnail alone is insufficient. "
                "Successful decoding verifies readable media, not the requested subject or motion quality. "
                "Before describing video content or quality, call image_analyze with the path of the published artifact whose role is sampled_video_contact_sheet, then poll reviewer_status for actual findings. "
                "Report unavailable contact sheets and review failures with their actual errors; limit visual claims to reviewed sampled frames and do not claim unverified motion quality or repeat the prompt as observed content. "
                "For a verified video preview or final artifact with provider_view_url, call browser_open with that exact URL while ComfyUI is running, then use desktop_read on the observed browser window to check its actual address. Launch acceptance alone does not verify page loading or playback. Report actual browser-launch or observation failures. Present the URL as a copyable reference and the complete artifacts[].path in a copyable fenced block; LM Studio 0.4.26+4's ordinary Markdown link path rejects loopback URLs. Never abbreviate file paths. Retain the sampled-frame preview. "
                "If requires_new_preview is true, apply the operator's requested revisions to new preview/final graphs and run a new preview before requesting approval. "
                "Present the actual local preview artifacts and approval_reply_choices (approved, yes, render final) to the operator, "
                "then wait for a later native user message approving it before stage=final with plan_id only. "
                "The saved preview-result evidence must precede the approval; a model-provided approval flag cannot approve generation. "
                "Image and video creation through this automation feature must use the managed comfy_run approval path; do not submit directly through shell, HTTP or browser queue controls. "
                "Poll or recover jobs with comfy_job_list/status/cancel/resume. Desktop scroll and drag support observed UI adjustments. "
                "image_provider_status checks the explicitly configured optional ComfyUI image provider. "
                "Use legacy image_generate or image_edit only when the operator separately requests those SD1 or masked-editing contracts, or ComfyUI automation is disabled. "
                "Poll image_job_status for the generated artifact, cancel with image_job_cancel, "
                "or reattach to the exact existing provider job with image_job_resume without generation replay. "
                "Cloud media services require their connected providers. "
                "agent_spawn/poll/cancel run independent scoped tasks, and schedule_create/list/run_now/cancel persist scheduled work. "
                "The connector starts or attaches to its matching Manager; inspect durable_manager availability and startup_error. "
                "Filesystem mode is selected by the owner; host mode grants available local volumes under ordinary Windows permissions "
                "while relative paths and project memory keep the selected project directory. "
                "Use actual tool results to establish availability and failures.";
            instructions +=
                "\nShell execution: shell_exec runs foreground PowerShell for at most 120 seconds; "
                "its descendants are terminated when the shell exits or times out. "
                "Normal Windows user/profile variables are included when available; Python defaults to UTF-8. "
                "Arbitrary host secrets are not inherited.";
            if (dependencies_.shell.supportsJobs()) {
                instructions +=
                    "\nFor long builds/tests use shell_job_start(command, cwd, timeout_sec), then poll "
                    "shell_job_status(job_id) on this same connector every 5 seconds or longer until done=true. "
                    "Running is not failure; final result.ok, exit_code and timeout/cancellation flags determine success. "
                    "Jobs default to 1800 seconds, maximum 3600, with two active jobs and sixteen retained results. "
                    "cmake_test_run(build_dir,mode,filter,config,target) runs CTest in an initialized CMake build tree; "
                    "mode=build_and_test first builds and only then tests after success. cmake_test_status returns actual "
                    "phase results, sealed JUnit counts and paged failures; missing results remain null. "
                    "process_launch(command,args,cwd,env) uses exact argv and returns a stable job_id, PID and named logs; "
                    "process_wait/poll/read_log/list/kill/adopt support reconnects when the persistent Manager is available. "
                    "Read durable output with process_read_log, and inspect memory_attached after completion. "
                    "Use shell_job_list to recover IDs and shell_job_cancel to stop one. "
                    "Keep the command in the foreground. Do not replace this with detached Start-Process or Task Scheduler. "
                    "Manager-owned jobs survive MCP reconnect; stopping their owning Manager cancels them. Without a matching Manager, status reports connector_process lifetime and reconnect cancels running jobs.";
            }
            auto handoff = projectHandoff(projectId, context);
            if (!handoff) {
                return propagate<std::string>(std::move(handoff));
            }
            if (handoff.value().record) {
                const auto& packet = handoff.value().record->packet;
                instructions +=
                    "\nContinuity packet for this project: " + packet.id.value() +
                    "\nBefore continuing, call context_get through forge-conductor or "
                    "forge-conductor-fallback with handoff_id=\"" + packet.id.value() +
                    "\" to acknowledge recovery. The CLU connector receives this packet "
                    "as policy context; recover the session through the primary or fallback "
                    "Forge connector. Resume its next_actions, retain its constraints, "
                    "and verify its claims. "
                    "Forge tools and agent-session tools remain available.\n";
                const auto encoded = legacyPacketJson(packet).dump();
                // Keep room for the connection identifier appended by McpServer.
                if (instructions.size() + encoded.size() < 30U * 1024U) {
                    instructions += "Continuity packet body:\n" + encoded;
                } else {
                    instructions += "The full packet exceeds the initialize budget; "
                        "context_get returns it without truncation. Read it before work.";
                }
            }
            return Domain::Result<std::string>::success(std::move(instructions));
        } catch (...) {
            return failure<std::string>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP bootstrap instructions could not be formatted.");
        }
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
            std::optional<Domain::PathText> defaultWorkspace;
            if (dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host) {
                auto root = dependencies_.workspaceAuthority.defaultWorkspacePath(authority, operationContext.value());
                if (!root) return propagate<Domain::ToolCallOutcome>(std::move(root));
                defaultWorkspace = std::move(root).value();
            }
            continuityObservation.seedWorkspace(authority, defaultWorkspace);
            std::optional<Domain::ContextRecoveryReceipt> contextRecovery;
            std::optional<std::size_t> fileReadByteStart;
            auto payload = dispatch(
                authorizedCall,
                authority,
                arguments,
                operationContext.value(),
                continuityObservation,
                contextRecovery,
                fileReadByteStart);
            if (!payload) {
                return propagate<Domain::ToolCallOutcome>(std::move(payload));
            }
            if (!payload.value().is_object()) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::InternalFailure,
                    "The MCP tool adapter produced a non-object payload.");
            }
            std::optional<Domain::LegacyHandoffId> contextPersistence;
            if ((authorizedCall.toolName() == "session_checkpoint" ||
                 authorizedCall.toolName() == "session_handoff") &&
                payload.value().value("ok", true)) {
                const auto id = strictString(payload.value(), "handoff_id");
                if (!id) return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The persisted continuity payload has no handoff id.");
                auto parsed = Domain::LegacyHandoffId::parse(*id);
                if (!parsed) return propagate<Domain::ToolCallOutcome>(std::move(parsed));
                contextPersistence.emplace(std::move(parsed).value());
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
            if (dependencies_.projectPolicy &&
                !authorizedCall.toolName().starts_with("clu.")) {
                auto evidencePayload = payload.value();
                evidencePayload.erase("image_base64");
                auto resultEvidence = evidencePayload.dump();
                if (resultEvidence.size() > 32U * 1024U) {
                    resultEvidence.resize(boundedUtf8End(resultEvidence, 0U, 32U * 1024U));
                }
                const Json evidence{{"phase", "post_operation"},
                    {"tool_name", authorizedCall.toolName()},
                    {"arguments", authorizedCall.canonicalRequest()},
                    {"effect", selected->tool.effect == Domain::ToolEffect::Read
                        ? "read" : selected->tool.effect == Domain::ToolEffect::Write
                        ? "write" : selected->tool.effect == Domain::ToolEffect::Execute
                        ? "execute" : "destructive"},
                    {"result", std::move(resultEvidence)}};
                static_cast<void>(dependencies_.projectPolicy->execute(
                    {authority.projectId(), Contracts::ProjectPolicyAction::Evaluate,
                     {}, {}, evidence.dump()}, operationContext.value()));
            }
            const bool boundedRead = authorizedCall.toolName() == "project_policy.read" ||
                authorizedCall.toolName() == "instruction_package.read" ||
                authorizedCall.toolName() == "fs_read";
            if (dependencies_.projectPolicy && boundedRead) {
                payload.value()["clu_governance_notifications_deferred"] = true;
                payload.value()["clu_governance_notifications_read_tool"] = "clu.findings";
            }
            if (dependencies_.projectPolicy && !boundedRead &&
                !authorizedCall.toolName().starts_with("clu.")) {
                auto guidance = dependencies_.projectPolicy->execute(
                    {authority.projectId(),
                     Contracts::ProjectPolicyAction::ListFindings, {}, {},
                     Json{{"acknowledge_notifications", true}}.dump()},
                    operationContext.value());
                if (guidance) {
                    auto value = Json::parse(guidance.value(), nullptr, false);
                    if (value.is_object() && value.contains("notifications") &&
                        !value.at("notifications").empty()) {
                        payload.value()["clu_governance_notifications"] =
                            value.at("notifications");
                    }
                }
            }
            if (authorizedCall.toolName() == "project_policy.read" &&
                !payload.value().contains("content")) {
                auto bounded = boundedPolicyIndex(std::move(payload).value(), arguments,
                    authority.projectId(), dependencies_.hasher);
                if (!bounded) return propagate<Domain::ToolCallOutcome>(std::move(bounded));
                payload = std::move(bounded);
            } else if (authorizedCall.toolName() == "project_policy.read" ||
                       authorizedCall.toolName() == "instruction_package.read") {
                auto bounded = boundReadContent(payload.value(), arguments,
                    authorizedCall.toolName() == "instruction_package.read");
                if (!bounded) return propagate<Domain::ToolCallOutcome>(std::move(bounded));
            }
            if (authorizedCall.toolName() == "fs_read") {
                auto bounded = boundFileReadContent(payload.value(), arguments, fileReadByteStart);
                if (!bounded) return propagate<Domain::ToolCallOutcome>(std::move(bounded));
            }
            auto encoded = codec.canonicalize(payload.value().dump());
            if (!encoded) {
                return propagate<Domain::ToolCallOutcome>(std::move(encoded));
            }
            const bool ok = payload.value().value("ok", true);
            if (dependencies_.visibleChatObservation) {
                try {
                    auto workspace = dependencies_.clientWorkspaceContext.snapshot(authorizedCall.clientId(), operationContext.value());
                    if (workspace && workspace.value()) dependencies_.visibleChatObservation(
                        workspace.value()->projectId, workspace.value()->authorityRoot, authorizedCall.toolName(), ok, encoded.value(), operationContext.value());
                    else if (defaultWorkspace) dependencies_.visibleChatObservation(
                        authority.projectId(), *defaultWorkspace, authorizedCall.toolName(), ok, encoded.value(), operationContext.value());
                    else if (!authority.trustedRoots().empty()) dependencies_.visibleChatObservation(
                        authority.projectId(), authority.trustedRoots().front(), authorizedCall.toolName(), ok, encoded.value(), operationContext.value());
                } catch (...) { /* Optional observation cannot change a completed tool. */ }
            }
            if (dependencies_.visibleChatWorkspaceBinding) {
                try {
                    auto workspace = dependencies_.clientWorkspaceContext.snapshot(
                        authorizedCall.clientId(), operationContext.value());
                    if (workspace && workspace.value()) {
                        dependencies_.visibleChatWorkspaceBinding(
                            workspace.value()->projectId, workspace.value()->authorityRoot);
                    } else if (defaultWorkspace) {
                        dependencies_.visibleChatWorkspaceBinding(authority.projectId(), *defaultWorkspace);
                    } else if (!authority.trustedRoots().empty()) {
                        dependencies_.visibleChatWorkspaceBinding(
                            authority.projectId(), authority.trustedRoots().front());
                    }
                } catch (...) {
                    // Optional native workspace observation cannot change a completed tool.
                }
            }
            if (dependencies_.visibleChatToolResult) {
                try {
                    dependencies_.visibleChatToolResult(authorizedCall.toolName(), ok, encoded.value());
                } catch (...) {
                    // Optional chat observation cannot turn a completed tool into a failure.
                }
            }
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
                    std::move(observation),
                    std::move(contextPersistence)});
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
        std::optional<Domain::ContextRecoveryReceipt>& contextRecovery,
        std::optional<std::size_t>& fileReadByteStart)
    {
        const auto& name = call.toolName();

        if (name == "agent_spawn" || name == "agent_poll" || name == "agent_cancel" || name.starts_with("schedule_")) {
            if (dependencies_.durableToolBroker) {
                auto brokerArguments = arguments;
                const auto accessName = [](const Domain::FileAccess access) {
                    switch (access) {
                    case Domain::FileAccess::Read: return "read";
                    case Domain::FileAccess::Write: return "write";
                    case Domain::FileAccess::Create: return "create";
                    case Domain::FileAccess::Delete: return "delete";
                    case Domain::FileAccess::Execute: return "execute";
                    }
                    return "unknown";
                };
                Json roots = Json::array(), grants = Json::array(), denials = Json::array();
                for (const auto& root : authority.trustedRoots()) roots.push_back(root.value());
                for (const auto access : authority.grants()) grants.push_back(accessName(access));
                for (const auto access : authority.denials()) denials.push_back(accessName(access));
                brokerArguments["_forge_worker_scope"] = Json{{"trusted_roots", std::move(roots)},
                    {"grants", std::move(grants)}, {"denials", std::move(denials)}, {"shell_enabled", authority.shellEnabled()}};
                auto result = dependencies_.durableToolBroker(name, brokerArguments.dump(), authority.projectId(), context);
                if (!result) return propagate<Json>(std::move(result));
                return Domain::Result<Json>::success(Json::parse(result.value()));
            }
            auto* scheduler = dependencies_.scheduledTasks ? dependencies_.scheduledTasks() : nullptr;
            auto result = name.starts_with("schedule_")
                ? scheduler
                    ? scheduler->execute(name, arguments.dump(), authority, context)
                    : failure<std::string>(Domain::ErrorCodes::HostCapabilityUnavailable, "Persistent model schedules require the Manager.")
                : executeAgentWorkerTool(name, arguments.dump(), authority, context,
                    {dependencies_.workerRuns, dependencies_.agentCatalog, dependencies_.catalog, dependencies_.uuidGenerator});
            if (!result) return propagate<Json>(std::move(result));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name.starts_with("comfy_")) {
            if (dependencies_.durableToolBroker) {
                auto brokerArguments = arguments;
                const auto canonicalize = [&](Json& value, const char* field, Domain::FileAccess access) -> Domain::Result<void> {
                    const auto found = value.find(field);
                    if (found == value.end()) return Domain::Result<void>::success();
                    const auto& path = found->get_ref<const std::string&>();
                    if (!isAbsoluteToolPath(path)) return failure<void>(Domain::ErrorCodes::InvalidRequest,
                        "ComfyUI paths must be absolute.");
                    auto authorized = authorizePath(dependencies_.workspaceAuthority, authority, path, access,
                        access != Domain::FileAccess::Read, context, &observation, ContinuityPathRole::Path);
                    if (!authorized) return propagate<void>(std::move(authorized));
                    value[field] = authorized.value().canonicalPath().value();
                    return Domain::Result<void>::success();
                };
                if (name == "comfy_workflow") {
                    if(arguments.at("action")=="import" && brokerArguments.contains("path")) {
                        if(!isAbsoluteToolPath(brokerArguments.at("path").get_ref<const std::string&>()))
                            return failure<Json>(Domain::ErrorCodes::InvalidRequest,"ComfyUI paths must be absolute.");
                        // The native provider owns authorization for its fixed installed
                        // workflow catalogs as well as ordinary project files.
                    } else {
                        auto checked = canonicalize(brokerArguments,"path",arguments.at("action")=="export"?Domain::FileAccess::Write:Domain::FileAccess::Read);
                        if (!checked) return propagate<Json>(std::move(checked));
                    }
                }
                if (name == "comfy_run") {
                    if (brokerArguments.contains("output_directory")) {
                        const auto directory = brokerArguments.at("output_directory").get<std::string>();
                        Json outputs{{"preview",directory + "/preview"},{"final",directory + "/final"}};
                        auto checked = canonicalize(outputs,"preview",Domain::FileAccess::Create);
                        if (!checked) return propagate<Json>(std::move(checked));
                        checked = canonicalize(outputs,"final",Domain::FileAccess::Create);
                        if (!checked) return propagate<Json>(std::move(checked));
                        const auto preview = outputs.at("preview").get<std::string>();
                        brokerArguments["output_directory"] = preview.substr(0U,preview.find_last_of("/\\") + 1U);
                    }
                    if (brokerArguments.contains("inputs")) for (auto& input : brokerArguments["inputs"]) {
                        auto checked = canonicalize(input, "path", Domain::FileAccess::Read);
                        if (!checked) return propagate<Json>(std::move(checked));
                    }
                }
                const auto accessName = [](Domain::FileAccess access) {
                    switch (access) {
                    case Domain::FileAccess::Read: return "read";
                    case Domain::FileAccess::Write: return "write";
                    case Domain::FileAccess::Create: return "create";
                    case Domain::FileAccess::Delete: return "delete";
                    case Domain::FileAccess::Execute: return "execute";
                    }
                    return "unknown";
                };
                Json roots = Json::array(), grants = Json::array(), denials = Json::array();
                for (const auto& root : authority.trustedRoots()) roots.push_back(root.value());
                for (auto access : authority.grants()) grants.push_back(accessName(access));
                for (auto access : authority.denials()) denials.push_back(accessName(access));
                brokerArguments["_forge_comfy_scope"] = Json{{"project_id", authority.projectId().value()},
                    {"caller_id", authority.callerId().value()}, {"generation", authority.generation()},
                    {"trusted_roots", std::move(roots)}, {"grants", std::move(grants)},
                    {"denials", std::move(denials)}, {"shell_enabled", authority.shellEnabled()}};
                auto result = dependencies_.durableToolBroker(name, brokerArguments.dump(), authority.projectId(), context);
                if (!result) return propagate<Json>(std::move(result));
                auto payload = Json::parse(result.value()); payload["broker"] = "persistent_manager";
                if (name == "comfy_run" || name == "comfy_job_status" || name == "comfy_job_resume" || name == "comfy_job_cancel")
                    return promoteComfyJobPreview(std::move(payload));
                return Domain::Result<Json>::success(std::move(payload));
            }
            if (!dependencies_.comfyUi) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "ComfyUI automation requires the durable Manager and configured local installation.");
            auto result = dependencies_.comfyUi->execute(name, arguments.dump(), authority, context);
            if (!result) return propagate<Json>(std::move(result));
            if (name == "comfy_run" || name == "comfy_job_status" || name == "comfy_job_resume" || name == "comfy_job_cancel")
                return promoteComfyJobPreview(Json::parse(result.value()));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name == "image_provider_status" || name == "image_generate" || name == "image_edit" ||
            name == "image_job_status" || name == "image_job_cancel" || name == "image_job_resume") {
            if (dependencies_.durableToolBroker) {
                auto brokerArguments = arguments;
                if (name == "image_generate" || name == "image_edit") {
                    for (const auto field : {"path", "source_path", "mask_path"}) {
                        const auto found = arguments.find(field);
                        if (found == arguments.end()) continue;
                        const auto& path = found->get_ref<const std::string&>();
                        if (!isAbsoluteToolPath(path)) return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                            "Image provider paths must be absolute.");
                        auto authorized = authorizePath(dependencies_.workspaceAuthority, authority, path,
                            std::string_view{field} == "path" ? Domain::FileAccess::Write : Domain::FileAccess::Read,
                            false, context, &observation, ContinuityPathRole::Path);
                        if (!authorized) return propagate<Json>(std::move(authorized));
                        brokerArguments[field] = authorized.value().canonicalPath().value();
                    }
                }
                const auto accessName = [](const Domain::FileAccess access) {
                    switch (access) {
                    case Domain::FileAccess::Read: return "read";
                    case Domain::FileAccess::Write: return "write";
                    case Domain::FileAccess::Create: return "create";
                    case Domain::FileAccess::Delete: return "delete";
                    case Domain::FileAccess::Execute: return "execute";
                    }
                    return "unknown";
                };
                Json roots = Json::array(), grants = Json::array(), denials = Json::array();
                for (const auto& root : authority.trustedRoots()) roots.push_back(root.value());
                for (const auto access : authority.grants()) grants.push_back(accessName(access));
                for (const auto access : authority.denials()) denials.push_back(accessName(access));
                brokerArguments["_forge_image_scope"] = Json{{"project_id", authority.projectId().value()},
                    {"caller_id", authority.callerId().value()}, {"generation", authority.generation()},
                    {"trusted_roots", std::move(roots)}, {"grants", std::move(grants)},
                    {"denials", std::move(denials)}, {"shell_enabled", authority.shellEnabled()}};
                auto result = dependencies_.durableToolBroker(name, brokerArguments.dump(), authority.projectId(), context);
                if (!result) return propagate<Json>(std::move(result));
                auto payload = Json::parse(result.value());
                payload["broker"] = "persistent_manager";
                return Domain::Result<Json>::success(std::move(payload));
            }
            if (!dependencies_.imageProvider) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Optional image generation requires the durable Manager and an explicitly configured provider.");
            auto result = dependencies_.imageProvider->execute(name, arguments.dump(), authority, context);
            if (!result) return propagate<Json>(std::move(result));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name == "host_capabilities") {
            Json names = Json::array();
            for (const auto& item : dependencies_.catalog.tools()) names.push_back(item.tool.name);
            Json roots = Json::array();
            for (const auto& root : authority.trustedRoots()) roots.push_back(root.value());
            const bool hostAccess = dependencies_.workspaceAuthority.fileSystemAccessMode() ==
                Domain::FileSystemAccessMode::Host;
            const bool independentReview = static_cast<bool>(dependencies_.durableToolBroker) ||
                (dependencies_.reviewerRuns && dependencies_.reviewerRuns() != nullptr);
            return Domain::Result<Json>::success(Json{{"ok", true}, {"version", dependencies_.productVersion},
                {"tool_count", names.size()}, {"tools", std::move(names)},
                {"durable_manager", durableManagerStatus()},
                {"filesystem_access", hostAccess ? "host" : "workspace"}, {"active_roots", std::move(roots)},
                {"dedicated", {{"web_fetch_search_http", dependencies_.webAccess != nullptr},
                    {"word_excel_powerpoint_creation", dependencies_.artifactDocuments != nullptr},
                    {"desktop_accessibility_capture_input", dependencies_.desktopArtifacts != nullptr},
                    {"native_image_drawing_and_vision_preview", dependencies_.desktopArtifacts != nullptr},
                    {"independent_image_analysis", dependencies_.desktopArtifacts != nullptr && independentReview},
                    {"comfyui_workflow_automation", dependencies_.comfyUi != nullptr || static_cast<bool>(dependencies_.durableToolBroker)},
                    {"optional_generative_image_provider_adapter", dependencies_.imageProvider != nullptr ||
                        static_cast<bool>(dependencies_.durableToolBroker)},
                    {"pdf_creation", true}, {"shell_and_process_execution", authority.shellEnabled()},
                    {"independent_read_only_review", independentReview}}},
                {"execution_permissions", "ordinary_windows_account_permissions_without_elevation"},
                {"shell_absolute_paths_and_network", "not_sandboxed_by_filesystem_tool_roots"},
                {"external_connections_required", {"generative_image_model", "cloud_email_calendar_chat_accounts"}},
                {"generative_image_provider", "Optional and disabled by default; call image_provider_status for configured endpoint, node/checkpoint compatibility and current availability."},
                {"agent_playbooks", "agent_run_start_is_a_current_model_specialist_session_not_parallel_inference"},
                {"reviewer_gate_approval", false},
                {"independent_mutable_workers", static_cast<bool>(dependencies_.workerRuns || dependencies_.durableToolBroker)},
                {"independent_worker_limits", {{"maximum_active", 16}, {"timeout_sec_max", 3600},
                    {"output_bytes_max", Domain::MaximumManagedRunOutputBytes},
                    {"receipt_history", "sealed_receipts_retained_by_run_id_subject_to_available_disk_space"}}},
                {"scheduled_task_notifications", "Manager submits local Windows toasts; last_notification reports actual acceptance or error, with display_confirmed=false"},
                {"persistent_model_schedules", static_cast<bool>(dependencies_.scheduledTasks || dependencies_.durableToolBroker)}});
        }
        if (name == "web_fetch" || name == "web_search" || name == "http_request") {
            if (!dependencies_.webAccess) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Native web access is unavailable in this composition.");
            auto result = dependencies_.webAccess->execute(name, arguments.dump(), context);
            if (!result) return propagate<Json>(std::move(result));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name == "document_write" || name == "spreadsheet_write" || name == "presentation_write") {
            if (!dependencies_.artifactDocuments) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Native Office document creation is unavailable in this composition.");
            auto result = dependencies_.artifactDocuments->execute(name, arguments.dump(), authority, context);
            if (!result) return propagate<Json>(std::move(result));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name == "image_analyze") {
            if (std::find(authority.grants().begin(), authority.grants().end(), Domain::FileAccess::Write) == authority.grants().end() ||
                std::find(authority.denials().begin(), authority.denials().end(), Domain::FileAccess::Write) != authority.denials().end())
                return failure<Json>(Domain::ErrorCodes::Unauthorized,
                    "Starting independent image analysis requires write authority; read-only callers cannot start reviews.");
            const auto authorization = arguments.at("authorization").get<std::string>();
            const auto question = arguments.value("question", std::string{
                "Describe the visible background, colors, shapes, text and positions. Identify details that are unclear."});
            if (authorization.size() > 1024U) return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "Image analysis authorization exceeds the reviewer authorization limit of 1024 bytes.");
            for (const auto* text : {&authorization, &question}) {
                if (text->empty() || text->size() > 4096U || text->find('\0') != std::string::npos || !Domain::isValidUtf8(*text))
                    return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                        "Image analysis authorization and question must contain nonempty NUL-free UTF-8 text within 4096 bytes.");
            }
            auto path = authorizePath(dependencies_.workspaceAuthority, authority,
                arguments.at("path").get<std::string>(), Domain::FileAccess::Read, false, context,
                &observation, ContinuityPathRole::Path);
            if (!path) return propagate<Json>(std::move(path));
            if (!dependencies_.desktopArtifacts) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Native image decoding is unavailable in this composition.");
            auto imageArguments = Json{{"path", path.value().canonicalPath().value()}};
            if (arguments.contains("preview_max_dimension")) imageArguments["preview_max_dimension"] = arguments.at("preview_max_dimension");
            auto decoded = dependencies_.desktopArtifacts->execute("image_read", imageArguments.dump(), authority, context);
            if (!decoded) return propagate<Json>(std::move(decoded));
            auto image = Json::parse(decoded.value());
            if (!image.is_object() || !image.value("ok", false) ||
                !image.contains("width") || !image.at("width").is_number_integer() ||
                !image.contains("height") || !image.at("height").is_number_integer() ||
                image.at("width").get<std::int64_t>() < 1 || image.at("width").get<std::int64_t>() > 4096 ||
                image.at("height").get<std::int64_t>() < 1 || image.at("height").get<std::int64_t>() > 4096 ||
                !image.contains("format") || !image.at("format").is_string())
                return failure<Json>(Domain::ErrorCodes::InternalFailure,
                "Native image decoding did not confirm the admitted image.");
            const auto imageAnalysis = Json{{"kind", "fresh_independent_readonly_reviewer"},
                {"path", path.value().canonicalPath().value()}, {"width", image.at("width")}, {"height", image.at("height")},
                {"format", image.at("format")}, {"poll_tool", "reviewer_status"}, {"asynchronous", true},
                {"preview_max_dimension_requested", arguments.value("preview_max_dimension", 256)},
                {"executor_history_included", false}, {"source_read_policy", "authorized_path_read_at_provider_image_read"},
                {"tool_scope", "existing_project_authorized_read_only_reviewer_catalog"}};
            const auto reviewArguments = Json{
                {"authorization", authorization}, {"mode", "tools"},
                {"receive_timeout_sec", arguments.value("receive_timeout_sec", Domain::DefaultReviewerReceiveTimeoutSeconds)},
                {"opening_message", "Independent visual analysis of an authorized local image. Use image_read exactly once with arguments " +
                    imageArguments.dump() + ". Interpret the actual returned pixels, not the filename or metadata. "
                    "If pixels are unavailable, report that instead of guessing. The source is read when image_read runs; "
                    "it is not a frozen copy of the admission preview. Answer this question: " + Json(question).dump()},
                {"task", "Report the visual observations and uncertainty. Do not alter files or approve policy gates."}};
            if (reviewArguments.at("opening_message").get_ref<const std::string&>().size() > Domain::MaximumReviewerOpeningMessageBytes)
                return failure<Json>(Domain::ErrorCodes::PayloadTooLarge,
                    "The encoded image path and question exceed the 64 KiB reviewer opening-message limit.");
            auto reviewed = [&]() -> Domain::Result<Json> {
                if (dependencies_.durableToolBroker) {
                    auto result = dependencies_.durableToolBroker("reviewer_start", reviewArguments.dump(), authority.projectId(), context);
                    if (!result) return propagate<Json>(std::move(result));
                    auto payload = Json::parse(result.value());
                    payload["broker"] = "persistent_manager";
                    return Domain::Result<Json>::success(std::move(payload));
                }
                return reviewer("reviewer_start", authority, reviewArguments, context, observation);
            }();
            if (!reviewed) return propagate<Json>(std::move(reviewed));
            auto result = std::move(reviewed).value();
            result["image_analysis"] = imageAnalysis;
            for (const auto field : {"image_base64", "image_mime_type", "preview_width", "preview_height",
                "preview_max_dimension_requested", "preview_encoded_bytes", "preview_encoded_byte_limit", "preview_reduced_for_byte_limit"})
                if (image.contains(field)) result[field] = image.at(field);
            return Domain::Result<Json>::success(std::move(result));
        }
        if (name.starts_with("desktop_") || name == "browser_open" || name == "image_read" || name == "image_write") {
            if (!dependencies_.desktopArtifacts) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Native desktop/image access is unavailable in this composition.");
            auto result = dependencies_.desktopArtifacts->execute(name, arguments.dump(), authority, context);
            if (!result) return propagate<Json>(std::move(result));
            return Domain::Result<Json>::success(Json::parse(result.value()));
        }
        if (name == "provider_status" || name == "process_status") {
            const auto& inspect = name == "provider_status"
                ? dependencies_.providerInspection : dependencies_.systemInspection;
            if (!inspect) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "The requested host inspection is unavailable in this composition.");
            auto inspected = inspect(context);
            if (!inspected) return propagate<Json>(std::move(inspected));
            return Domain::Result<Json>::success(Json::parse(inspected.value()));
        }
        if (name == "github_read") {
            if (!dependencies_.githubRead) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "GitHub read access is unavailable in this composition.");
            Contracts::GitHubReadRequest request{arguments.at("repository").get<std::string>(),
                arguments.at("operation").get<std::string>()};
            if (arguments.contains("id")) request.id = arguments.at("id").get<std::uint64_t>();
            if (arguments.contains("ref")) request.ref = arguments.at("ref").get<std::string>();
            request.page = arguments.value("page", 1U);
            request.perPage = arguments.value("per_page", 30U);
            auto read = dependencies_.githubRead->read(request, context);
            if (!read) return propagate<Json>(std::move(read));
            return Domain::Result<Json>::success(Json::parse(read.value()));
        }
        if (name == "workspace_authority_bind") {
            auto root = Domain::PathText::create(arguments.at("root").get<std::string>());
            if (!root) return propagate<Json>(std::move(root));
            bool managerBound{};
            if (dependencies_.durableToolBroker) {
                auto brokered = dependencies_.durableToolBroker(name, arguments.dump(), authority.projectId(), context);
                if (!brokered) return propagate<Json>(std::move(brokered));
                const auto confirmation = Json::parse(brokered.value().begin(), brokered.value().end(),
                    nullptr, false, false);
                if (!confirmation.is_object() || !confirmation.contains("ok") ||
                    !confirmation.at("ok").is_boolean() || !confirmation.at("ok").get<bool>())
                    return failure<Json>(Domain::ErrorCodes::IntegrityFailure,
                        "The persistent Manager did not confirm workspace root binding.");
                managerBound = true;
            }
            auto bound = dependencies_.workspaceAuthority.bindConfiguredRoot(authority, root.value(), context);
            if (!bound) {
                auto error = std::move(bound).error();
                if (managerBound) error.message =
                    "The persistent Manager bound owner-configured root " + Json(root.value().value()).dump() +
                    " (manager_bound=true; local_bound=false), but local MCP authority binding failed: " +
                    error.message + ". Inspect workspace_authority.configured_additional_roots and "
                    "the local host's root-binding capability before retrying. Manager binding remains active.";
                return Domain::Result<Json>::failure(std::move(error));
            }
            Json roots = Json::array();
            for (const auto& item : bound.value().trustedRoots()) roots.push_back(item.value());
            return Domain::Result<Json>::success(Json{{"ok", true}, {"project_id", authority.projectId().value()},
                {"active_roots", std::move(roots)}, {"root", root.value().value()},
                {"authority_generation", bound.value().generation()}});
        }
        if (name == "evidence_digest" || name == "evidence_log_read") {
            if (!dependencies_.evidence) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
                "Evidence capture is unavailable in this composition.");
            if (name == "evidence_log_read") {
                auto log = dependencies_.evidence->readLog(authority, arguments.value("offset", std::size_t{}),
                    arguments.value("limit", std::size_t{4}), arguments.value("verify", true), context);
                if (!log) return propagate<Json>(std::move(log));
                return Domain::Result<Json>::success(Json::parse(log.value()));
            }
            std::vector<Domain::PathText> paths;
            for (const auto& item : arguments.at("paths")) {
                auto path = Domain::PathText::create(item.get<std::string>());
                if (!path) return propagate<Json>(std::move(path));
                paths.push_back(std::move(path).value());
            }
            auto captured = dependencies_.evidence->digest(paths, authority, context);
            if (!captured) return propagate<Json>(std::move(captured));
            return Domain::Result<Json>::success(Json::parse(captured.value()));
        }
        if (name.starts_with("process_") || name.starts_with("reviewer_") ||
            name.starts_with("verification_env_") || name.starts_with("shell_job_") || name.starts_with("cmake_test_")) {
            if (dependencies_.durableToolBroker) {
                auto brokerArguments = arguments;
                if (name == "process_launch" || name == "shell_job_start") {
                    auto cwd = authorizePath(dependencies_.workspaceAuthority, authority,
                        arguments.value("cwd", observation.defaultDirectory()), Domain::FileAccess::Execute,
                        false, context, &observation, ContinuityPathRole::WorkingDirectory);
                    if (!cwd) return propagate<Json>(std::move(cwd));
                    brokerArguments["cwd"] = cwd.value().canonicalPath().value();
                }
                if (name == "cmake_test_run") {
                    auto directory = cmakeTestDirectory(authority, arguments, context, observation);
                    if (!directory) return propagate<Json>(std::move(directory));
                    brokerArguments["build_dir"] = directory.value().value();
                }
                if (name == "reviewer_start" && arguments.contains("opening_message_path")) {
                    auto openingPath = authorizePath(dependencies_.workspaceAuthority, authority,
                        arguments.at("opening_message_path").get<std::string>(), Domain::FileAccess::Read,
                        false, context, &observation, ContinuityPathRole::Path);
                    if (!openingPath) return propagate<Json>(std::move(openingPath));
                    brokerArguments["opening_message_path"] = openingPath.value().canonicalPath().value();
                }
                auto brokered = dependencies_.durableToolBroker(name, brokerArguments.dump(), authority.projectId(), context);
                if (!brokered) return propagate<Json>(std::move(brokered));
                auto result = Json::parse(brokered.value());
                result["broker"] = "persistent_manager";
                return Domain::Result<Json>::success(std::move(result));
            }
            if (name.starts_with("cmake_test_")) return cmakeTest(name, authority, arguments, context, observation);
            if (name.starts_with("process_")) return process(name, authority, arguments, context, observation);
            if (name.starts_with("reviewer_")) return reviewer(name, authority, arguments, context, observation);
            if (name.starts_with("verification_env_")) return verificationEnvironment(name, authority, arguments, context, observation);
        }
        if (name == "instruction_package.read") {
            auto workspace = workspaceContext(authority.projectId(), std::nullopt, context);
            if (!workspace) return propagate<Json>(std::move(workspace));
            const auto rowId = arguments.at("queue_row_id").get<std::string>();
            const auto& packages = workspace.value().at("instruction_packages").at("packages");
            const auto selected = std::find_if(packages.begin(), packages.end(),
                [&](const auto& row) { return row.at("queue_row_id") == rowId; });
            if (selected == packages.end()) return failure<Json>(Domain::ErrorCodes::RecordNotFound,
                "The selected instruction package is not in this project's queue.");
            const auto path = arguments.value("path", std::string{});
            const auto offset = arguments.value("offset", std::size_t{});
            std::optional<std::string> cursor;
            if (arguments.contains("cursor")) cursor = arguments.at("cursor").get<std::string>();
            const Domain::OperationContext readContext{context.operationId,
                (std::min)(context.deadline, dependencies_.clock.monotonicNow() + std::chrono::seconds{30}),
                context.cancellation, context.correlationId};
            Json entries = Json::array();
            do {
                auto page = dependencies_.projectMemory.search(
                    Domain::SearchProjectMemoryRequest{authority.projectId(), rowId,
                        {"instruction_package_entry"}, {}, std::nullopt,
                        1U, cursor, true, 256U * 1024U}, readContext);
                if (!page) return propagate<Json>(std::move(page));
                cursor = page.value().nextCursor;
                for (const auto& hit : page.value().records) {
                    if (!hit.record.body) continue;
                    auto entry = Json::parse(*hit.record.body);
                    if (entry.at("queue_row_id") != rowId ||
                        entry.at("revision") != selected->at("revision")) continue;
                    if (!path.empty() && entry.at("relative_path") != path) continue;
                    entry["content"] = nullptr;
                    entry["complete"] = false;
                    if (entry.contains("derived_text") && entry.at("derived_text").is_string()) {
                        const auto text = entry.at("derived_text").get<std::string>();
                        if (offset > text.size() || (offset < text.size() &&
                            (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U))
                            return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                                "The package offset must be a UTF-8 character boundary within the entry.");
                        auto end = (std::min)(text.size(), offset + 16U * 1024U);
                        while (end < text.size() &&
                            (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) --end;
                        entry["content"] = text.substr(offset, end - offset);
                        entry["offset"] = offset;
                        entry["next_offset"] = end;
                        entry["complete"] = end == text.size();
                    }
                    entry.erase("derived_text");
                    entries.push_back(std::move(entry));
                }
            } while (entries.empty() && cursor);
            if (!path.empty() && entries.empty()) return failure<Json>(Domain::ErrorCodes::RecordNotFound,
                "The selected package entry was not found.");
            return Domain::Result<Json>::success(Json{{"package", *selected},
                {"entries", std::move(entries)}, {"next_cursor", cursor ? Json(*cursor) : Json(nullptr)},
                {"instruction", "Read all pages and entries. For an incomplete text entry, call with its path and next_offset. Entries without text retain explicit coverage details."}});
        }
        if (name == "project_policy.read") {
            if (!dependencies_.projectPolicy) return failure<Json>(Domain::ErrorCodes::InvalidRequest, "Policy retrieval is unavailable in this composition.");
            auto inspected = dependencies_.projectPolicy->execute({authority.projectId(), Contracts::ProjectPolicyAction::Inspect}, context);
            if (!inspected) return propagate<Json>(std::move(inspected));
            auto index = Json::parse(inspected.value());
            const auto path = arguments.value("path", "");
            if (!path.empty() && arguments.contains("cursor"))
                return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                    "Policy index cursors cannot be used with a document path.");
            if (path.empty() || !index.value("active", false)) {
                auto window = policyIndexWindow(index, arguments, authority.projectId(), dependencies_.hasher);
                if (!window) return propagate<Json>(std::move(window));
                return Domain::Result<Json>::success(std::move(index));
            }
            const auto offset = arguments.value("offset", std::size_t{});
            auto document = dependencies_.projectPolicy->execute({authority.projectId(), Contracts::ProjectPolicyAction::ReadDocument,
                path, index.at("revision").get<std::string>(), Json{{"offset", offset}}.dump()}, context);
            if (!document) return propagate<Json>(std::move(document));
            return Domain::Result<Json>::success(Json::parse(document.value()));
        }
        if (name.starts_with("clu.")) {
            return cluGovernance(name, authority, arguments, context);
        }
        if (name == "get_forge_status" || name == "forge_status" || name.starts_with("agent_")) {
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
                name, authority, arguments, context, observation, fileReadByteStart);
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
        if (name == "shell_exec" || name.starts_with("shell_job_")) {
            return shell(name, authority, arguments, context, observation);
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

    [[nodiscard]] Domain::Result<Json> cluGovernance(
        const std::string_view name,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context) const
    {
        if (!dependencies_.projectPolicy) {
            return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "CLU governance is unavailable in this composition.");
        }
        auto inspect = dependencies_.projectPolicy->execute(
            {authority.projectId(), Contracts::ProjectPolicyAction::Inspect}, context);
        if (!inspect) return propagate<Json>(std::move(inspect));
        const auto status = Json::parse(inspect.value());
        if (!status.value("active", false)) {
            if (name == "clu.findings") {
                return Domain::Result<Json>::success(Json{{"active", false},
                    {"revision", ""}, {"findings", Json::array()},
                    {"notifications", Json::array()}});
            }
            if (name == "clu.export_log") {
                return Domain::Result<Json>::success(Json{{"active", false},
                    {"revision", ""}, {"history", Json::array()},
                    {"findings", Json::array()},
                    {"notifications", Json::array()}});
            }
            if (name == "clu.evaluate") {
                return Domain::Result<Json>::success(Json{{"active", false},
                    {"evaluated", false}, {"finding_id", nullptr},
                    {"reason", "No development policy is bound to this project."}});
            }
        }
        const auto revision = status.value("revision", std::string{});
        Contracts::ProjectPolicyAction action{};
        std::string details;
        if (name == "clu.findings") {
            action = Contracts::ProjectPolicyAction::ListFindings;
        } else if (name == "clu.export_log") {
            action = Contracts::ProjectPolicyAction::ExportLog;
        } else if (name == "clu.evaluate") {
            action = Contracts::ProjectPolicyAction::Evaluate;
            details = arguments.at("evidence").dump();
        } else if (name == "clu.resolve") {
            action = Contracts::ProjectPolicyAction::Resolve;
            details = Json{{"finding_id", arguments.at("finding_id")},
                {"correction_evidence", arguments.at("correction_evidence")}}.dump();
        } else {
            return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "The requested CLU governance operation is not registered.");
        }
        auto result = dependencies_.projectPolicy->execute(
            {authority.projectId(), action, {}, revision, std::move(details)}, context);
        if (!result) return propagate<Json>(std::move(result));
        return Domain::Result<Json>::success(Json::parse(result.value()));
    }

    [[nodiscard]] Domain::Result<Json> agents(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "get_forge_status" || name == "forge_status") {
            std::optional<Domain::PathText> preferredRoot;
            if (dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host) {
                auto root = dependencies_.workspaceAuthority.defaultWorkspacePath(authority, context);
                if (!root) return propagate<Json>(std::move(root));
                preferredRoot = std::move(root).value();
            } else if (!authority.trustedRoots().empty()) preferredRoot = authority.trustedRoots().front();
            auto projectContext = workspaceContext(
                authority.projectId(), preferredRoot, context);
            if (!projectContext) {
                return propagate<Json>(std::move(projectContext));
            }
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
                         {"note", summary.automatic.note}}}};
            }

            const auto automatic =
                dependencies_.continuityAutomationStatus.snapshot(
                    call.clientId());
            Json implicitRoots = Json::array();
            for (const auto& root : automatic.implicitRoots) {
                implicitRoots.push_back(root.value());
            }
            auto resume = projectHandoff(authority.projectId(), context);
            if (!resume) {
                return propagate<Json>(std::move(resume));
            }
            auto visibleChat = Json::object();
            if (dependencies_.visibleChatRemoteStatus || dependencies_.visibleChatContinuityStatus) {
                try {
                    auto remote = dependencies_.visibleChatRemoteStatus
                        ? dependencies_.visibleChatRemoteStatus(authority.projectId(), context)
                        : Domain::Result<std::string>::success(dependencies_.visibleChatContinuityStatus());
                    if (!remote) throw std::runtime_error{remote.error().message};
                    const auto observed = Json::parse(remote.value(), nullptr, false);
                    if (observed.is_object() &&
                        (!observed.contains("available") || observed["available"].is_boolean()) &&
                        (!observed.contains("enabled") || observed["enabled"].is_boolean())) visibleChat = observed;
                    else visibleChat["error"] = "The optional visible-chat observation was malformed.";
                } catch (...) {
                    visibleChat["error"] = "The optional visible-chat observation failed.";
                }
            }
            Json configuredRoots = Json::array();
            auto configured = dependencies_.workspaceAuthority.configuredRootAllowlist(context);
            if (!configured) return propagate<Json>(std::move(configured));
            for (const auto& root : configured.value()) configuredRoots.push_back(root.value());
            Json activeRoots = Json::array();
            for (const auto& root : authority.trustedRoots()) activeRoots.push_back(root.value());
            const auto packetId = resume.value().record
                ? std::optional<std::string>{resume.value().record->packet.id.value()} : std::nullopt;
            Json automaticStatus{
                {"resume_packet_ready", packetId.has_value()},
                {"resume_packet_id", packetId ? Json(*packetId) : Json(nullptr)},
                {"readback_confirmed", packetId && automatic.readbackHandoffId == packetId},
                {"readback_handoff_id", automatic.readbackHandoffId ? Json(*automatic.readbackHandoffId) : Json(nullptr)},
                {"readback_scope", "this_mcp_client_confirmed_context_get"},
                {"packet_transport", "mcp_initialize_and_context_get"},
                {"project_handoff", resume.value().record
                    ? legacyPacketJson(resume.value().record->packet) : Json(nullptr)},
                {"enabled", visibleChat.value("enabled", automatic.enabled)},
                {"blocked", automatic.blocked},
                {"handoff_pending", automatic.handoffPending},
                {"visible_chat_handoff_available", visibleChat.value("available", false)},
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
                {"home_kind", "application_data"},
                {"home_is_project", false},
                {"durable_manager", durableManagerStatus()},
                {"client_id", call.clientId().value()},
                {"agent_count", agents.size()},
                {"tool_count", tools.size()},
                {"shell_execution", Json{{"enabled", authority.shellEnabled()},
                    {"maximum_command_bytes", 65'536U},
                    {"synchronous_timeout_sec_max", 120},
                    {"synchronous_default_timeout_sec", dependencies_.shellDefaultTimeout.count()},
                    {"detached_processes_survive", false},
                    {"jobs_available", dependencies_.shell.supportsJobs()},
                    {"job_timeout_sec_max", 3600}, {"job_default_timeout_sec", 1800},
                    {"maximum_active_jobs", 2}, {"maximum_retained_jobs", 16}, {"maximum_persisted_jobs_per_project", 32},
                    {"maximum_log_bytes_per_stream", 16U * 1024U * 1024U}, {"log_read_page_bytes", 32U * 1024U},
                    {"job_lifetime", dependencies_.durableToolBroker || dependencies_.reviewerRuns ? "manager_process" : "connector_process"},
                    {"job_output", "durable_named_stream_logs_and_bounded_final_capture"},
                    {"durable_across_mcp_reconnect", static_cast<bool>(dependencies_.durableToolBroker || dependencies_.reviewerRuns)},
                    {"environment_policy", "allowlisted_system_and_user_profile_with_explicit_overrides"},
                    {"python_utf8_default", true}}},
                {"agents", std::move(agents)},
                {"tools", std::move(tools)},
                {"memory_note_count", memoryCount},
                {"presence_count", status.value().presenceCount},
                {"open_sessions", openIds.size()},
                {"open_session_ids", std::move(openIds)},
                {"continuity", std::move(continuityStatus)},
                {"auto_continuity", std::move(automaticStatus)},
                {"visible_chat_continuity", visibleChat},
                {"reviewer_execution", Json{{"manager_required", true},
                    {"read_only_tools_enforced", true}, {"executor_history_included", false},
                    {"receive_timeout_sec_default", Domain::DefaultReviewerReceiveTimeoutSeconds},
                    {"receive_timeout_sec_max", Domain::MaximumManagedProviderReceiveTimeoutSeconds},
                    {"maximum_opening_message_bytes", Domain::MaximumReviewerOpeningMessageBytes},
                    {"opening_message_sources", Json::array({"authorized_file", "inline"})},
                    {"modes", Json::array({"tools", "text_only"})},
                    {"deadline_exceeded_disposition", "infrastructure_blocked"},
                    {"maximum_persisted_reviews", nullptr}, {"maximum_concurrent_reviews", 16},
                    {"maximum_output_bytes", Domain::MaximumManagedRunOutputBytes},
                    {"retention_policy", "sealed_receipts_retained_by_run_id_subject_to_available_disk_space"}}},
                {"context_telemetry", visibleChat.contains("context_telemetry")
                    ? visibleChat.at("context_telemetry") : Json{{"tokens_used", nullptr},
                        {"reason", "No completed provider generation observation is available."}}},
                {"workspace_authority", Json{{"configured_additional_roots", std::move(configuredRoots)},
                    {"active_roots", std::move(activeRoots)}, {"bind_tool", "workspace_authority_bind"},
                    {"cwd_policy", "shell_exec_shell_job_start_and_process_launch_require_locally_active_execute_authority"},
                    {"filesystem_access", dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host ? "host" : "workspace"},
                    {"recovered_workspace_policy", dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host
                        ? "current_owner_issued_local_volumes_with_project_identity_preserved"
                        : "selected_project_alias_plus_explicitly_bound_owner_configured_roots"},
                    {"configured_roots_active_by_default", false},
                    {"local_volume_roots_active_by_default", dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host},
                    {"file_write_limit_bytes", 2U * 1024U * 1024U}}},
                {"workspace", std::move(projectContext.value().at("workspace"))},
                {"instruction_packages",
                 std::move(projectContext.value().at("instruction_packages"))},
                {"development_policy",
                 std::move(projectContext.value().at("development_policy"))},
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
        ToolContinuityObservationBuilder& observation,
        std::optional<std::size_t>& fileReadByteStart)
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
        const auto encodedPath = suppliedPath.value_or(observation.defaultDirectory());
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
            fileReadByteStart = first < lines.size()
                ? static_cast<std::size_t>(lines[first].data() - content.data()) : content.size();
            const auto selectedLineCount = bytePage ? (selected.empty() ? 0U : 1U) : last - first;
            return Domain::Result<Json>::success(Json{
                {"ok", true},
                {"path", authorized.value().canonicalPath().value()},
                {"content", std::move(selected)},
                {"size", bytes.value().size()},
                {"total_lines", lines.size()},
                {"start_line", startLineSigned},
                {"end_line", endLine},
                {"line_count", selectedLineCount},
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
            observation.defaultDirectory());
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
            observation.defaultDirectory());
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


    [[nodiscard]] Domain::Result<Domain::PathText> cmakeTestDirectory(
        const Contracts::WorkspaceAuthority& authority, const Json& arguments,
        const Domain::OperationContext& context, ToolContinuityObservationBuilder& observation)
    {
        if (!authority.shellEnabled()) return failure<Domain::PathText>(Domain::ErrorCodes::ShellDisabled,
            "CMake/CTest execution requires enabled shell policy.");
        if (arguments.contains("target") && arguments.value("mode", std::string{"test"}) != "build_and_test")
            return failure<Domain::PathText>(Domain::ErrorCodes::InvalidRequest,
                "A build target requires build_and_test mode.");
        std::optional<Domain::PathText> directory;
        for (const auto access : {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Execute}) {
            auto authorized = authorizePath(dependencies_.workspaceAuthority, authority,
                directory ? directory->value() : arguments.at("build_dir").get<std::string>(),
                access, false, context, &observation, ContinuityPathRole::WorkingDirectory);
            if (!authorized) return propagate<Domain::PathText>(std::move(authorized));
            directory = authorized.value().canonicalPath();
        }
        return Domain::Result<Domain::PathText>::success(*directory);
    }

    [[nodiscard]] Domain::Result<Json> cmakeTest(
        const std::string_view name, const Contracts::WorkspaceAuthority& authority,
        const Json& arguments, const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "cmake_test_run") {
            auto directory = cmakeTestDirectory(authority, arguments, context, observation);
            if (!directory) return propagate<Json>(std::move(directory));
            Domain::CMakeTestRequest request{directory.value()};
            const auto mode = arguments.value("mode", std::string{"test"});
            if (mode != "test" && mode != "build_and_test") return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "CMake/CTest mode must be test or build_and_test.");
            request.build = mode == "build_and_test";
            if (arguments.contains("target")) request.target = arguments.at("target").get<std::string>();
            if (arguments.contains("filter")) request.filter = arguments.at("filter").get<std::string>();
            if (arguments.contains("config")) request.configuration = arguments.at("config").get<std::string>();
            if (!request.build && request.target) return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "A build target requires build_and_test mode.");
            request.timeout = std::chrono::seconds{arguments.value("timeout_sec", 1800U)};
            auto started = dependencies_.shell.startCMakeTestRun(request, authority, context);
            if (!started) return propagate<Json>(std::move(started));
            auto value = shellJobJson(started.value(), false);
            value["lifetime"] = dependencies_.reviewerRuns ? "manager_process" : "connector_process";
            value["durable_across_mcp_reconnect"] = static_cast<bool>(dependencies_.reviewerRuns);
            return Domain::Result<Json>::success(std::move(value));
        }
        auto status = dependencies_.shell.getCMakeTestRun(arguments.at("job_id").get<std::string>(),
            arguments.value("failure_offset", std::uint64_t{}), arguments.value("max_failures", std::size_t{16}),
            authority, context);
        if (!status) return propagate<Json>(std::move(status));
        auto value = shellJobJson(status.value().job, false);
        Json failures = Json::array();
        for (const auto& item : status.value().failures) failures.push_back(Json{
            {"name", item.name}, {"status", item.status}, {"message", item.message},
            {"output", item.output}, {"output_truncated", item.outputTruncated}});
        value["failures"] = std::move(failures);
        value["failure_offset"] = status.value().failureOffset;
        value["next_failure_offset"] = status.value().nextFailureOffset;
        value["total_failures"] = status.value().totalFailures;
        value["has_more"] = status.value().hasMore;
        return Domain::Result<Json>::success(std::move(value));
    }

    [[nodiscard]] Domain::Result<Json> process(
        const std::string_view name, const Contracts::WorkspaceAuthority& authority,
        const Json& arguments, const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "process_launch") {
            auto executable = Domain::PathText::create(arguments.at("command").get<std::string>());
            if (!executable) return propagate<Json>(std::move(executable));
            auto cwd = authorizePath(dependencies_.workspaceAuthority, authority,
                arguments.value("cwd", observation.defaultDirectory()), Domain::FileAccess::Execute,
                false, context, &observation, ContinuityPathRole::WorkingDirectory);
            if (!cwd) return propagate<Json>(std::move(cwd));
            auto timeout = arguments.value("timeout_sec", 1800.0);
            if (!std::isfinite(timeout) || timeout <= 0 || timeout > 3600)
                return failure<Json>(Domain::ErrorCodes::InvalidRequest, "Job timeout must be within 0...3600 seconds.");
            Domain::ProcessRequest request{executable.value(), {}, cwd.value().canonicalPath(), {}, false,
                std::chrono::milliseconds{static_cast<std::int64_t>(std::ceil(timeout * 1000))},
                MaximumShellOutputBytes, MaximumShellErrorBytes};
            if (arguments.contains("args")) request.arguments = arguments.at("args").get<std::vector<std::string>>();
            if (arguments.contains("env")) for (const auto& item : arguments.at("env").items())
                request.environment.push_back({item.key(), item.value().get<std::string>()});
            auto job = dependencies_.shell.startProcess(request, authority, context);
            if (!job) return propagate<Json>(std::move(job));
            auto result = shellJobJson(job.value());
            result["lifetime"] = dependencies_.reviewerRuns ? "manager_process" : "connector_process";
            result["durable_across_mcp_reconnect"] = static_cast<bool>(dependencies_.reviewerRuns);
            return Domain::Result<Json>::success(std::move(result));
        }
        if (name == "process_list") {
            auto jobs = dependencies_.shell.listJobs(authority, context);
            if (!jobs) return propagate<Json>(std::move(jobs));
            Json values = Json::array();
            for (const auto& job : jobs.value()) values.push_back(shellJobJson(job, false));
            return Domain::Result<Json>::success(Json{{"ok", true}, {"jobs", std::move(values)}});
        }
        const auto id = arguments.at("job_id").get<std::string>();
        if (name == "process_read_log") {
            if (arguments.contains("offset") && arguments.contains("tail_lines"))
                return failure<Json>(Domain::ErrorCodes::InvalidRequest, "Select offset or tail_lines, not both.");
            std::optional<std::uint64_t> offset;
            if (arguments.contains("offset")) offset = arguments.at("offset").get<std::uint64_t>();
            auto page = dependencies_.shell.readJobLog(id, arguments.value("stream", "stdout") == "stderr",
                offset, arguments.value("tail_lines", std::size_t{100}), authority, context);
            if (!page) return propagate<Json>(std::move(page));
            return Domain::Result<Json>::success(Json{{"ok", true}, {"job_id", id}, {"text", page.value().text},
                {"path", page.value().path}, {"offset", page.value().offset},
                {"next_offset", page.value().nextOffset}, {"bytes", page.value().totalBytes},
                {"has_more", page.value().hasMore}, {"text_lossy", page.value().textLossy}});
        }
        auto job = name == "process_kill" ? dependencies_.shell.cancelJob(id, authority, context)
            : name == "process_adopt" ? dependencies_.shell.adoptJob(id, authority, context)
            : dependencies_.shell.getJob(id, authority, context);
        if (!job) return propagate<Json>(std::move(job));
        if (name == "process_wait") {
            const auto requested = arguments.value("timeout_sec", 30U);
            const auto deadline = (std::min)(context.deadline,
                dependencies_.clock.monotonicNow() + std::chrono::seconds{(std::min)(requested, 30U)});
            while (job.value().state == Domain::ShellJobState::Running &&
                dependencies_.clock.monotonicNow() < deadline && !context.isCancellationRequested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                job = dependencies_.shell.getJob(id, authority, context);
                if (!job) return propagate<Json>(std::move(job));
            }
            if (context.isCancellationRequested()) return failure<Json>(Domain::ErrorCodes::Cancelled, "The wait was cancelled; the job remains owned by its broker.");
        }
        auto result = shellJobJson(job.value());
        result["adopted"] = name == "process_adopt";
        return Domain::Result<Json>::success(std::move(result));
    }

    [[nodiscard]] Domain::Result<Json> reviewer(
        const std::string_view name, const Contracts::WorkspaceAuthority& authority,
        const Json& arguments, const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        auto* service = dependencies_.reviewerRuns ? dependencies_.reviewerRuns() : nullptr;
        if (!service) return failure<Json>(Domain::ErrorCodes::HostCapabilityUnavailable,
            "Independent review requires the authenticated persistent Manager; start Forge Manager and reconnect.");
        auto result = [&]() -> Domain::Result<Domain::ManagedRunSnapshot> {
            if (name == "reviewer_start") {
                if (arguments.contains("opening_message_path") == arguments.contains("opening_message"))
                    return failure<Domain::ManagedRunSnapshot>(Domain::ErrorCodes::InvalidRequest,
                        "Provide exactly one of opening_message_path or opening_message.");
                std::string text;
                std::string source{"inline tool argument"};
                if (arguments.contains("opening_message_path")) {
                    auto openingPath = authorizePath(dependencies_.workspaceAuthority, authority,
                        arguments.at("opening_message_path").get<std::string>(), Domain::FileAccess::Read,
                        false, context, &observation, ContinuityPathRole::Path);
                    if (!openingPath) return propagate<Domain::ManagedRunSnapshot>(std::move(openingPath));
                    auto opening = dependencies_.fileSystem.readFile(openingPath.value(), Domain::MaximumReviewerOpeningMessageBytes, context);
                    if (!opening) return propagate<Domain::ManagedRunSnapshot>(std::move(opening));
                    text.assign(reinterpret_cast<const char*>(opening.value().data()), opening.value().size());
                    source = openingPath.value().canonicalPath().value();
                } else {
                    text = arguments.at("opening_message").get<std::string>();
                }
                if (text.empty() || text.size() > Domain::MaximumReviewerOpeningMessageBytes)
                    return failure<Domain::ManagedRunSnapshot>(Domain::ErrorCodes::LimitExceeded,
                        "Reviewer opening message must be nonempty and at most 64 KiB.");
                if (text.find('\0') != std::string::npos || !Domain::isValidUtf8(text))
                    return failure<Domain::ManagedRunSnapshot>(Domain::ErrorCodes::InvalidRequest,
                        "Reviewer opening message must contain valid UTF-8 without NUL bytes.");
                const auto mode = arguments.value("mode", std::string{"tools"});
                if (mode != "tools" && mode != "text_only")
                    return failure<Domain::ManagedRunSnapshot>(Domain::ErrorCodes::InvalidRequest,
                        "Reviewer mode must be tools or text_only.");
                auto uuid = dependencies_.uuidGenerator.next();
                if (!uuid) return propagate<Domain::ManagedRunSnapshot>(std::move(uuid));
                Domain::ManagedRunStartRequest request{Domain::SessionId{uuid.value()}, authority.projectId(),
                    authority.callerId(), context.operationId, context.correlationId, authority.generation(),
                    "Independent read-only reviewer. Use only read tools; report findings and unresolved gates honestly. "
                    "You have no executor conversation history. Authorization reference: " + arguments.at("authorization").get<std::string>() +
                    (mode == "text_only" ? "\nText-only review: no tools are available. Review only supplied text; identify any missing external evidence as unverified." : "") +
                    "\nOpening message source: " + source + "\n" + text +
                    "\nReview task: " + arguments.value("task", std::string{}), mode == "tools", false, true,
                    arguments.value("receive_timeout_sec", Domain::DefaultReviewerReceiveTimeoutSeconds)};
                return service->start(request, context);
            }
            auto id = Domain::SessionId::parse(arguments.at("run_id").get<std::string>());
            if (!id) return propagate<Domain::ManagedRunSnapshot>(std::move(id));
            auto existing = service->status(id.value(), context);
            if (!existing) return existing;
            if (existing.value().record.projectId != authority.projectId() || !existing.value().record.readOnlyTools)
                return failure<Domain::ManagedRunSnapshot>(Domain::ErrorCodes::ProjectScopeMismatch,
                    "The requested run is not a read-only reviewer owned by this project.");
            return name == "reviewer_cancel" ? service->cancel(id.value(), context) : std::move(existing);
        }();
        if (!result) return propagate<Json>(std::move(result));
        const auto& record = result.value().record;
        constexpr std::size_t maximumPageBytes = 32U * 1024U;
        const std::string_view output = record.outputText ? std::string_view{*record.outputText} : std::string_view{};
        const auto offset = name == "reviewer_status" ? arguments.value("output_offset", std::size_t{}) : 0U;
        const auto maximumBytes = name == "reviewer_status" ? arguments.value("max_output_bytes", maximumPageBytes) : maximumPageBytes;
        if (offset > output.size() || !isUtf8Boundary(output, offset))
            return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                "output_offset must identify a UTF-8 character boundary within the retained reviewer output.");
        const auto end = boundedUtf8End(output, offset, maximumBytes);
        const bool hasMore = end < output.size();
        Json error = nullptr;
        if (record.lastError) {
            const auto& message = record.lastError->message;
            const auto messageEnd = boundedUtf8End(message, 0U, 4U * 1024U);
            error = Json{{"code", record.lastError->code}, {"message", message.substr(0U, messageEnd)},
                {"retryable", record.lastError->retryable},
                {"message_total_bytes", message.size()}, {"message_truncated", messageEnd < message.size()}};
        }
        const auto state = record.state == Domain::ManagedRunState::Completed ? "completed"
            : record.state == Domain::ManagedRunState::Failed ? "failed"
            : record.state == Domain::ManagedRunState::Cancelled ? "cancelled"
            : record.state == Domain::ManagedRunState::Paused ? "paused"
            : record.state == Domain::ManagedRunState::Cancelling ? "cancelling" : "running";
        const bool infrastructureBlocked = record.state == Domain::ManagedRunState::Failed && record.lastError &&
            (record.lastError->code == Domain::ErrorCodes::DeadlineExceeded ||
             record.lastError->code == Domain::ErrorCodes::TransportClosed ||
             record.lastError->code == Domain::ErrorCodes::HostCapabilityUnavailable);
        const auto evidenceIntegrity = [&]() -> std::string_view {
            switch (record.evidenceIntegrity) {
            case Domain::ManagedRunEvidenceIntegrity::NotTerminal: return "not_terminal";
            case Domain::ManagedRunEvidenceIntegrity::LegacyUnsealed: return "legacy_unsealed";
            case Domain::ManagedRunEvidenceIntegrity::Verified: return "verified";
            case Domain::ManagedRunEvidenceIntegrity::Mismatch: return "mismatch";
            }
            return "mismatch";
        }();
        return Domain::Result<Json>::success(Json{{"ok", true}, {"run_id", record.runId.value()},
            {"project_id", record.projectId.value()}, {"state", state}, {"read_only", record.readOnlyTools},
            {"fresh_provider_context", true}, {"executor_history_included", false}, {"manager_owned", true},
            {"authorization_source", "project_scoped_tool_invocation_and_authenticated_same_user_manager"},
            {"authorization_reference_is_human_proof", false},
            {"provider_response_id", record.providerResponseId ? Json(record.providerResponseId->value()) : Json(nullptr)},
            {"input_tokens", record.inputTokens}, {"output_tokens", record.outputTokens},
            {"evidence_sha256", record.evidenceSeal ? Json(record.evidenceSeal->value()) : Json(nullptr)},
            {"evidence_integrity", evidenceIntegrity},
            {"receive_timeout_sec", record.providerReceiveTimeoutSeconds ? Json(*record.providerReceiveTimeoutSeconds) : Json(nullptr)},
            {"mode", record.allowTools ? "tools" : "text_only"},
            {"infrastructure_blocked", infrastructureBlocked},
            {"failure_category", infrastructureBlocked ? "infrastructure" : record.lastError ? "run_error" : "none"},
            {"output", record.outputText ? Json(output.substr(offset, end - offset)) : Json(nullptr)},
            {"output_offset", offset}, {"output_bytes_returned", end - offset},
            {"output_total_bytes", output.size()}, {"output_has_more", hasMore},
            {"next_output_offset", hasMore ? Json(end) : Json(nullptr)},
            {"output_page_truncated", offset != 0U || hasMore},
            {"output_truncated", record.outputTruncated},
            {"gate_approved", false},
            {"error", std::move(error)},
            {"review_disposition", "Inspect the actual reviewer output and resolve findings; this tool does not approve a policy gate."}});
    }

    [[nodiscard]] Domain::Result<Json> verificationEnvironment(
        const std::string_view name, const Contracts::WorkspaceAuthority& authority,
        const Json& arguments, const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        auto path = arguments.at("path").get<std::string>();
        while (path.size() > 3 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
        if (name == "verification_env_status") {
            auto readable = authorizePath(dependencies_.workspaceAuthority, authority,
                path + "/verification-env.json", Domain::FileAccess::Read, false, context,
                &observation, ContinuityPathRole::Path);
            if (!readable) return propagate<Json>(std::move(readable));
            auto bytes = dependencies_.fileSystem.readFile(readable.value(), 64U * 1024U, context);
            if (!bytes) return propagate<Json>(std::move(bytes));
            auto manifest = Json::parse(std::string{reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()});
            if (!manifest.is_object() || !manifest.value("ready", false))
                return failure<Json>(Domain::ErrorCodes::IntegrityFailure, "The verification environment has no successful manifest.");
            return Domain::Result<Json>::success(Json{{"ok", true}, {"path", path}, {"manifest", std::move(manifest)},
                {"manifest_path", readable.value().canonicalPath().value()}, {"provenance", "durable_creation_manifest"}});
        }
        const bool hostAccess = dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host;
        std::optional<Domain::PathText> excludedSubtree;
        if (hostAccess) {
            auto source = dependencies_.workspaceAuthority.defaultWorkspacePath(authority, context);
            if (!source) return propagate<Json>(std::move(source));
            excludedSubtree = std::move(source).value();
        }
        auto destination = authorizePath(dependencies_.workspaceAuthority, authority, path,
            Domain::FileAccess::Create, false, context, &observation, ContinuityPathRole::Path, std::move(excludedSubtree));
        if (!destination) return propagate<Json>(std::move(destination));
        // Dependency writes belong to an explicitly bound evidence root, away
        // from the main source tree. The venv itself remains a normal user venv.
        if (!hostAccess && destination.value().authorityRoot().value() == observation.defaultDirectory())
            return failure<Json>(Domain::ErrorCodes::Unauthorized,
                "Create verification dependencies below an owner-configured additional root, outside the source tree.");
        auto executable = Domain::PathText::create(arguments.at("python_path").get<std::string>());
        if (!executable) return propagate<Json>(std::move(executable));
        std::vector<std::string> requirements = arguments.value("requirements",
            std::vector<std::string>{"jsonschema==4.25.1", "PyYAML==6.0.3"});
        if (requirements.empty() || requirements.size() > 2 ||
            std::any_of(requirements.begin(), requirements.end(), [](const auto& value) {
                return value != "jsonschema==4.25.1" && value != "PyYAML==6.0.3";
            })) return failure<Json>(Domain::ErrorCodes::InvalidRequest, "Only the qualified exact jsonschema and PyYAML pins are supported.");
        const std::string script = R"PY(import sys, pathlib, venv, subprocess, json, datetime
p=pathlib.Path(sys.argv[1]); req=json.loads(sys.argv[2])
if p.exists(): raise RuntimeError('Destination already exists; refusing to alter it')
venv.EnvBuilder(with_pip=True).create(p)
py=p/'Scripts'/'python.exe'
subprocess.run([str(py),'-I','-m','pip','install','--disable-pip-version-check','--no-input','--only-binary=:all:',*req],check=True)
probe=subprocess.run([str(py),'-I','-c',"import sys,json,importlib.metadata as m; print(json.dumps({'python_version':sys.version,'executable':sys.executable,'distributions':{d.metadata['Name']:d.version for d in m.distributions()}}))"],check=True,capture_output=True,text=True,encoding='utf-8')
facts=json.loads(probe.stdout)
versions={k.lower():v for k,v in facts['distributions'].items()}
for pin in req:
 name,ver=pin.split('==')
 if versions.get(name.lower())!=ver: raise RuntimeError('Installed pin mismatch: '+pin)
facts.update({'ready':True,'requirements':req,'created_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'isolation':'dedicated_venv','source_scope':'dependency writes outside primary project root'})
with (p/'verification-env.json').open('x',encoding='utf-8') as f: json.dump(facts,f,indent=2)
print(json.dumps(facts))
)PY";
        Domain::ProcessRequest request{executable.value(), {"-I", "-c", script, destination.value().canonicalPath().value(), Json(requirements).dump()},
            destination.value().authorityRoot(), {}, false, std::chrono::seconds{1800}, MaximumShellOutputBytes, MaximumShellErrorBytes};
        auto job = dependencies_.shell.startProcess(request, authority, context);
        if (!job) return propagate<Json>(std::move(job));
        auto result = shellJobJson(job.value());
        result["environment_path"] = destination.value().canonicalPath().value();
        result["manifest_path"] = path + "/verification-env.json";
        result["next_action"] = "Wait for this job and require successful exit; then call verification_env_status and evidence_digest on its manifest.";
        return Domain::Result<Json>::success(std::move(result));
    }

    [[nodiscard]] Domain::Result<Json> shell(
        const std::string_view name,
        const Contracts::WorkspaceAuthority& authority,
        const Json& arguments,
        const Domain::OperationContext& context,
        ToolContinuityObservationBuilder& observation)
    {
        if (name == "shell_job_list") {
            auto jobs = dependencies_.shell.listJobs(authority, context);
            if (!jobs) return propagate<Json>(std::move(jobs));
            Json values = Json::array();
            for (const auto& job : jobs.value()) values.push_back(shellJobJson(job, false));
            return Domain::Result<Json>::success(Json{{"ok", true}, {"jobs", std::move(values)},
                {"lifetime", dependencies_.reviewerRuns ? "manager_process" : "connector_process"},
                {"pid", dependencies_.processId}});
        }
        if (name == "shell_job_status" || name == "shell_job_cancel") {
            const auto id = strictString(arguments, "job_id").value_or("");
            auto job = name == "shell_job_cancel"
                ? dependencies_.shell.cancelJob(id, authority, context)
                : dependencies_.shell.getJob(id, authority, context);
            if (!job) return propagate<Json>(std::move(job));
            auto value = shellJobJson(job.value());
            return Domain::Result<Json>::success(std::move(value));
        }
        const auto command = legacyString(arguments, "command").value_or("");
        if (command.empty()) {
            return failure<Json>(
                "missing_command",
                "command required");
        }
        const auto cwdText = legacyString(arguments, "cwd").value_or(
            observation.defaultDirectory());
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
        const bool trackedJob = name == "shell_job_start";
        auto timeoutSeconds = strictNumber(arguments, "timeout_sec").value_or(
            trackedJob ? 1800.0 : static_cast<double>(dependencies_.shellDefaultTimeout.count()));
        if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0.0) {
            return failure<Json>(
                "invalid_timeout",
                "timeout_sec must be finite and positive");
        }
        timeoutSeconds = (std::min)(timeoutSeconds, trackedJob ? 3600.0 : 120.0);
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
        if (trackedJob) {
            auto job = dependencies_.shell.startJob(request, authority, context);
            if (!job) return propagate<Json>(std::move(job));
            auto value = shellJobJson(job.value());
            value["lifetime"] = dependencies_.reviewerRuns ? "manager_process" : "connector_process";
            value["next_action"] = "Poll shell_job_status or process_wait until done=true; inspect result.ok and exit_code. Manager-owned jobs survive MCP reconnect; shutting down their owning process cancels them.";
            return Domain::Result<Json>::success(std::move(value));
        }
        auto result = dependencies_.shell.execute(request, authority, context);
        if (!result) {
            return propagate<Json>(std::move(result));
        }
        auto payload = processJson(
            result.value(), cwd.value().canonicalPath().value());
        payload["command"] = command;
        payload["timeout_sec"] = timeoutSeconds;
        payload["process_tree_lifetime"] = "until_command_exit_or_timeout";
        if (result.value().timedOut) {
            payload["next_action"] = "For longer commands use shell_job_start and poll shell_job_status. Keep the command in the foreground; detached descendants are terminated when their parent shell exits.";
        }
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
            context);
    }

    [[nodiscard]] Domain::Result<Json> projectMemoryMutation(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::ProjectId& project,
        const Json& arguments,
        const Domain::OperationContext& context)
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
            name, call, authority, project, arguments, context);
    }

    [[nodiscard]] Domain::Result<Json> projectMemoryArtifact(
        const std::string_view name,
        const Contracts::AuthorizedToolCall& call,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::ProjectId& project,
        const Json& arguments,
        const Domain::OperationContext& context)
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
            // Export artifacts live in the application's owned directory, not
            // in an authorized workspace root. The artifact store resolves the
            // candidate against the exact project's immediate exports child
            // and rejects foreign paths and reparse-point escapes on open.
            auto artifact = pathText(artifactText, "artifact");
            if (!artifact) {
                return propagate<Json>(std::move(artifact));
            }
            const auto policy = strictString(arguments, "merge_policy").value_or("");
            std::optional<Domain::Sha256Digest> expectedChecksum;
            if (const auto expected = strictString(arguments, "expected_checksum")) {
                auto parsed = Domain::Sha256Digest::parse(*expected);
                if (!parsed) return propagate<Json>(std::move(parsed));
                expectedChecksum = std::move(parsed).value();
            }
            Domain::ImportProjectMemoryRequest request{
                project,
                std::move(artifact).value(),
                strictBoolean(arguments, "preview").value_or(true),
                policy == "merge",
                std::move(expectedChecksum)};
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

    [[nodiscard]] bool automaticHandoffRequested(const Domain::ProjectId& project, const Domain::OperationContext& context) const noexcept
    {
        if (!dependencies_.visibleChatRemoteStatus && !dependencies_.visibleChatContinuityStatus) {
            return false;
        }
        try {
            auto remote=dependencies_.visibleChatRemoteStatus?dependencies_.visibleChatRemoteStatus(project,context)
                :Domain::Result<std::string>::success(dependencies_.visibleChatContinuityStatus());
            if(!remote) return false;
            const auto status = Json::parse(remote.value(), nullptr, false);
            const auto* state = member(status, "state");
            if (state == nullptr || !state->is_string()) {
                return false;
            }
            const auto& value = state->get_ref<const std::string&>();
            return value == "requesting_model_packet" ||
                value == "waiting_for_model_packet" ||
                value == "repairing_model_packet";
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] Domain::Result<void> validateAutomaticHandoff(
        const Json& arguments) const
    {
        std::vector<std::string> incomplete;
        const auto* goal = member(arguments, "goal");
        if (goal == nullptr || !goal->is_string() ||
            goal->get_ref<const std::string&>().find_first_not_of(" \t\r\n\f\v") ==
                std::string::npos) {
            incomplete.emplace_back("goal (nonempty string)");
        }
        for (const auto key : {"narrative", "resume_seed"}) {
            const auto* value = member(arguments, key);
            if (value == nullptr || !value->is_string() ||
                value->get_ref<const std::string&>().size() < 256U) {
                incomplete.emplace_back(std::string{key} +
                    " (detailed string of at least 256 characters)");
            }
            if (value != nullptr && value->is_string()) {
                auto text = value->get<std::string>();
                std::transform(text.begin(), text.end(), text.begin(),
                    [](const unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                const auto first = text.find_first_not_of(" \t\r\n\f\v");
                if (first == std::string::npos ||
                    std::all_of(text.begin(), text.end(), [character = text[first]](const unsigned char codeUnit) {
                        return std::isspace(codeUnit) != 0 || codeUnit == static_cast<unsigned char>(character);
                    })) {
                    incomplete.emplace_back(std::string{key} +
                        " (actual detailed task state, not repeated-single-character padding)");
                }
                if (hasLeadingFillerMarker(text)) {
                    incomplete.emplace_back(std::string{key} +
                        " (task-specific prose without filler)");
                }
            }
        }
        for (const auto key : {"key_files", "next_actions", "decisions"}) {
            const auto* value = member(arguments, key);
            if (value == nullptr || !value->is_array() || value->empty() ||
                !std::all_of(value->begin(), value->end(), [](const Json& item) {
                    return item.is_string() &&
                        item.get_ref<const std::string&>().find_first_not_of(" \t\r\n\f\v") !=
                            std::string::npos;
                })) {
                incomplete.emplace_back(std::string{key} +
                    (std::string_view{key} == "decisions"
                        ? " (nonempty array of nonblank strings recording all explicit user constraints)"
                        : " (nonempty array of actual path or ordered action strings)"));
            } else if (std::string_view{key} == "next_actions") {
                for (const auto& item : *value) {
                    auto action = item.get<std::string>();
                    const auto first = action.find_first_not_of(" \t\r\n\f\v");
                    const auto last = action.find_last_not_of(" \t\r\n\f\v");
                    action = action.substr(first, last - first + 1U);
                    std::transform(action.begin(), action.end(), action.begin(),
                        [](const unsigned char character) {
                            return static_cast<char>(std::tolower(character));
                        });
                    if (action == "continue" || action == "resume" || action == "next") {
                        incomplete.emplace_back("next_actions (replace continue/resume/next with concrete ordered actions)");
                        break;
                    }
                }
            }
        }
        if (incomplete.empty()) {
            return Domain::Result<void>::success();
        }
        std::string message = "Auto Continuity session_handoff requires these complete packet fields: ";
        for (std::size_t index = 0U; index < incomplete.size(); ++index) {
            if (index != 0U) {
                message += "; ";
            }
            message += incomplete[index];
        }
        message += ". No packet was saved. Resend session_handoff with these fields directly, or in the complete JSON object serialized into the accepted packet_json string; both forms are parsed against the same schema. Collections must be JSON arrays, not prose inside narrative or resume_seed. Do not invent handoff_id; omit it when creating a new packet.";
        return failure<void>(Domain::ErrorCodes::InvalidRequest, std::move(message));
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
            Json writeArguments = arguments;
            if (name == "session_handoff" && arguments.contains("packet_json")) {
                const auto& encoded = arguments.at("packet_json");
                if (!encoded.is_string()) {
                    return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                        "packet_json must be a JSON-string containing a session_handoff argument object.");
                }
                const auto packet = Json::parse(
                    encoded.get_ref<const std::string&>(), nullptr, false);
                if (!packet.is_object() || packet.contains("packet_json")) {
                    return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                        "packet_json must encode one JSON object of direct session_handoff fields; nested packet_json is not supported.");
                }
                writeArguments.erase("packet_json");
                for (auto field = packet.begin(); field != packet.end(); ++field) {
                    const auto outer = writeArguments.find(field.key());
                    if (outer != writeArguments.end() && *outer != field.value()) {
                        return failure<Json>(Domain::ErrorCodes::InvalidRequest,
                            "packet_json conflicts with the outer session_handoff field: " + field.key() + ". No packet was saved.");
                    }
                    writeArguments[field.key()] = field.value();
                }
                auto valid = validateArgumentsAgainstSchema(writeArguments, *descriptor(name));
                if (!valid) {
                    return propagate<Json>(std::move(valid));
                }
            }
            if (name == "session_handoff" && automaticHandoffRequested(authority.projectId(), context)) {
                auto complete = validateAutomaticHandoff(writeArguments);
                if (!complete) {
                    return propagate<Json>(std::move(complete));
                }
            }
            auto patch = legacyPatch(writeArguments);
            if (!patch) {
                return propagate<Json>(std::move(patch));
            }
            std::optional<Domain::LegacyHandoffId> handoffId;
            auto encodedId = legacyString(writeArguments, "handoff_id");
            if (!encodedId) {
                encodedId = legacyString(writeArguments, "id");
            }
            if (encodedId && !encodedId->empty()) {
                auto parsed = parseOpaque<Domain::LegacyHandoffId>(
                    *encodedId, "handoff_id");
                if (!parsed) {
                    return propagate<Json>(std::move(parsed));
                }
                handoffId.emplace(std::move(parsed).value());
            }
            if (!patch.value().workingDirectory && !authority.trustedRoots().empty()) {
                if (dependencies_.workspaceAuthority.fileSystemAccessMode() == Domain::FileSystemAccessMode::Host) {
                    auto root = dependencies_.workspaceAuthority.defaultWorkspacePath(authority, context);
                    if (!root) return propagate<Json>(std::move(root));
                    patch.value().workingDirectory = root.value().value();
                } else patch.value().workingDirectory = authority.trustedRoots().front().value();
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
            auto response = legacyPersistJson(result.value(),
                name == "session_checkpoint" ? "checkpoint" : "handoff");
            if (name == "session_handoff") {
                auto published = dependencies_.legacyMemory.set(
                    {"continuity/project/" + authority.projectId().value(),
                     result.value().record.packet.id.value(), {}}, context);
                if (!published) {
                    return failure<Json>(Domain::ErrorCodes::StoreError,
                        "Packet " + result.value().record.packet.id.value() +
                        " was saved, but publishing it for successor connections failed: " +
                        published.error().message);
                }
            }
            if (name == "session_handoff") {
                response["successor_pickup"] = "mcp_initialize_and_context_get";
                response["predecessor_session_id"] = call.clientId().value();
                response["session_kind"] = "mcp_connection";
                response["message"] = "Packet published for this project. The next Forge "
                    "connection receives it in initialize.instructions; an already connected "
                    "chat retrieves it with context_get. This does not create a visible chat.";
            }
            return Domain::Result<Json>::success(std::move(response));
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
            const auto automation =
                dependencies_.continuityAutomationStatus.snapshot(call.clientId());
            const bool explicitIdRequested = handoffId.has_value();
            if (!handoffId && automation.handoffId &&
                (automation.handoffPending || automation.blocked)) {
                auto parsed = parseOpaque<Domain::LegacyHandoffId>(
                    *automation.handoffId, "active handoff id");
                if (!parsed) return propagate<Json>(std::move(parsed));
                handoffId.emplace(std::move(parsed).value());
            }
            auto result = handoffId
                ? dependencies_.legacyContinuity.get(
                    {*handoffId, strictBoolean(arguments, "resume_ready").value_or(false)}, context)
                : projectHandoff(authority.projectId(), context);
            if (!result) return propagate<Json>(std::move(result));
            if (!handoffId && !result.value().record) {
                result = dependencies_.legacyContinuity.get(
                    {std::nullopt, strictBoolean(arguments, "resume_ready").value_or(false)}, context);
                if (!result) return propagate<Json>(std::move(result));
                if (result.value().record) {
                    // Old packets have no project id. Adopt one implicitly only
                    // when an absolute packet path is authorized for this project.
                    const auto& packet = result.value().record->packet;
                    std::vector<std::string> candidates;
                    if (packet.workingDirectory) candidates.push_back(*packet.workingDirectory);
                    candidates.insert(candidates.end(), packet.keyFiles.begin(), packet.keyFiles.end());
                    bool matchesProject{};
                    for (const auto& candidate : candidates) {
                        if (!isAbsoluteToolPath(candidate)) continue;
                        auto parsed = pathText(candidate, "legacy continuity path");
                        if (!parsed) continue;
                        for (const auto& root : authority.trustedRoots()) {
                            auto authorized = dependencies_.workspaceAuthority.authorize(
                                authority, {parsed.value(), root, Domain::FileAccess::Read, false}, context);
                            if (authorized) {
                                matchesProject = true;
                                break;
                            }
                            if (authorized.error().code != Domain::ErrorCodes::PathOutsideAuthority &&
                                authorized.error().code != Domain::ErrorCodes::Unauthorized &&
                                authorized.error().code != Domain::ErrorCodes::ProjectScopeMismatch &&
                                authorized.error().code != Domain::ErrorCodes::InvalidRequest &&
                                authorized.error().code != Domain::ErrorCodes::RecordNotFound) {
                                return propagate<Json>(std::move(authorized));
                            }
                        }
                        if (matchesProject) break;
                    }
                    if (!matchesProject) result.value().record.reset();
                }
            }
            result.value().explicitIdRequested = explicitIdRequested;
            if (!result.value().record) {
                return Domain::Result<Json>::success(Json{
                    {"ok", true},
                    {"found", false},
                    {"message",
                     result.value().explicitIdRequested
                         ? "No handoff packet found for the requested id."
                         : "No handoff packet for this project. Call session_checkpoint or session_handoff during work; use context_list and an explicit handoff_id to inspect older packets."},
                    {"bootstrap",
                     Json::array({"forge_status", "session_checkpoint when you have a goal"})}});
            }
            if (authority.callerId() != call.clientId()) {
                return failure<Json>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The recovered context caller does not match workspace authority.");
            }
            if (automation.handoffPending &&
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
            auto response = legacyGetJson(*result.value().record, adoption.value());
            response["successor_session_id"] = call.clientId().value();
            response["session_kind"] = "mcp_connection";
            response["predecessor_session_id"] = result.value().record->packet.clientId
                ? Json(result.value().record->packet.clientId->value()) : Json(nullptr);
            return Domain::Result<Json>::success(std::move(response));
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

Domain::Result<std::string> McpToolPackAdapter::bootstrapInstructions(
    const Domain::ProjectId& projectId,
    const Domain::PathText& projectRoot,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<std::string>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP tool-pack adapter is unavailable.");
    }
    return implementation_->bootstrapInstructions(
        projectId, projectRoot, context);
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
