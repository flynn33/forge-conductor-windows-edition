#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

class ConfigurationCodecException final : public std::runtime_error {
public:
    ConfigurationCodecException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

class DuplicateKeySax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t, const string_t&) override { return value(); }
    bool string(string_t&) override { return value(); }
    bool binary(binary_t&) override { return value(); }

    bool start_object(std::size_t) override
    {
        containers_.push_back(Container{true, {}});
        return true;
    }

    bool key(string_t& name) override
    {
        if (containers_.empty() || !containers_.back().object ||
            !containers_.back().keys.insert(name).second) {
            duplicate_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override
    {
        if (containers_.empty() || !containers_.back().object) {
            return false;
        }
        containers_.pop_back();
        return true;
    }

    bool start_array(std::size_t) override
    {
        containers_.push_back(Container{false, {}});
        return true;
    }

    bool end_array() override
    {
        if (containers_.empty() || containers_.back().object) {
            return false;
        }
        containers_.pop_back();
        return true;
    }

    bool parse_error(
        std::size_t,
        const std::string&,
        const nlohmann::detail::exception&) override
    {
        malformed_ = true;
        return false;
    }

    [[nodiscard]] bool duplicate() const noexcept { return duplicate_; }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    struct Container final {
        bool object{};
        std::unordered_set<std::string> keys;
    };

    [[nodiscard]] bool value() const noexcept { return true; }

    std::vector<Container> containers_;
    bool duplicate_{};
    bool malformed_{};
};

[[noreturn]] void reject(const std::string_view code, const std::string_view message)
{
    throw ConfigurationCodecException{std::string{code}, std::string{message}};
}

void enforceBounds(const Json& value, const std::size_t depth, std::size_t& nodes)
{
    if (depth > LMStudioConfigurationCodec::MaximumJsonDepth) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "LM Studio MCP configuration exceeds the supported JSON depth.");
    }
    ++nodes;
    if (nodes > LMStudioConfigurationCodec::MaximumJsonNodes) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "LM Studio MCP configuration exceeds the supported JSON node count.");
    }
    if (value.is_object() || value.is_array()) {
        for (const auto& child : value) {
            enforceBounds(child, depth + 1U, nodes);
        }
    }
}

[[nodiscard]] Json decode(const std::string& source)
{
    DuplicateKeySax duplicateKeys;
    const bool saxAccepted = Json::sax_parse(source, &duplicateKeys);
    if (duplicateKeys.duplicate()) {
        reject(Domain::ErrorCodes::MalformedMessage,
               "LM Studio MCP configuration contains an ambiguous duplicate JSON key; the existing file was preserved.");
    }
    if (!saxAccepted || duplicateKeys.malformed()) {
        reject(Domain::ErrorCodes::MalformedMessage,
               "LM Studio MCP configuration is malformed JSON; the existing file was preserved.");
    }
    Json document;
    try {
        document = Json::parse(source, nullptr, true, false);
    } catch (const Json::parse_error&) {
        reject(Domain::ErrorCodes::MalformedMessage,
               "LM Studio MCP configuration is malformed JSON; the existing file was preserved.");
    }
    if (!document.is_object()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "LM Studio MCP configuration root must be a JSON object.");
    }
    const auto servers = document.find("mcpServers");
    if (servers != document.end() && !servers->is_object()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "LM Studio MCP configuration mcpServers must be a JSON object.");
    }
    std::size_t nodes{};
    enforceBounds(document, 1U, nodes);
    return document;
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

[[nodiscard]] LMStudioRoleConfigurationStatus inspectRole(
    const Json& servers,
    const Domain::LMStudioConnectorRole role,
    const Domain::PathText& expectedBinary,
    const Domain::PathText& expectedForgeHome)
{
    LMStudioRoleConfigurationStatus status;
    status.role = role;
    const auto entry = servers.find(serverId(role));
    if (entry == servers.end()) {
        status.detail = std::string{"missing "} + serverId(role) + " registration";
        return status;
    }
    status.present = true;
    if (!entry->is_object()) {
        status.detail = std::string{serverId(role)} + " registration is not a JSON object";
        return status;
    }

    const auto command = stringMember(*entry, "command");
    if (!command || *command != expectedBinary.value()) {
        status.detail = std::string{serverId(role)} + " command path does not match the selected binary";
        return status;
    }
    const auto args = entry->find("args");
    if (args == entry->end() || !args->is_array() || args->size() != 1U ||
        !(*args)[0].is_string() || (*args)[0].get<std::string>() != "serve") {
        status.detail = std::string{serverId(role)} + " args must be exactly [\"serve\"]";
        return status;
    }
    const auto environment = entry->find("env");
    if (environment == entry->end() || !environment->is_object()) {
        status.detail = std::string{serverId(role)} + " env must be a JSON object";
        return status;
    }
    const auto configuredRole = stringMember(*environment, "FORGE_MCP_ROLE");
    if (!configuredRole || *configuredRole != roleText(role)) {
        status.detail = std::string{serverId(role)} + " has the wrong FORGE_MCP_ROLE";
        return status;
    }
    const auto configuredHome = stringMember(*environment, "FORGE_CONDUCTOR_HOME");
    if (!configuredHome || *configuredHome != expectedForgeHome.value()) {
        status.detail = std::string{serverId(role)} + " has the wrong FORGE_CONDUCTOR_HOME";
        return status;
    }
    const auto revision = stringMember(*environment, "FORGE_DEPLOYMENT_ID");
    if (!revision || revision->empty()) {
        status.detail = std::string{serverId(role)} + " has no deployment revision";
        return status;
    }
    auto parsedRevision = Domain::DeploymentId::parse(*revision);
    if (!parsedRevision) {
        status.detail = std::string{serverId(role)} + " has an invalid deployment revision";
        return status;
    }
    status.deploymentId.emplace(std::move(parsedRevision).value());
    status.valid = true;
    status.detail = std::string{serverId(role)} + " registration is current";
    return status;
}

[[nodiscard]] std::vector<std::byte> bytesOf(const std::string& value)
{
    std::vector<std::byte> bytes(value.size());
    if (!value.empty()) {
        std::memcpy(bytes.data(), value.data(), value.size());
    }
    return bytes;
}

void mergeRole(
    Json& servers,
    const Domain::LMStudioConnectorRole role,
    const Domain::PathText& binary,
    const Domain::PathText& forgeHome,
    const Domain::DeploymentId& deploymentId)
{
    auto& entry = servers[serverId(role)];
    if (!entry.is_object()) {
        entry = Json::object();
    }
    entry["command"] = binary.value();
    entry["args"] = Json::array({"serve"});
    auto& environment = entry["env"];
    if (!environment.is_object()) {
        environment = Json::object();
    }
    environment["FORGE_MCP_ROLE"] = roleText(role);
    environment["FORGE_CONDUCTOR_HOME"] = forgeHome.value();
    environment["FORGE_DEPLOYMENT_ID"] = deploymentId.value();
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

void removeLegacyForgeLaunchers(
    Json& servers,
    const Domain::PathText& forgeHome)
{
    std::vector<std::string> legacyIds;
    legacyIds.reserve(servers.size());
    for (const auto& [id, entry] : servers.items()) {
        if (legacyForgeLauncher(id, entry, forgeHome)) {
            legacyIds.push_back(id);
        }
    }
    for (const auto& id : legacyIds) {
        servers.erase(id);
    }
}

} // namespace

LMStudioConfigurationDocument LMStudioConfigurationCodec::empty()
{
    return LMStudioConfigurationDocument{"{\"mcpServers\":{}}"};
}

Domain::Result<LMStudioConfigurationDocument> LMStudioConfigurationCodec::parse(
    const std::span<const std::byte> content) noexcept
{
    try {
        if (content.size() > MaximumDocumentBytes) {
            return Domain::Result<LMStudioConfigurationDocument>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "LM Studio MCP configuration exceeds the 2 MiB safety limit."));
        }
        if (content.empty()) {
            return Domain::Result<LMStudioConfigurationDocument>::failure(Domain::makeError(
                Domain::ErrorCodes::MalformedMessage,
                "LM Studio MCP configuration is empty; the existing file was preserved."));
        }
        std::string source(content.size(), '\0');
        std::memcpy(source.data(), content.data(), content.size());
        static_cast<void>(decode(source));
        return Domain::Result<LMStudioConfigurationDocument>::success(
            LMStudioConfigurationDocument{std::move(source)});
    } catch (const ConfigurationCodecException& error) {
        return Domain::Result<LMStudioConfigurationDocument>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<LMStudioConfigurationDocument>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio MCP configuration could not be parsed."));
    }
}

Domain::Result<LMStudioConfigurationInspection> LMStudioConfigurationCodec::inspect(
    const LMStudioConfigurationDocument& document,
    const Domain::PathText& expectedBinary,
    const Domain::PathText& expectedForgeHome) noexcept
{
    try {
        const auto root = decode(document.sourceUtf8());
        const auto serversMember = root.find("mcpServers");
        const Json emptyServers = Json::object();
        const Json& servers = serversMember == root.end() ? emptyServers : *serversMember;

        LMStudioConfigurationInspection inspection;
        inspection.roles.reserve(2U);
        inspection.roles.push_back(inspectRole(
            servers, Domain::LMStudioConnectorRole::Primary, expectedBinary, expectedForgeHome));
        inspection.roles.push_back(inspectRole(
            servers, Domain::LMStudioConnectorRole::Fallback, expectedBinary, expectedForgeHome));

        const auto& primary = inspection.roles[0];
        const auto& fallback = inspection.roles[1];
        if (!primary.valid || !fallback.valid) {
            inspection.detail = primary.valid ? fallback.detail : primary.detail;
            return Domain::Result<LMStudioConfigurationInspection>::success(
                std::move(inspection));
        }
        if (!primary.deploymentId || !fallback.deploymentId ||
            primary.deploymentId.value() != fallback.deploymentId.value()) {
            inspection.detail = "primary and fallback do not share one nonempty deployment revision";
            return Domain::Result<LMStudioConfigurationInspection>::success(
                std::move(inspection));
        }
        inspection.registered = true;
        inspection.deploymentId = primary.deploymentId;
        inspection.detail = "primary and fallback registrations share the selected binary and revision";
        return Domain::Result<LMStudioConfigurationInspection>::success(
            std::move(inspection));
    } catch (const ConfigurationCodecException& error) {
        return Domain::Result<LMStudioConfigurationInspection>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<LMStudioConfigurationInspection>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio MCP configuration could not be inspected."));
    }
}

Domain::Result<std::vector<std::byte>> LMStudioConfigurationCodec::mergeForgeServers(
    const LMStudioConfigurationDocument& document,
    const Domain::PathText& binary,
    const Domain::PathText& forgeHome,
    const Domain::DeploymentId& deploymentId) noexcept
{
    try {
        if (deploymentId.value().empty()) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio deployment revision must not be empty."));
        }
        auto root = decode(document.sourceUtf8());
        auto& servers = root["mcpServers"];
        if (servers.is_null()) {
            servers = Json::object();
        }
        if (!servers.is_object()) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio MCP configuration mcpServers must be a JSON object."));
        }
        removeLegacyForgeLaunchers(servers, forgeHome);
        mergeRole(servers, Domain::LMStudioConnectorRole::Fallback,
                  binary, forgeHome, deploymentId);
        mergeRole(servers, Domain::LMStudioConnectorRole::Primary,
                  binary, forgeHome, deploymentId);
        auto encoded = root.dump(2, ' ', false, Json::error_handler_t::strict);
        encoded.push_back('\n');
        if (encoded.size() > MaximumDocumentBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Merged LM Studio MCP configuration exceeds the 2 MiB safety limit."));
        }

        auto candidate = bytesOf(encoded);
        auto reparsed = parse(candidate);
        if (!reparsed) {
            return Domain::Result<std::vector<std::byte>>::failure(
                std::move(reparsed).error());
        }
        auto inspected = inspect(reparsed.value(), binary, forgeHome);
        if (!inspected || !inspected.value().registered ||
            !inspected.value().deploymentId ||
            inspected.value().deploymentId.value() != deploymentId) {
            return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "Merged LM Studio MCP configuration did not retain both exact Forge registrations."));
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(candidate));
    } catch (const ConfigurationCodecException& error) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "LM Studio MCP configuration could not be merged."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
