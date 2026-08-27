#include "ForgeConductor/Dashboard/DashboardManagerJsonCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {
namespace {

using Json = nlohmann::json;

class DashboardCodecException final : public std::runtime_error {
public:
    DashboardCodecException(std::string code, std::string message)
        : std::runtime_error{std::move(message)}, code_{std::move(code)}
    {
    }

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[noreturn]] void reject(
    const std::string_view code,
    const std::string_view message)
{
    throw DashboardCodecException{std::string{code}, std::string{message}};
}

[[nodiscard]] bool isValidUtf8(const std::string_view value) noexcept
{
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        const auto continuation = [&](const std::size_t offset) noexcept {
            return index + offset < value.size() &&
                (static_cast<unsigned char>(value[index + offset]) & 0xc0U) ==
                    0x80U;
        };
        if (first >= 0xc2U && first <= 0xdfU) {
            if (!continuation(1U)) return false;
            index += 2U;
            continue;
        }
        if (first == 0xe0U) {
            if (index + 2U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0xa0U || second > 0xbfU || !continuation(2U)) {
                return false;
            }
            index += 3U;
            continue;
        }
        if ((first >= 0xe1U && first <= 0xecU) ||
            (first >= 0xeeU && first <= 0xefU)) {
            if (!continuation(1U) || !continuation(2U)) return false;
            index += 3U;
            continue;
        }
        if (first == 0xedU) {
            if (index + 2U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x9fU || !continuation(2U)) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first == 0xf0U) {
            if (index + 3U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x90U || second > 0xbfU || !continuation(2U) ||
                !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first >= 0xf1U && first <= 0xf3U) {
            if (!continuation(1U) || !continuation(2U) ||
                !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first == 0xf4U) {
            if (index + 3U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x8fU || !continuation(2U) ||
                !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

void requireValidText(
    const std::string_view value,
    const std::string_view field)
{
    if (value.find('\0') != std::string_view::npos || !isValidUtf8(value)) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{field} + " must be strict UTF-8 without NUL bytes.");
    }
}

class StrictJsonSax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t&) override
    {
        return scalar();
    }

    bool string(string_t& value) override
    {
        if (value.find('\0') != std::string::npos) {
            embeddedNul_ = true;
            return false;
        }
        return scalar();
    }

    bool binary(binary_t&) override { return scalar(); }

    bool start_object(std::size_t) override
    {
        if (!startContainer()) return false;
        containers_.push_back(Container{true, {}});
        return true;
    }

    bool key(string_t& name) override
    {
        if (name.find('\0') != std::string::npos) {
            embeddedNul_ = true;
            return false;
        }
        if (containers_.empty() || !containers_.back().isObject) {
            malformed_ = true;
            return false;
        }
        if (!containers_.back().keys.insert(name).second) {
            duplicateKey_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override
    {
        if (containers_.empty() || !containers_.back().isObject) {
            malformed_ = true;
            return false;
        }
        containers_.pop_back();
        --depth_;
        return true;
    }

    bool start_array(std::size_t) override
    {
        if (!startContainer()) return false;
        containers_.push_back(Container{false, {}});
        return true;
    }

    bool end_array() override
    {
        if (containers_.empty() || containers_.back().isObject) {
            malformed_ = true;
            return false;
        }
        containers_.pop_back();
        --depth_;
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

    [[nodiscard]] bool duplicateKey() const noexcept { return duplicateKey_; }
    [[nodiscard]] bool embeddedNul() const noexcept { return embeddedNul_; }
    [[nodiscard]] bool tooDeep() const noexcept { return tooDeep_; }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    struct Container final {
        bool isObject{};
        std::unordered_set<std::string> keys;
    };

    [[nodiscard]] bool scalar() const noexcept { return true; }

    [[nodiscard]] bool startContainer() noexcept
    {
        ++depth_;
        if (depth_ > DashboardManagerJsonCodec::MaximumJsonNesting) {
            tooDeep_ = true;
            return false;
        }
        return true;
    }

    std::vector<Container> containers_;
    std::size_t depth_{};
    bool duplicateKey_{};
    bool embeddedNul_{};
    bool tooDeep_{};
    bool malformed_{};
};

class BoundedJsonStreamBuffer final : public std::streambuf {
public:
    explicit BoundedJsonStreamBuffer(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
        bytes_.reserve((std::min)(maximumBytes, std::size_t{4096U}));
    }

    [[nodiscard]] bool exceeded() const noexcept { return exceeded_; }

    [[nodiscard]] std::vector<std::byte> takeBytes() &&
    {
        return std::move(bytes_);
    }

protected:
    int_type overflow(const int_type character) override
    {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        if (bytes_.size() == maximumBytes_) {
            exceeded_ = true;
            return traits_type::eof();
        }
        bytes_.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(traits_type::to_char_type(character))));
        return character;
    }

    std::streamsize xsputn(
        const char_type* source,
        const std::streamsize count) override
    {
        if (count <= 0) return 0;
        const auto requested = static_cast<std::size_t>(count);
        const auto available = maximumBytes_ - bytes_.size();
        const auto accepted = (std::min)(requested, available);
        bytes_.reserve(bytes_.size() + accepted);
        std::transform(
            source,
            source + accepted,
            std::back_inserter(bytes_),
            [](const char value) {
                return static_cast<std::byte>(
                    static_cast<unsigned char>(value));
            });
        if (accepted != requested) exceeded_ = true;
        return static_cast<std::streamsize>(accepted);
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t maximumBytes_{};
    bool exceeded_{};
};

void validateMaximum(
    const std::size_t maximumBytes,
    const std::size_t hardMaximum,
    const std::string_view kind)
{
    if (maximumBytes == 0U || maximumBytes > hardMaximum) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{kind} + " byte limit must be within the hard bound.");
    }
}

class JsonResponseBudget final {
public:
    explicit JsonResponseBudget(const std::size_t maximumBytes)
        : maximumBytes_{maximumBytes}
    {
    }

    void beginObject() { add(1U); }
    void endObject() { add(1U); }

    void member(const std::string_view name, bool& first)
    {
        if (!first) add(1U);
        first = false;
        string(name);
        add(1U);
    }

    void string(const std::string_view value)
    {
        add(1U);
        for (const unsigned char character : value) {
            if (character == '"' || character == '\\' || character == '\b' ||
                character == '\f' || character == '\n' || character == '\r' ||
                character == '\t') {
                add(2U);
            } else if (character < 0x20U) {
                add(6U);
            } else {
                add(1U);
            }
        }
        add(1U);
    }

    void boolean(const bool value) { add(value ? 4U : 5U); }
    void null() { add(4U); }

    template <typename Integer>
    void integer(const Integer value)
    {
        std::array<char, 32U> buffer{};
        const auto result = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc{}) {
            reject(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard manager response integer could not be measured.");
        }
        add(static_cast<std::size_t>(result.ptr - buffer.data()));
    }

private:
    void add(const std::size_t count)
    {
        if (count > maximumBytes_ - measuredBytes_) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard manager response exceeds its byte limit.");
        }
        measuredBytes_ += count;
    }

    std::size_t maximumBytes_{};
    std::size_t measuredBytes_{};
};

[[nodiscard]] std::vector<std::byte> boundedSerialize(
    const Json& document,
    const std::size_t maximumBytes)
{
    validateMaximum(
        maximumBytes,
        DashboardManagerJsonCodec::MaximumResponseBytes,
        "Dashboard manager response");

    BoundedJsonStreamBuffer buffer{maximumBytes};
    std::ostream output{&buffer};
    output.exceptions(std::ios_base::badbit | std::ios_base::failbit);
    try {
        output << document;
        output.flush();
    } catch (const std::ios_base::failure&) {
        if (buffer.exceeded()) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "Dashboard manager response exceeds its byte limit.");
        }
        throw;
    }
    if (buffer.exceeded()) {
        reject(
            Domain::ErrorCodes::PayloadTooLarge,
            "Dashboard manager response exceeds its byte limit.");
    }
    return std::move(buffer).takeBytes();
}

[[nodiscard]] Json parseMutation(
    const std::span<const std::byte> body,
    const std::size_t maximumBytes)
{
    validateMaximum(
        maximumBytes,
        DashboardManagerJsonCodec::MaximumMutationBytes,
        "Dashboard manager mutation");
    if (body.size() > maximumBytes) {
        reject(
            Domain::ErrorCodes::PayloadTooLarge,
            "Dashboard manager mutation exceeds its byte limit.");
    }
    if (body.empty()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation body is empty.");
    }

    std::string payload(body.size(), '\0');
    std::memcpy(payload.data(), body.data(), body.size());
    if (payload.find('\0') != std::string::npos || !isValidUtf8(payload)) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation is not strict NUL-free UTF-8.");
    }

    StrictJsonSax preflight;
    const bool accepted = Json::sax_parse(payload, &preflight);
    if (preflight.duplicateKey()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation contains a duplicate object key.");
    }
    if (preflight.embeddedNul()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation contains an embedded NUL.");
    }
    if (preflight.tooDeep()) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            "Dashboard manager mutation exceeds its JSON nesting limit.");
    }
    if (!accepted || preflight.malformed()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation is malformed JSON.");
    }

    try {
        return Json::parse(payload, nullptr, true, false);
    } catch (const Json::exception&) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard manager mutation is malformed JSON.");
    }
}

void requireObject(const Json& value, const std::string_view schema)
{
    if (!value.is_object()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{schema} + " must be a JSON object.");
    }
}

[[nodiscard]] bool allowedName(
    const std::string_view name,
    const std::initializer_list<std::string_view> allowed) noexcept
{
    return std::find(allowed.begin(), allowed.end(), name) != allowed.end();
}

void requireAllowedFields(
    const Json& value,
    const std::initializer_list<std::string_view> allowed,
    const std::string_view schema)
{
    requireObject(value, schema);
    for (const auto& [name, unused] : value.items()) {
        static_cast<void>(unused);
        if (!allowedName(name, allowed)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{schema} + " contains an unknown field.");
        }
    }
}

[[nodiscard]] const Json& member(
    const Json& object,
    const std::string_view name)
{
    return object.at(std::string{name});
}

[[nodiscard]] bool booleanValue(
    const Json& object,
    const std::string_view name)
{
    const auto& value = member(object, name);
    if (!value.is_boolean()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a JSON boolean.");
    }
    return value.get<bool>();
}

[[nodiscard]] const std::string& stringValue(
    const Json& object,
    const std::string_view name)
{
    const auto& value = member(object, name);
    if (!value.is_string()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a JSON string.");
    }
    const auto& text = value.get_ref<const std::string&>();
    requireValidText(text, name);
    return text;
}

[[nodiscard]] std::int64_t integerValue(
    const Json& object,
    const std::string_view name)
{
    const auto& value = member(object, name);
    if (value.is_number_unsigned()) {
        const auto unsignedValue = value.get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(
                                (std::numeric_limits<std::int64_t>::max)())) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{name} + " exceeds the signed integer range.");
        }
        return static_cast<std::int64_t>(unsignedValue);
    }
    if (!value.is_number_integer()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a JSON integer.");
    }
    return value.get<std::int64_t>();
}

[[nodiscard]] std::chrono::seconds positiveSeconds(
    const Json& object,
    const std::string_view name)
{
    const auto count = integerValue(object, name);
    if (count <= 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be positive.");
    }
    using Rep = std::chrono::seconds::rep;
    if constexpr (sizeof(Rep) < sizeof(std::int64_t)) {
        if (count > static_cast<std::int64_t>((std::numeric_limits<Rep>::max)())) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{name} + " exceeds the supported duration range.");
        }
    }
    return std::chrono::seconds{static_cast<Rep>(count)};
}

[[nodiscard]] std::uint16_t portValue(
    const Json& object,
    const std::string_view name)
{
    const auto number = integerValue(object, name);
    if (number <= 0 || number > (std::numeric_limits<std::uint16_t>::max)()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be within 1 through 65535.");
    }
    return static_cast<std::uint16_t>(number);
}

[[nodiscard]] Domain::LogLevel parseLogLevel(const std::string_view value)
{
    if (value == "trace") return Domain::LogLevel::Trace;
    if (value == "debug") return Domain::LogLevel::Debug;
    if (value == "info") return Domain::LogLevel::Info;
    if (value == "warn") return Domain::LogLevel::Warning;
    if (value == "error") return Domain::LogLevel::Error;
    if (value == "critical") return Domain::LogLevel::Critical;
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "log_level contains an unsupported value.");
}

[[nodiscard]] std::string_view logLevelName(const Domain::LogLevel level)
{
    switch (level) {
    case Domain::LogLevel::Trace: return "trace";
    case Domain::LogLevel::Debug: return "debug";
    case Domain::LogLevel::Info: return "info";
    case Domain::LogLevel::Warning: return "warn";
    case Domain::LogLevel::Error: return "error";
    case Domain::LogLevel::Critical: return "critical";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager settings contain an invalid log level.");
}

[[nodiscard]] std::string_view serviceStateName(
    const Domain::ManagerServiceState state)
{
    switch (state) {
    case Domain::ManagerServiceState::Stopped: return "stopped";
    case Domain::ManagerServiceState::Starting: return "starting";
    case Domain::ManagerServiceState::Running: return "running";
    case Domain::ManagerServiceState::Restarting: return "restarting";
    case Domain::ManagerServiceState::Stopping: return "stopping";
    case Domain::ManagerServiceState::Failed: return "failed";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager status contains an invalid service state.");
}

void validateSettings(const Domain::ManagerSettings& settings)
{
    auto valid = Domain::validateManagerSettings(settings);
    if (!valid) reject(valid.error().code, valid.error().message);
    static_cast<void>(logLevelName(settings.logLevel));
}

void validateStatus(const Domain::ManagerStatus& status)
{
    static_cast<void>(serviceStateName(status.state));
    if (status.uptime && *status.uptime < std::chrono::seconds::zero()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager status uptime_sec must not be negative.");
    }
    if (status.startedAt &&
        status.startedAt->time_since_epoch() <
            Domain::UtcTimePoint::duration::zero()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager status started_at must not precede the Unix epoch.");
    }

    Domain::ManagerSettings projection;
    projection.dashboardHost = status.dashboardHost;
    projection.dashboardPort = status.dashboardPort;
    projection.dashboardRefreshInterval = status.dashboardRefreshInterval;
    projection.autoRestart = status.autoRestart;
    projection.watchdogInterval = status.watchdogInterval;
    projection.openBrowserOnStart = status.openBrowserOnStart;
    validateSettings(projection);
    requireValidText(status.home.value(), "home");
    requireValidText(status.version, "version");
    if (status.lastError) requireValidText(*status.lastError, "last_error");
}

[[nodiscard]] std::string canonicalDashboardUrl(
    const std::string_view host,
    const std::uint16_t port)
{
    if (host == "127.0.0.1") {
        return "http://127.0.0.1:" + std::to_string(port) + '/';
    }
    if (host == "::1") {
        return "http://[::1]:" + std::to_string(port) + '/';
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard URL requires a canonical loopback host.");
}

[[nodiscard]] std::string timestampText(const Domain::UtcTimePoint timestamp)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        timestamp.time_since_epoch());
    const __time64_t encoded = static_cast<__time64_t>(seconds.count());
    std::tm utc{};
    if (::_gmtime64_s(&utc, &encoded) != 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager status started_at is outside the supported UTC range.");
    }
    std::array<char, 21U> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec);
    if (written != 20) {
        reject(
            Domain::ErrorCodes::InternalFailure,
            "Manager status started_at could not be formatted.");
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

void measureStatusObject(
    JsonResponseBudget& budget,
    const Domain::ManagerStatus& status)
{
    budget.beginObject();
    bool first{true};
    const auto member = [&](const std::string_view name) {
        budget.member(name, first);
    };

    member("ok");
    budget.boolean(status.ok);
    member("manager");
    budget.boolean(status.isManager);
    member("state");
    budget.string(serviceStateName(status.state));
    member("desired_running");
    budget.boolean(status.desiredRunning);
    member("http_listening");
    budget.boolean(status.httpListening);
    member("service_active");
    budget.boolean(status.serviceActive);
    member("pid");
    budget.integer(status.processId);
    member("started_at");
    if (status.startedAt) {
        budget.string(timestampText(*status.startedAt));
    } else {
        budget.null();
    }
    member("uptime_sec");
    if (status.uptime) {
        budget.integer(status.uptime->count());
    } else {
        budget.null();
    }
    member("restart_count");
    budget.integer(status.restartCount);
    member("last_error");
    if (status.lastError) {
        budget.string(*status.lastError);
    } else {
        budget.null();
    }
    member("auto_restart");
    budget.boolean(status.autoRestart);
    member("watchdog_interval_sec");
    budget.integer(status.watchdogInterval.count());
    member("open_browser_on_start");
    budget.boolean(status.openBrowserOnStart);
    member("dashboard");
    budget.beginObject();
    bool firstDashboard{true};
    budget.member("host", firstDashboard);
    budget.string(status.dashboardHost);
    budget.member("port", firstDashboard);
    budget.integer(status.dashboardPort);
    budget.member("url", firstDashboard);
    budget.string(canonicalDashboardUrl(
        status.dashboardHost, status.dashboardPort));
    budget.member("refresh_interval_sec", firstDashboard);
    budget.integer(status.dashboardRefreshInterval.count());
    budget.endObject();
    member("home");
    budget.string(status.home.value());
    member("version");
    budget.string(status.version);
    budget.endObject();
}

void measureSettingsMembers(
    JsonResponseBudget& budget,
    bool& first,
    const Domain::ManagerSettings& settings)
{
    budget.member("ok", first);
    budget.boolean(true);
    budget.member("dashboard", first);
    budget.beginObject();
    bool firstDashboard{true};
    budget.member("host", firstDashboard);
    budget.string(settings.dashboardHost);
    budget.member("port", firstDashboard);
    budget.integer(settings.dashboardPort);
    budget.member("refresh_interval_sec", firstDashboard);
    budget.integer(settings.dashboardRefreshInterval.count());
    budget.endObject();
    budget.member("manager", first);
    budget.beginObject();
    bool firstManager{true};
    budget.member("auto_restart", firstManager);
    budget.boolean(settings.autoRestart);
    budget.member("watchdog_interval_sec", firstManager);
    budget.integer(settings.watchdogInterval.count());
    budget.member("open_browser_on_start", firstManager);
    budget.boolean(settings.openBrowserOnStart);
    budget.endObject();
    budget.member("sessions", first);
    budget.beginObject();
    bool firstSessions{true};
    budget.member("idle_ttl_sec", firstSessions);
    budget.integer(settings.sessionIdleTtl.count());
    budget.endObject();
    budget.member("shell", first);
    budget.beginObject();
    bool firstShell{true};
    budget.member("default_timeout_sec", firstShell);
    budget.integer(settings.shellTimeout.count());
    budget.endObject();
    budget.member("log_level", first);
    budget.string(logLevelName(settings.logLevel));
}

void preflightStatusResponse(
    const Domain::ManagerStatus& status,
    const std::size_t maximumBytes)
{
    JsonResponseBudget budget{maximumBytes};
    measureStatusObject(budget, status);
}

void preflightSettingsResponse(
    const Domain::ManagerSettings& settings,
    const std::size_t maximumBytes)
{
    JsonResponseBudget budget{maximumBytes};
    budget.beginObject();
    bool first{true};
    measureSettingsMembers(budget, first, settings);
    budget.endObject();
}

void preflightSettingsUpdateResponse(
    const Domain::ManagerSettingsUpdateOutcome& outcome,
    const std::size_t maximumBytes)
{
    JsonResponseBudget budget{maximumBytes};
    budget.beginObject();
    bool first{true};
    measureSettingsMembers(budget, first, outcome.settings);
    budget.member("applied", first);
    budget.boolean(outcome.applied);
    budget.member("bind_changed", first);
    budget.boolean(outcome.bindingChanged);
    budget.member("status", first);
    measureStatusObject(budget, outcome.status);
    budget.endObject();
}

[[nodiscard]] Json statusJson(const Domain::ManagerStatus& status)
{
    validateStatus(status);
    Json dashboard = Json::object();
    dashboard["host"] = status.dashboardHost;
    dashboard["port"] = status.dashboardPort;
    dashboard["refresh_interval_sec"] =
        status.dashboardRefreshInterval.count();
    dashboard["url"] = canonicalDashboardUrl(
        status.dashboardHost, status.dashboardPort);

    Json value = Json::object();
    value["auto_restart"] = status.autoRestart;
    value["dashboard"] = std::move(dashboard);
    value["desired_running"] = status.desiredRunning;
    value["home"] = status.home.value();
    value["http_listening"] = status.httpListening;
    value["last_error"] = nullptr;
    if (status.lastError) value["last_error"] = *status.lastError;
    value["manager"] = status.isManager;
    value["ok"] = status.ok;
    value["open_browser_on_start"] = status.openBrowserOnStart;
    value["pid"] = status.processId;
    value["restart_count"] = status.restartCount;
    value["service_active"] = status.serviceActive;
    value["started_at"] = nullptr;
    if (status.startedAt) value["started_at"] = timestampText(*status.startedAt);
    value["state"] = serviceStateName(status.state);
    value["uptime_sec"] = nullptr;
    if (status.uptime) value["uptime_sec"] = status.uptime->count();
    value["version"] = status.version;
    value["watchdog_interval_sec"] = status.watchdogInterval.count();
    return value;
}

[[nodiscard]] Json settingsJson(const Domain::ManagerSettings& settings)
{
    validateSettings(settings);
    Json dashboard = Json::object();
    dashboard["host"] = settings.dashboardHost;
    dashboard["port"] = settings.dashboardPort;
    dashboard["refresh_interval_sec"] =
        settings.dashboardRefreshInterval.count();

    Json manager = Json::object();
    manager["auto_restart"] = settings.autoRestart;
    manager["open_browser_on_start"] = settings.openBrowserOnStart;
    manager["watchdog_interval_sec"] = settings.watchdogInterval.count();

    Json sessions = Json::object();
    sessions["idle_ttl_sec"] = settings.sessionIdleTtl.count();

    Json shell = Json::object();
    shell["default_timeout_sec"] = settings.shellTimeout.count();

    Json value = Json::object();
    value["dashboard"] = std::move(dashboard);
    value["log_level"] = logLevelName(settings.logLevel);
    value["manager"] = std::move(manager);
    value["ok"] = true;
    value["sessions"] = std::move(sessions);
    value["shell"] = std::move(shell);
    return value;
}

void validateOutcome(const Domain::ManagerSettingsUpdateOutcome& outcome)
{
    validateSettings(outcome.settings);
    validateStatus(outcome.status);
    if (outcome.settings.dashboardHost != outcome.status.dashboardHost ||
        outcome.settings.dashboardPort != outcome.status.dashboardPort ||
        outcome.settings.dashboardRefreshInterval !=
            outcome.status.dashboardRefreshInterval ||
        outcome.settings.autoRestart != outcome.status.autoRestart ||
        outcome.settings.watchdogInterval != outcome.status.watchdogInterval ||
        outcome.settings.openBrowserOnStart !=
            outcome.status.openBrowserOnStart) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager settings update outcome disagrees with its status projection.");
    }
}

[[nodiscard]] Domain::ManagerSettingsPatch parsePatch(const Json& value)
{
    requireAllowedFields(
        value,
        {"dashboard", "manager", "sessions", "shell", "log_level"},
        "Manager settings patch");

    Domain::ManagerSettingsPatch patch;
    if (const auto dashboard = value.find("dashboard"); dashboard != value.end()) {
        requireAllowedFields(
            *dashboard,
            {"host", "port", "refresh_interval_sec"},
            "dashboard patch");
        if (dashboard->contains("host")) {
            patch.dashboardHost = stringValue(*dashboard, "host");
        }
        if (dashboard->contains("port")) {
            patch.dashboardPort = portValue(*dashboard, "port");
        }
        if (dashboard->contains("refresh_interval_sec")) {
            patch.dashboardRefreshInterval = positiveSeconds(
                *dashboard, "refresh_interval_sec");
        }
    }
    if (const auto manager = value.find("manager"); manager != value.end()) {
        requireAllowedFields(
            *manager,
            {"auto_restart", "watchdog_interval_sec", "open_browser_on_start"},
            "manager patch");
        if (manager->contains("auto_restart")) {
            patch.autoRestart = booleanValue(*manager, "auto_restart");
        }
        if (manager->contains("watchdog_interval_sec")) {
            patch.watchdogInterval = positiveSeconds(
                *manager, "watchdog_interval_sec");
        }
        if (manager->contains("open_browser_on_start")) {
            patch.openBrowserOnStart = booleanValue(
                *manager, "open_browser_on_start");
        }
    }
    if (const auto sessions = value.find("sessions"); sessions != value.end()) {
        requireAllowedFields(
            *sessions, {"idle_ttl_sec"}, "sessions patch");
        if (sessions->contains("idle_ttl_sec")) {
            patch.sessionIdleTtl = positiveSeconds(
                *sessions, "idle_ttl_sec");
        }
    }
    if (const auto shell = value.find("shell"); shell != value.end()) {
        requireAllowedFields(
            *shell, {"default_timeout_sec"}, "shell patch");
        if (shell->contains("default_timeout_sec")) {
            patch.shellTimeout = positiveSeconds(
                *shell, "default_timeout_sec");
        }
    }
    if (value.contains("log_level")) {
        patch.logLevel = parseLogLevel(stringValue(value, "log_level"));
    }

    auto valid = Domain::applyManagerSettingsPatch(
        Domain::ManagerSettings{}, patch);
    if (!valid) reject(valid.error().code, valid.error().message);
    return patch;
}

[[nodiscard]] DashboardManagerSettingsMutation mutationFromJson(
    const Json& root)
{
    requireObject(root, "Manager settings mutation");
    const bool nested = root.contains("settings");
    if (nested) {
        requireAllowedFields(
            root, {"apply", "settings"}, "Manager settings mutation");
    } else {
        requireAllowedFields(
            root,
            {"apply", "dashboard", "manager", "sessions", "shell", "log_level"},
            "Manager settings mutation");
    }

    const bool apply = root.contains("apply")
        ? booleanValue(root, "apply")
        : true;
    if (nested) {
        return DashboardManagerSettingsMutation{
            parsePatch(member(root, "settings")), apply};
    }

    Json patch = root;
    patch.erase("apply");
    return DashboardManagerSettingsMutation{parsePatch(patch), apply};
}

[[nodiscard]] Domain::Error unexpectedError(const std::string_view operation)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string{"Dashboard manager JSON codec failed while "} +
            std::string{operation} + '.');
}

} // namespace

Domain::Result<std::vector<std::byte>> DashboardManagerJsonCodec::encodeStatus(
    const Domain::ManagerStatus& status,
    const std::size_t maximumBytes) noexcept
{
    try {
        validateMaximum(
            maximumBytes,
            MaximumResponseBytes,
            "Dashboard manager response");
        validateStatus(status);
        preflightStatusResponse(status, maximumBytes);
        return Domain::Result<std::vector<std::byte>>::success(
            boundedSerialize(statusJson(status), maximumBytes));
    } catch (const DashboardCodecException& error) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(
            unexpectedError("encoding manager status"));
    }
}

Domain::Result<std::vector<std::byte>> DashboardManagerJsonCodec::encodeSettings(
    const Domain::ManagerSettings& settings,
    const std::size_t maximumBytes) noexcept
{
    try {
        validateMaximum(
            maximumBytes,
            MaximumResponseBytes,
            "Dashboard manager response");
        validateSettings(settings);
        preflightSettingsResponse(settings, maximumBytes);
        return Domain::Result<std::vector<std::byte>>::success(
            boundedSerialize(settingsJson(settings), maximumBytes));
    } catch (const DashboardCodecException& error) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(
            unexpectedError("encoding manager settings"));
    }
}

Domain::Result<std::vector<std::byte>>
DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
    const Domain::ManagerSettingsUpdateOutcome& outcome,
    const std::size_t maximumBytes) noexcept
{
    try {
        validateMaximum(
            maximumBytes,
            MaximumResponseBytes,
            "Dashboard manager response");
        validateOutcome(outcome);
        preflightSettingsUpdateResponse(outcome, maximumBytes);
        Json value = settingsJson(outcome.settings);
        value["applied"] = outcome.applied;
        value["bind_changed"] = outcome.bindingChanged;
        value["status"] = statusJson(outcome.status);
        return Domain::Result<std::vector<std::byte>>::success(
            boundedSerialize(value, maximumBytes));
    } catch (const DashboardCodecException& error) {
        return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(
            unexpectedError("encoding a manager settings update"));
    }
}

Domain::Result<DashboardManagerSettingsMutation>
DashboardManagerJsonCodec::decodeSettingsMutation(
    const std::span<const std::byte> body,
    const std::size_t maximumBytes) noexcept
{
    try {
        return Domain::Result<DashboardManagerSettingsMutation>::success(
            mutationFromJson(parseMutation(body, maximumBytes)));
    } catch (const DashboardCodecException& error) {
        return Domain::Result<DashboardManagerSettingsMutation>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<DashboardManagerSettingsMutation>::failure(
            unexpectedError("decoding a manager settings mutation"));
    }
}

} // namespace ForgeConductor::Dashboard
