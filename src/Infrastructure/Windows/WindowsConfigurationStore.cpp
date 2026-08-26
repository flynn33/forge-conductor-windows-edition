#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"

#include "Detail/BoundedSerialExecutor.h"
#include "Detail/OperationContextGuard.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using Json = nlohmann::json;

class ConfigurationDocumentException final : public std::runtime_error {
public:
    ConfigurationDocumentException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

struct ParsedConfiguration final {
    Json document;
    Domain::AppConfig configuration;
};

[[noreturn]] void reject(const std::string_view code, const std::string_view message)
{
    throw ConfigurationDocumentException{std::string{code}, std::string{message}};
}

[[nodiscard]] const Json* optionalMember(const Json& object, const std::string_view name)
{
    const auto iterator = object.find(name);
    return iterator == object.end() ? nullptr : &*iterator;
}

[[nodiscard]] const Json* optionalObject(const Json& object, const std::string_view name)
{
    const auto* value = optionalMember(object, name);
    if (value != nullptr && !value->is_object()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "A known configuration section is not a JSON object.");
    }
    return value;
}

[[nodiscard]] std::optional<std::int64_t> optionalInteger(const Json& object,
                                                          const std::string_view name)
{
    const auto* value = optionalMember(object, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_number_integer()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "A known configuration integer has the wrong JSON type.");
    }
    try {
        return value->get<std::int64_t>();
    } catch (...) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "A known configuration integer is outside its supported range.");
    }
}

[[nodiscard]] std::optional<bool> optionalBoolean(const Json& object, const std::string_view name)
{
    const auto* value = optionalMember(object, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_boolean()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "A known configuration boolean has the wrong JSON type.");
    }
    return value->get<bool>();
}

[[nodiscard]] std::optional<std::string> optionalString(const Json& object,
                                                        const std::string_view name)
{
    const auto* value = optionalMember(object, name);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (!value->is_string()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "A known configuration string has the wrong JSON type.");
    }
    return value->get<std::string>();
}

[[nodiscard]] std::chrono::seconds secondsValue(const std::optional<std::int64_t> value,
                                                const std::chrono::seconds fallback)
{
    if (!value) {
        return fallback;
    }
    if (*value <= 0) {
        reject(Domain::ErrorCodes::InvalidRequest, "A configuration duration must be positive.");
    }
    return std::chrono::seconds{*value};
}

[[nodiscard]] Domain::LogLevel parseLogLevel(const std::string_view value)
{
    if (value == "trace")
        return Domain::LogLevel::Trace;
    if (value == "debug")
        return Domain::LogLevel::Debug;
    if (value == "info")
        return Domain::LogLevel::Info;
    if (value == "warn")
        return Domain::LogLevel::Warning;
    if (value == "error")
        return Domain::LogLevel::Error;
    if (value == "critical")
        return Domain::LogLevel::Critical;
    reject(Domain::ErrorCodes::InvalidRequest,
           "Configuration log_level is not a supported wire value.");
}

[[nodiscard]] Domain::McpRole parseMcpRole(const std::string_view value)
{
    if (value == "primary")
        return Domain::McpRole::Primary;
    if (value == "fallback")
        return Domain::McpRole::Fallback;
    reject(Domain::ErrorCodes::InvalidRequest,
           "Configuration mcp.role is not primary or fallback.");
}

[[nodiscard]] std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool endsWith(const std::string_view value, const std::string_view suffix) noexcept
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool isSecretFieldName(const std::string& rawName)
{
    const auto name = lowercase(rawName);
    if (endsWith(name, "_ref")) {
        return false;
    }
    static const std::unordered_set<std::string> ExactNames{
        "secret",        "password",      "token",  "api_key",     "access_token",
        "refresh_token", "authorization", "cookie", "credentials", "private_key"};
    return ExactNames.contains(name) || endsWith(name, "_secret") || endsWith(name, "_password") ||
           endsWith(name, "_token") || endsWith(name, "_api_key");
}

void rejectSecretFields(const Json& value)
{
    if (value.is_object()) {
        for (const auto& [name, member] : value.items()) {
            if (isSecretFieldName(name)) {
                reject(Domain::ErrorCodes::RedactionRejected,
                       "Configuration documents may contain secret references, not secrets.");
            }
            rejectSecretFields(member);
        }
    } else if (value.is_array()) {
        for (const auto& member : value) {
            rejectSecretFields(member);
        }
    }
}

[[nodiscard]] Domain::AppConfig decodeConfiguration(const Json& document)
{
    if (!document.is_object()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "Configuration document root must be a JSON object.");
    }

    const auto schemaVersion = optionalInteger(document, "schema_version");
    if (!schemaVersion || *schemaVersion != WindowsConfigurationStore::SchemaVersion) {
        reject(Domain::ErrorCodes::UnsupportedVersion, "Configuration schema_version must be 1.");
    }
    rejectSecretFields(document);

    auto configuration = Domain::defaultAppConfig();
    if (const auto value = optionalString(document, "log_level")) {
        configuration.logLevel = parseLogLevel(*value);
    }

    if (const auto* roots = optionalMember(document, "allowed_roots")) {
        if (!roots->is_array()) {
            reject(Domain::ErrorCodes::InvalidRequest,
                   "Configuration allowed_roots must be a JSON array.");
        }
        if (roots->size() > Domain::MaximumAppConfigAllowedRootCount) {
            reject(Domain::ErrorCodes::LimitExceeded,
                   "Configuration allowed_roots exceed 32 entries.");
        }
        std::vector<Domain::PathText> parsedRoots;
        parsedRoots.reserve(roots->size());
        for (const auto& root : *roots) {
            if (!root.is_string()) {
                reject(Domain::ErrorCodes::InvalidRequest,
                       "Every configuration allowed root must be a string.");
            }
            auto path = Domain::PathText::create(root.get_ref<const std::string&>());
            if (!path) {
                reject(path.error().code, path.error().message);
            }
            parsedRoots.emplace_back(std::move(path).value());
        }
        configuration.allowedRoots = std::move(parsedRoots);
    }

    if (const auto* shell = optionalObject(document, "shell")) {
        if (const auto enabled = optionalBoolean(*shell, "enabled")) {
            configuration.shell.enabled = *enabled;
        }
        configuration.shell.defaultTimeout = secondsValue(
            optionalInteger(*shell, "default_timeout_sec"), configuration.shell.defaultTimeout);
    }
    if (const auto* dashboard = optionalObject(document, "dashboard")) {
        if (const auto host = optionalString(*dashboard, "host")) {
            configuration.dashboard.host = *host;
        }
        if (const auto port = optionalInteger(*dashboard, "port")) {
            if (*port <= 0 || *port > 65'535) {
                reject(Domain::ErrorCodes::InvalidRequest,
                       "Configuration dashboard port is outside 1 through 65535.");
            }
            configuration.dashboard.port = static_cast<std::uint16_t>(*port);
        }
        configuration.dashboard.refreshInterval =
            secondsValue(optionalInteger(*dashboard, "refresh_interval_sec"),
                         configuration.dashboard.refreshInterval);
    }
    if (const auto* manager = optionalObject(document, "manager")) {
        if (const auto value = optionalBoolean(*manager, "auto_restart")) {
            configuration.manager.autoRestart = *value;
        }
        configuration.manager.watchdogInterval =
            secondsValue(optionalInteger(*manager, "watchdog_interval_sec"),
                         configuration.manager.watchdogInterval);
        if (const auto value = optionalBoolean(*manager, "open_browser_on_start")) {
            configuration.manager.openBrowserOnStart = *value;
        }
    }
    if (const auto* mcp = optionalObject(document, "mcp")) {
        if (const auto role = optionalString(*mcp, "role")) {
            configuration.mcpRole = parseMcpRole(*role);
        }
    }
    if (const auto* sessions = optionalObject(document, "sessions")) {
        configuration.sessions.idleTimeToLive = secondsValue(
            optionalInteger(*sessions, "idle_ttl_sec"), configuration.sessions.idleTimeToLive);
    }
    if (const auto* coordinator = optionalObject(document, "coordinator")) {
        if (const auto enabled = optionalBoolean(*coordinator, "enabled")) {
            configuration.coordinator.enabled = *enabled;
        }
        configuration.coordinator.leaseTimeToLive =
            secondsValue(optionalInteger(*coordinator, "lease_ttl_sec"),
                         configuration.coordinator.leaseTimeToLive);
        configuration.coordinator.presenceTimeToLive =
            secondsValue(optionalInteger(*coordinator, "presence_ttl_sec"),
                         configuration.coordinator.presenceTimeToLive);
    }

    auto valid = Domain::validateAppConfig(configuration);
    if (!valid) {
        reject(valid.error().code, valid.error().message);
    }
    return configuration;
}

void writeKnownConfiguration(Json& document, const Domain::AppConfig& configuration)
{
    document["schema_version"] = WindowsConfigurationStore::SchemaVersion;
    document["log_level"] = Domain::wireName(configuration.logLevel);

    Json roots = Json::array();
    for (const auto& root : configuration.allowedRoots) {
        roots.push_back(root.value());
    }
    document["allowed_roots"] = std::move(roots);

    auto& shell = document["shell"];
    if (!shell.is_object())
        shell = Json::object();
    shell["enabled"] = configuration.shell.enabled;
    shell["default_timeout_sec"] = configuration.shell.defaultTimeout.count();

    auto& dashboard = document["dashboard"];
    if (!dashboard.is_object())
        dashboard = Json::object();
    dashboard["host"] = configuration.dashboard.host;
    dashboard["port"] = configuration.dashboard.port;
    dashboard["refresh_interval_sec"] = configuration.dashboard.refreshInterval.count();

    auto& manager = document["manager"];
    if (!manager.is_object())
        manager = Json::object();
    manager["auto_restart"] = configuration.manager.autoRestart;
    manager["watchdog_interval_sec"] = configuration.manager.watchdogInterval.count();
    manager["open_browser_on_start"] = configuration.manager.openBrowserOnStart;

    auto& mcp = document["mcp"];
    if (!mcp.is_object())
        mcp = Json::object();
    mcp["role"] = Domain::wireName(configuration.mcpRole);

    auto& sessions = document["sessions"];
    if (!sessions.is_object())
        sessions = Json::object();
    sessions["idle_ttl_sec"] = configuration.sessions.idleTimeToLive.count();

    auto& coordinator = document["coordinator"];
    if (!coordinator.is_object())
        coordinator = Json::object();
    coordinator["enabled"] = configuration.coordinator.enabled;
    coordinator["lease_ttl_sec"] = configuration.coordinator.leaseTimeToLive.count();
    coordinator["presence_ttl_sec"] = configuration.coordinator.presenceTimeToLive.count();
}

[[nodiscard]] Json defaultDocument()
{
    Json document = Json::object();
    writeKnownConfiguration(document, Domain::defaultAppConfig());
    return document;
}

[[nodiscard]] Domain::Result<ParsedConfiguration>
parseDocument(const std::span<const std::byte> bytes) noexcept
{
    try {
        if (bytes.size() > WindowsConfigurationStore::MaximumDocumentBytes) {
            return Domain::Result<ParsedConfiguration>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Configuration document exceeds 2 MiB."));
        }
        const std::string text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        std::vector<std::unordered_set<std::string>> objectKeys;
        const auto callback = [&objectKeys](const int depth, const Json::parse_event_t event,
                                            Json& parsed) {
            if (depth < 0 ||
                static_cast<std::size_t>(depth) > WindowsConfigurationStore::MaximumJsonDepth) {
                reject(Domain::ErrorCodes::LimitExceeded,
                       "Configuration JSON nesting exceeds depth 32.");
            }
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
                if (objectKeys.empty() ||
                    !objectKeys.back().insert(parsed.get<std::string>()).second) {
                    reject(Domain::ErrorCodes::MalformedMessage,
                           "Configuration JSON contains a duplicate object key.");
                }
            } else if (event == Json::parse_event_t::object_end) {
                if (objectKeys.empty()) {
                    reject(Domain::ErrorCodes::MalformedMessage,
                           "Configuration JSON object nesting is malformed.");
                }
                objectKeys.pop_back();
            }
            return true;
        };

        auto document = Json::parse(text, callback, true, false);
        auto configuration = decodeConfiguration(document);
        return Domain::Result<ParsedConfiguration>::success(
            ParsedConfiguration{std::move(document), std::move(configuration)});
    } catch (const ConfigurationDocumentException& error) {
        return Domain::Result<ParsedConfiguration>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (const Json::parse_error&) {
        return Domain::Result<ParsedConfiguration>::failure(
            Domain::makeError(Domain::ErrorCodes::MalformedMessage,
                              "Configuration document is not valid strict JSON."));
    } catch (...) {
        return Domain::Result<ParsedConfiguration>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Configuration document parsing failed."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::byte>>
serializeDocument(const Json& document) noexcept
{
    try {
        auto text = document.dump(2, ' ', false, Json::error_handler_t::strict);
        text.push_back('\n');
        if (text.size() > WindowsConfigurationStore::MaximumDocumentBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "Updated configuration document exceeds 2 MiB."));
        }
        std::vector<std::byte> bytes(text.size());
        if (!text.empty()) {
            std::memcpy(bytes.data(), text.data(), text.size());
        }
        return Domain::Result<std::vector<std::byte>>::success(std::move(bytes));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Configuration document serialization failed."));
    }
}

[[nodiscard]] bool samePathBinding(const Contracts::AuthorizedPath& readPath,
                                   const Contracts::AuthorizedPath& writePath,
                                   const Contracts::AuthorizedPath& createPath) noexcept
{
    return readPath.access() == Domain::FileAccess::Read &&
           writePath.access() == Domain::FileAccess::Write &&
           createPath.access() == Domain::FileAccess::Create &&
           readPath.authorityId() == writePath.authorityId() &&
           readPath.authorityId() == createPath.authorityId() &&
           readPath.canonicalPath() == writePath.canonicalPath() &&
           readPath.canonicalPath() == createPath.canonicalPath() &&
           readPath.authorityRoot() == writePath.authorityRoot() &&
           readPath.authorityRoot() == createPath.authorityRoot();
}

[[nodiscard]] bool backupPathBinding(const Contracts::AuthorizedPath& primaryReadPath,
                                     const Contracts::AuthorizedPath& backupReadPath) noexcept
{
    const auto& primary = primaryReadPath.canonicalPath().value();
    const auto& backup = backupReadPath.canonicalPath().value();
    return backupReadPath.access() == Domain::FileAccess::Read &&
           primaryReadPath.authorityId() == backupReadPath.authorityId() &&
           primaryReadPath.authorityRoot() == backupReadPath.authorityRoot() &&
           backup.size() == primary.size() + 4U && backup.starts_with(primary) &&
           backup.ends_with(".bak");
}

} // namespace

class WindowsConfigurationStore::Impl final {
public:
    Impl(Contracts::IAtomicFileStore& atomicFileStore, Contracts::AuthorizedPath readPath,
         Contracts::AuthorizedPath writePath, Contracts::AuthorizedPath createPath,
         Contracts::AuthorizedPath backupReadPath)
        : atomicFileStore_{atomicFileStore}, readPath_{std::move(readPath)},
          writePath_{std::move(writePath)}, createPath_{std::move(createPath)},
          backupReadPath_{std::move(backupReadPath)}, document_{defaultDocument()},
          snapshot_{Domain::defaultAppConfig()}
    {
        if (!samePathBinding(readPath_, writePath_, createPath_) ||
            !backupPathBinding(readPath_, backupReadPath_)) {
            throw std::invalid_argument("Configuration capabilities must bind one primary file and "
                                        "its read-only sibling backup under the same authority.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    load(const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Configuration load");
        if (!lease) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(lease).error());
        }
        if (loaded_) {
            return Domain::Result<Domain::AppConfig>::success(snapshot_);
        }
        return loadFromDisk(context);
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    reload(const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Configuration reload");
        if (!lease) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(lease).error());
        }
        return loadFromDisk(context);
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    update(const Domain::AppConfigPatch& patch, const Domain::OperationContext& context) noexcept
    {
        auto lease = executor_.acquire(context, "Configuration update");
        if (!lease) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(lease).error());
        }
        if (!loaded_) {
            auto loaded = loadFromDisk(context);
            if (!loaded) {
                return loaded;
            }
        }

        auto updated = Domain::applyConfigPatch(snapshot_, patch);
        if (!updated) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(updated).error());
        }
        auto candidateDocument = document_;
        writeKnownConfiguration(candidateDocument, updated.value());
        auto serialized = serializeDocument(candidateDocument);
        if (!serialized) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(serialized).error());
        }

        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "Configuration update");
        if (!validContext) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(validContext).error());
        }
        const auto& persistPath = targetExists_ ? writePath_ : createPath_;
        auto committed = atomicFileStore_.replace(persistPath, serialized.value(), true, context);
        if (!committed) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(committed).error());
        }

        targetExists_ = true;
        // Durable replacement is the linearization point. Publish the matching
        // immutable snapshot even if cancellation arrives immediately afterward.
        document_ = std::move(candidateDocument);
        snapshot_ = std::move(updated).value();
        loaded_ = true;
        return Domain::Result<Domain::AppConfig>::success(snapshot_);
    }

    void shutdown() noexcept { executor_.shutdown(); }

private:
    [[nodiscard]] Domain::Result<Domain::AppConfig>
    loadFromDisk(const Domain::OperationContext& context) noexcept
    {
        auto content = atomicFileStore_.read(readPath_, MaximumDocumentBytes, context);
        if (!content) {
            if (content.error().code == Domain::ErrorCodes::RecordNotFound) {
                auto validContext = Detail::validateOperationContext(
                    context, std::chrono::steady_clock::now(), "Configuration load");
                if (!validContext) {
                    return Domain::Result<Domain::AppConfig>::failure(
                        std::move(validContext).error());
                }
                targetExists_ = false;
                document_ = defaultDocument();
                snapshot_ = Domain::defaultAppConfig();
                loaded_ = true;
                return Domain::Result<Domain::AppConfig>::success(snapshot_);
            }
            if (content.error().code == Domain::ErrorCodes::PayloadTooLarge ||
                content.error().code == Domain::ErrorCodes::IntegrityFailure) {
                return recoverFromBackup(context);
            }
            return Domain::Result<Domain::AppConfig>::failure(std::move(content).error());
        }

        auto parsed = parseDocument(content.value());
        if (!parsed) {
            return recoverFromBackup(context);
        }
        return publishLoaded(std::move(parsed).value(), context);
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    recoverFromBackup(const Domain::OperationContext& context) noexcept
    {
        auto content = atomicFileStore_.read(backupReadPath_, MaximumDocumentBytes, context);
        if (!content) {
            if (content.error().code != Domain::ErrorCodes::RecordNotFound &&
                content.error().code != Domain::ErrorCodes::PayloadTooLarge &&
                content.error().code != Domain::ErrorCodes::IntegrityFailure) {
                return Domain::Result<Domain::AppConfig>::failure(std::move(content).error());
            }
            return invalidRecoveryPair();
        }

        auto parsed = parseDocument(content.value());
        if (!parsed) {
            return invalidRecoveryPair();
        }
        return publishLoaded(std::move(parsed).value(), context);
    }

    [[nodiscard]] Domain::Result<Domain::AppConfig>
    publishLoaded(ParsedConfiguration parsed, const Domain::OperationContext& context) noexcept
    {
        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "Configuration load");
        if (!validContext) {
            return Domain::Result<Domain::AppConfig>::failure(std::move(validContext).error());
        }
        targetExists_ = true;
        document_ = std::move(parsed.document);
        snapshot_ = std::move(parsed.configuration);
        loaded_ = true;
        return Domain::Result<Domain::AppConfig>::success(snapshot_);
    }

    [[nodiscard]] static Domain::Result<Domain::AppConfig> invalidRecoveryPair()
    {
        return Domain::Result<Domain::AppConfig>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The primary configuration is corrupt and no valid sibling backup is available."));
    }

    Contracts::IAtomicFileStore& atomicFileStore_;
    const Contracts::AuthorizedPath readPath_;
    const Contracts::AuthorizedPath writePath_;
    const Contracts::AuthorizedPath createPath_;
    const Contracts::AuthorizedPath backupReadPath_;
    Detail::BoundedSerialExecutor executor_;
    Json document_;
    Domain::AppConfig snapshot_;
    bool loaded_{};
    bool targetExists_{};
};

WindowsConfigurationStore::WindowsConfigurationStore(Contracts::IAtomicFileStore& atomicFileStore,
                                                     Contracts::AuthorizedPath readPath,
                                                     Contracts::AuthorizedPath writePath,
                                                     Contracts::AuthorizedPath createPath,
                                                     Contracts::AuthorizedPath backupReadPath)
    : implementation_{std::make_unique<Impl>(atomicFileStore, std::move(readPath),
                                             std::move(writePath), std::move(createPath),
                                             std::move(backupReadPath))}
{
}

WindowsConfigurationStore::~WindowsConfigurationStore() { shutdown(); }

Domain::Result<Domain::AppConfig>
WindowsConfigurationStore::load(const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->load(context);
    } catch (...) {
        return Domain::Result<Domain::AppConfig>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Configuration load failed at the platform boundary."));
    }
}

Domain::Result<Domain::AppConfig>
WindowsConfigurationStore::update(const Domain::AppConfigPatch& patch,
                                  const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->update(patch, context);
    } catch (...) {
        return Domain::Result<Domain::AppConfig>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Configuration update failed at the platform boundary."));
    }
}

Domain::Result<Domain::AppConfig>
WindowsConfigurationStore::reload(const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->reload(context);
    } catch (...) {
        return Domain::Result<Domain::AppConfig>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Configuration reload failed at the platform boundary."));
    }
}

void WindowsConfigurationStore::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
