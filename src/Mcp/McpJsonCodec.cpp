#include "ForgeConductor/Mcp/McpJsonCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

using Json = nlohmann::json;

[[nodiscard]] Domain::Error codecError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] bool containsEmbeddedNul(const Json& value)
{
    if (value.is_string()) {
        return value.get_ref<const std::string&>().find('\0') != std::string::npos;
    }
    if (value.is_object()) {
        for (auto item = value.cbegin(); item != value.cend(); ++item) {
            if (item.key().find('\0') != std::string::npos ||
                containsEmbeddedNul(*item)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        return std::any_of(value.begin(), value.end(), [](const auto& item) {
            return containsEmbeddedNul(item);
        });
    }
    return false;
}

} // namespace

Domain::Result<std::string> McpJsonCodec::canonicalize(
    const std::string_view utf8Json) const noexcept
{
    try {
        if (utf8Json.empty()) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::MalformedMessage,
                "JSON input must not be empty."));
        }
        if (utf8Json.size() > MaximumDocumentBytes) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::PayloadTooLarge,
                "JSON input exceeds the MCP document byte limit."));
        }

        bool duplicateKey{};
        bool depthExceeded{};
        std::vector<std::set<std::string, std::less<>>> objectKeys;
        objectKeys.reserve(MaximumNestingDepth + 1U);
        const auto callback = [&objectKeys, &duplicateKey, &depthExceeded](
                                  const int depth,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (depth < 0 || static_cast<std::size_t>(depth) > MaximumNestingDepth) {
                depthExceeded = true;
            }
            switch (event) {
            case Json::parse_event_t::object_start:
                objectKeys.emplace_back();
                break;
            case Json::parse_event_t::key:
                if (objectKeys.empty() ||
                    !objectKeys.back().insert(parsed.get<std::string>()).second) {
                    duplicateKey = true;
                }
                break;
            case Json::parse_event_t::object_end:
                if (!objectKeys.empty()) {
                    objectKeys.pop_back();
                }
                break;
            default:
                break;
            }
            return !depthExceeded;
        };

        const auto document = Json::parse(
            utf8Json.begin(), utf8Json.end(), callback, true, false);
        if (depthExceeded) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::LimitExceeded,
                "JSON input exceeds the MCP nesting-depth limit."));
        }
        if (duplicateKey) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::MalformedMessage,
                "JSON objects must not contain duplicate keys."));
        }
        if (document.is_discarded()) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::MalformedMessage,
                "JSON input could not be decoded."));
        }
        if (!document.is_object()) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::MalformedMessage,
                "MCP JSON input must have an object root."));
        }
        if (containsEmbeddedNul(document)) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::MalformedMessage,
                "MCP JSON input must not contain embedded NUL characters."));
        }
        auto canonical = document.dump();
        if (canonical.size() > MaximumDocumentBytes) {
            return Domain::Result<std::string>::failure(codecError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Canonical JSON exceeds the MCP document byte limit."));
        }
        return Domain::Result<std::string>::success(std::move(canonical));
    } catch (const Json::exception&) {
        return Domain::Result<std::string>::failure(codecError(
            Domain::ErrorCodes::MalformedMessage,
            "JSON input is malformed or contains invalid UTF-8."));
    } catch (...) {
        return Domain::Result<std::string>::failure(codecError(
            Domain::ErrorCodes::InternalFailure,
            "JSON canonicalization failed."));
    }
}

} // namespace ForgeConductor::Mcp
