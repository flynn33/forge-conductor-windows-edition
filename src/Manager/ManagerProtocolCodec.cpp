#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::Manager {
namespace {

using Json = nlohmann::json;

class ProtocolCodecException final : public std::runtime_error {
public:
    ProtocolCodecException(std::string code, std::string message)
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
    throw ProtocolCodecException{std::string{code}, std::string{message}};
}

void validateMaximumFrameBytes(const std::size_t maximumFrameBytes)
{
    if (maximumFrameBytes == 0U ||
        maximumFrameBytes > (std::numeric_limits<std::uint32_t>::max)()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager protocol maximum frame bytes must fit the nonzero 32-bit length prefix.");
    }
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
                (static_cast<unsigned char>(value[index + offset]) & 0xc0U) == 0x80U;
        };

        if (first >= 0xc2U && first <= 0xdfU) {
            if (!continuation(1U)) return false;
            index += 2U;
            continue;
        }
        if (first == 0xe0U) {
            if (index + 2U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0xa0U || second > 0xbfU || !continuation(2U)) return false;
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
            if (second < 0x80U || second > 0x9fU || !continuation(2U)) return false;
            index += 3U;
            continue;
        }
        if (first == 0xf0U) {
            if (index + 3U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x90U || second > 0xbfU ||
                !continuation(2U) || !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first >= 0xf1U && first <= 0xf3U) {
            if (!continuation(1U) || !continuation(2U) || !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        if (first == 0xf4U) {
            if (index + 3U >= value.size()) return false;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x8fU ||
                !continuation(2U) || !continuation(3U)) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

class StrictJsonSax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return scalar(); }
    bool boolean(bool) override { return scalar(); }
    bool number_integer(number_integer_t) override { return scalar(); }
    bool number_unsigned(number_unsigned_t) override { return scalar(); }
    bool number_float(number_float_t, const string_t&) override { return scalar(); }

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
        if (containers_.empty() || !containers_.back().object ||
            !containers_.back().keys.insert(name).second) {
            duplicateKey_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override
    {
        if (containers_.empty() || !containers_.back().object) return false;
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
        if (containers_.empty() || containers_.back().object) return false;
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
        bool object{};
        std::unordered_set<std::string> keys;
    };

    [[nodiscard]] bool scalar() const noexcept { return true; }

    [[nodiscard]] bool startContainer() noexcept
    {
        ++depth_;
        if (depth_ > ManagerProtocolCodec::MaximumJsonNesting) {
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

void validateJsonStringsAndNesting(const Json& value, const std::size_t depth)
{
    if (depth > ManagerProtocolCodec::MaximumJsonNesting) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            "Manager protocol JSON exceeds 64 nested containers.");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.find('\0') != std::string::npos || !isValidUtf8(text)) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Manager protocol model contains invalid UTF-8 or an embedded NUL.");
        }
        return;
    }
    if (value.is_object()) {
        for (const auto& [name, child] : value.items()) {
            if (name.find('\0') != std::string::npos || !isValidUtf8(name)) {
                reject(
                    Domain::ErrorCodes::InvalidRequest,
                    "Manager protocol model contains an invalid JSON member name.");
            }
            validateJsonStringsAndNesting(child, depth + 1U);
        }
    } else if (value.is_array()) {
        for (const auto& child : value) {
            validateJsonStringsAndNesting(child, depth + 1U);
        }
    }
}

[[nodiscard]] std::uint32_t frameLength(
    const std::span<const std::byte> frame)
{
    if (frame.size() < 4U) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol frame has an incomplete length prefix.");
    }
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[0])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(frame[3])) << 24U);
}

[[nodiscard]] std::string unframe(
    const std::span<const std::byte> frame,
    const std::size_t maximumFrameBytes)
{
    validateMaximumFrameBytes(maximumFrameBytes);
    const auto length = frameLength(frame);
    if (length == 0U) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol frame declares a zero-length payload.");
    }
    if (static_cast<std::size_t>(length) > maximumFrameBytes) {
        reject(
            Domain::ErrorCodes::PayloadTooLarge,
            "Manager protocol frame exceeds the configured payload limit.");
    }
    const auto expectedBytes = static_cast<std::size_t>(length) + 4U;
    if (frame.size() < expectedBytes) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol frame payload is incomplete.");
    }
    if (frame.size() > expectedBytes) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol frame contains trailing bytes.");
    }

    std::string payload(length, '\0');
    std::memcpy(payload.data(), frame.data() + 4U, length);
    if (payload.find('\0') != std::string::npos) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol JSON contains an embedded NUL.");
    }
    if (!isValidUtf8(payload)) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol payload is not strict UTF-8.");
    }
    return payload;
}

[[nodiscard]] Json parseFrame(
    const std::span<const std::byte> frame,
    const std::size_t maximumFrameBytes)
{
    const auto payload = unframe(frame, maximumFrameBytes);
    StrictJsonSax preflight;
    const bool accepted = Json::sax_parse(payload, &preflight);
    if (preflight.duplicateKey()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol JSON contains a duplicate object key.");
    }
    if (preflight.embeddedNul()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol JSON contains an embedded NUL string value.");
    }
    if (preflight.tooDeep()) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            "Manager protocol JSON exceeds 64 nested containers.");
    }
    if (!accepted || preflight.malformed()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol payload is malformed JSON.");
    }

    try {
        return Json::parse(payload, nullptr, true, false);
    } catch (const Json::exception&) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Manager protocol payload is malformed JSON.");
    }
}

[[nodiscard]] std::vector<std::byte> makeFrame(
    const Json& document,
    const std::size_t maximumFrameBytes)
{
    validateMaximumFrameBytes(maximumFrameBytes);
    validateJsonStringsAndNesting(document, 1U);

    std::string payload;
    try {
        payload = document.dump(-1, ' ', false, Json::error_handler_t::strict);
    } catch (const Json::exception&) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager protocol model cannot be encoded as strict UTF-8 JSON.");
    }
    if (payload.empty()) {
        reject(
            Domain::ErrorCodes::InternalFailure,
            "Manager protocol canonical encoder produced an empty payload.");
    }
    if (payload.size() > maximumFrameBytes) {
        reject(
            Domain::ErrorCodes::PayloadTooLarge,
            "Manager protocol payload exceeds the configured limit.");
    }

    const auto length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::byte> frame(payload.size() + 4U);
    frame[0] = static_cast<std::byte>(length & 0xffU);
    frame[1] = static_cast<std::byte>((length >> 8U) & 0xffU);
    frame[2] = static_cast<std::byte>((length >> 16U) & 0xffU);
    frame[3] = static_cast<std::byte>((length >> 24U) & 0xffU);
    std::memcpy(frame.data() + 4U, payload.data(), payload.size());
    return frame;
}

void requireObject(const Json& value, const std::string_view schema)
{
    if (!value.is_object()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{schema} + " must be a JSON object.");
    }
}

void requireExactFields(
    const Json& value,
    const std::initializer_list<std::string_view> fields,
    const std::string_view schema)
{
    requireObject(value, schema);
    if (value.size() != fields.size()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{schema} + " has missing or unknown fields.");
    }
    for (const auto field : fields) {
        if (value.find(std::string{field}) == value.end()) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{schema} + " has missing or unknown fields.");
        }
    }
}

[[nodiscard]] const Json& member(
    const Json& value,
    const std::string_view name)
{
    return value.at(std::string{name});
}

[[nodiscard]] const std::string& stringMember(
    const Json& value,
    const std::string_view name)
{
    const auto& field = member(value, name);
    if (!field.is_string()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a JSON string.");
    }
    return field.get_ref<const std::string&>();
}

[[nodiscard]] bool booleanMember(
    const Json& value,
    const std::string_view name)
{
    const auto& field = member(value, name);
    if (!field.is_boolean()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a JSON boolean.");
    }
    return field.get<bool>();
}

[[nodiscard]] std::int64_t integerMember(
    const Json& value,
    const std::string_view name)
{
    const auto& field = member(value, name);
    if (field.is_number_unsigned()) {
        const auto number = field.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         (std::numeric_limits<std::int64_t>::max)())) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{name} + " exceeds the signed 64-bit range.");
        }
        return static_cast<std::int64_t>(number);
    }
    if (!field.is_number_integer()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be an integer.");
    }
    return field.get<std::int64_t>();
}

[[nodiscard]] std::int64_t nonnegativeIntegerMember(
    const Json& value,
    const std::string_view name)
{
    const auto number = integerMember(value, name);
    if (number < 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must not be negative.");
    }
    return number;
}

[[nodiscard]] std::int64_t positiveIntegerMember(
    const Json& value,
    const std::string_view name)
{
    const auto number = integerMember(value, name);
    if (number <= 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be positive.");
    }
    return number;
}

[[nodiscard]] std::uint32_t uint32Member(
    const Json& value,
    const std::string_view name)
{
    const auto number = nonnegativeIntegerMember(value, name);
    if (static_cast<std::uint64_t>(number) >
        (std::numeric_limits<std::uint32_t>::max)()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " exceeds the unsigned 32-bit range.");
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] std::uint16_t uint16Member(
    const Json& value,
    const std::string_view name)
{
    const auto number = nonnegativeIntegerMember(value, name);
    if (static_cast<std::uint64_t>(number) >
        (std::numeric_limits<std::uint16_t>::max)()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " exceeds the unsigned 16-bit range.");
    }
    return static_cast<std::uint16_t>(number);
}

template <typename Identifier>
[[nodiscard]] Identifier identifierMember(
    const Json& value,
    const std::string_view name)
{
    auto parsed = Identifier::parse(stringMember(value, name));
    if (!parsed) {
        reject(parsed.error().code, parsed.error().message);
    }
    return std::move(parsed).value();
}

void validateVersion(const std::uint32_t version)
{
    if (version != ManagerProtocolVersion) {
        reject(
            Domain::ErrorCodes::UnsupportedVersion,
            "Manager protocol version is unsupported.");
    }
}

[[nodiscard]] std::chrono::seconds secondsFrom(
    const std::int64_t count,
    const std::string_view name)
{
    if (count < static_cast<std::int64_t>(
                    (std::numeric_limits<std::chrono::seconds::rep>::min)()) ||
        count > static_cast<std::int64_t>(
                    (std::numeric_limits<std::chrono::seconds::rep>::max)())) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " exceeds the supported duration range.");
    }
    return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(count)};
}

[[nodiscard]] std::int64_t epochMilliseconds(
    const Domain::UtcTimePoint value)
{
    const auto elapsed = value.time_since_epoch();
    if (elapsed < Domain::UtcTimePoint::duration::zero()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "started_at_utc_ms must not be negative.");
    }

    // Manager protocol timestamps have millisecond wire precision. Because
    // negative timestamps are rejected above, duration_cast's truncation toward
    // zero deterministically discards sub-millisecond ticks toward the Unix epoch.
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

    using MillisecondRep = std::chrono::milliseconds::rep;
    using ComparisonRep = std::common_type_t<MillisecondRep, std::int64_t>;
    if (static_cast<ComparisonRep>(milliseconds.count()) >
        static_cast<ComparisonRep>((std::numeric_limits<std::int64_t>::max)())) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "started_at_utc_ms exceeds the signed 64-bit wire range.");
    }
    return static_cast<std::int64_t>(milliseconds.count());
}

[[nodiscard]] Domain::UtcTimePoint utcTimePointFromMilliseconds(
    const std::int64_t count)
{
    using ClockDuration = Domain::UtcTimePoint::duration;
    using MillisecondsToClock = std::ratio_divide<
        std::chrono::milliseconds::period,
        ClockDuration::period>;
    static_assert(
        MillisecondsToClock::den == 1,
        "Manager protocol requires a system clock whose tick evenly divides one millisecond.");

    // Convert the clock maximum toward milliseconds first; this direction only
    // divides on the supported Windows clock. Proving count is within that bound
    // makes the subsequent milliseconds-to-clock multiplication representable.
    constexpr auto maximumMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ClockDuration::max());
    using MillisecondRep = std::chrono::milliseconds::rep;
    using ComparisonRep = std::common_type_t<MillisecondRep, std::int64_t>;
    if (static_cast<ComparisonRep>(count) >
        static_cast<ComparisonRep>(maximumMilliseconds.count())) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "started_at_utc_ms exceeds the supported timestamp range.");
    }

    const auto milliseconds = std::chrono::milliseconds{count};
    const auto elapsed =
        std::chrono::duration_cast<ClockDuration>(milliseconds);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed) !=
        milliseconds) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "started_at_utc_ms exceeds the supported timestamp range.");
    }
    return Domain::UtcTimePoint{elapsed};
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

[[nodiscard]] Domain::ManagerServiceState parseServiceState(
    const std::string_view value)
{
    if (value == "stopped") return Domain::ManagerServiceState::Stopped;
    if (value == "starting") return Domain::ManagerServiceState::Starting;
    if (value == "running") return Domain::ManagerServiceState::Running;
    if (value == "restarting") return Domain::ManagerServiceState::Restarting;
    if (value == "stopping") return Domain::ManagerServiceState::Stopping;
    if (value == "failed") return Domain::ManagerServiceState::Failed;
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager status contains an unknown service state.");
}

[[nodiscard]] std::string_view controlActionName(
    const Domain::ManagerControlAction action)
{
    switch (action) {
    case Domain::ManagerControlAction::Start: return "start";
    case Domain::ManagerControlAction::Stop: return "stop";
    case Domain::ManagerControlAction::Restart: return "restart";
    case Domain::ManagerControlAction::Repair: return "repair";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager control request contains an invalid action.");
}

[[nodiscard]] Domain::ManagerControlAction parseControlAction(
    const std::string_view value)
{
    if (value == "start") return Domain::ManagerControlAction::Start;
    if (value == "stop") return Domain::ManagerControlAction::Stop;
    if (value == "restart") return Domain::ManagerControlAction::Restart;
    if (value == "repair") return Domain::ManagerControlAction::Repair;
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager control request contains an unknown action.");
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
        "Manager settings contain an unknown log level.");
}

void validateSettings(const Domain::ManagerSettings& settings)
{
    auto validated = Domain::validateManagerSettings(settings);
    if (!validated) {
        reject(validated.error().code, validated.error().message);
    }
    static_cast<void>(logLevelName(settings.logLevel));
}

void validatePatch(const Domain::ManagerSettingsPatch& patch)
{
    auto validated = Domain::applyManagerSettingsPatch(
        Domain::ManagerSettings{}, patch);
    if (!validated) {
        reject(validated.error().code, validated.error().message);
    }
    if (patch.logLevel) static_cast<void>(logLevelName(*patch.logLevel));
}

[[nodiscard]] Json optionalString(const std::optional<std::string>& value)
{
    Json encoded;
    if (value) {
        encoded = *value;
    } else {
        encoded = nullptr;
    }
    return encoded;
}

template <typename Duration>
[[nodiscard]] Json optionalDuration(const std::optional<Duration>& value)
{
    Json encoded;
    if (value) {
        encoded = value->count();
    } else {
        encoded = nullptr;
    }
    return encoded;
}

[[nodiscard]] Json settingsJson(const Domain::ManagerSettings& settings)
{
    validateSettings(settings);
    Json value = Json::object();
    value["auto_restart"] = settings.autoRestart;
    value["dashboard_host"] = settings.dashboardHost;
    value["dashboard_port"] = settings.dashboardPort;
    value["dashboard_refresh_interval_seconds"] =
        settings.dashboardRefreshInterval.count();
    value["log_level"] = logLevelName(settings.logLevel);
    value["open_browser_on_start"] = settings.openBrowserOnStart;
    value["session_idle_ttl_seconds"] = settings.sessionIdleTtl.count();
    value["shell_timeout_seconds"] = settings.shellTimeout.count();
    value["watchdog_interval_seconds"] = settings.watchdogInterval.count();
    return value;
}

[[nodiscard]] Domain::ManagerSettings parseSettings(const Json& value)
{
    requireExactFields(
        value,
        {"auto_restart",
         "dashboard_host",
         "dashboard_port",
         "dashboard_refresh_interval_seconds",
         "log_level",
         "open_browser_on_start",
         "session_idle_ttl_seconds",
         "shell_timeout_seconds",
         "watchdog_interval_seconds"},
        "Manager settings");

    Domain::ManagerSettings settings;
    settings.dashboardHost = stringMember(value, "dashboard_host");
    settings.dashboardPort = uint16Member(value, "dashboard_port");
    settings.dashboardRefreshInterval = secondsFrom(
        positiveIntegerMember(value, "dashboard_refresh_interval_seconds"),
        "dashboard_refresh_interval_seconds");
    settings.autoRestart = booleanMember(value, "auto_restart");
    settings.watchdogInterval = secondsFrom(
        positiveIntegerMember(value, "watchdog_interval_seconds"),
        "watchdog_interval_seconds");
    settings.openBrowserOnStart =
        booleanMember(value, "open_browser_on_start");
    settings.sessionIdleTtl = secondsFrom(
        positiveIntegerMember(value, "session_idle_ttl_seconds"),
        "session_idle_ttl_seconds");
    settings.shellTimeout = secondsFrom(
        positiveIntegerMember(value, "shell_timeout_seconds"),
        "shell_timeout_seconds");
    settings.logLevel = parseLogLevel(stringMember(value, "log_level"));
    validateSettings(settings);
    return settings;
}

[[nodiscard]] Json patchJson(const Domain::ManagerSettingsPatch& patch)
{
    validatePatch(patch);
    Json value = Json::object();
    value["auto_restart"] = nullptr;
    if (patch.autoRestart) value["auto_restart"] = *patch.autoRestart;
    value["dashboard_host"] = optionalString(patch.dashboardHost);
    value["dashboard_port"] = nullptr;
    if (patch.dashboardPort) value["dashboard_port"] = *patch.dashboardPort;
    value["dashboard_refresh_interval_seconds"] =
        optionalDuration(patch.dashboardRefreshInterval);
    value["log_level"] = nullptr;
    if (patch.logLevel) value["log_level"] = logLevelName(*patch.logLevel);
    value["open_browser_on_start"] = nullptr;
    if (patch.openBrowserOnStart) {
        value["open_browser_on_start"] = *patch.openBrowserOnStart;
    }
    value["session_idle_ttl_seconds"] = optionalDuration(patch.sessionIdleTtl);
    value["shell_timeout_seconds"] = optionalDuration(patch.shellTimeout);
    value["watchdog_interval_seconds"] =
        optionalDuration(patch.watchdogInterval);
    return value;
}

template <typename Value, typename Parser>
[[nodiscard]] std::optional<Value> optionalField(
    const Json& object,
    const std::string_view name,
    Parser&& parser)
{
    if (member(object, name).is_null()) return std::nullopt;
    return std::optional<Value>{parser(object, name)};
}

[[nodiscard]] Domain::ManagerSettingsPatch parsePatch(const Json& value)
{
    requireExactFields(
        value,
        {"auto_restart",
         "dashboard_host",
         "dashboard_port",
         "dashboard_refresh_interval_seconds",
         "log_level",
         "open_browser_on_start",
         "session_idle_ttl_seconds",
         "shell_timeout_seconds",
         "watchdog_interval_seconds"},
        "Manager settings patch");

    Domain::ManagerSettingsPatch patch;
    patch.dashboardHost = optionalField<std::string>(
        value,
        "dashboard_host",
        [](const Json& object, const std::string_view name) {
            return std::string{stringMember(object, name)};
        });
    patch.dashboardPort = optionalField<std::uint16_t>(
        value, "dashboard_port", uint16Member);
    patch.dashboardRefreshInterval = optionalField<std::chrono::seconds>(
        value,
        "dashboard_refresh_interval_seconds",
        [](const Json& object, const std::string_view name) {
            return secondsFrom(positiveIntegerMember(object, name), name);
        });
    patch.autoRestart = optionalField<bool>(
        value, "auto_restart", booleanMember);
    patch.watchdogInterval = optionalField<std::chrono::seconds>(
        value,
        "watchdog_interval_seconds",
        [](const Json& object, const std::string_view name) {
            return secondsFrom(positiveIntegerMember(object, name), name);
        });
    patch.openBrowserOnStart = optionalField<bool>(
        value, "open_browser_on_start", booleanMember);
    patch.sessionIdleTtl = optionalField<std::chrono::seconds>(
        value,
        "session_idle_ttl_seconds",
        [](const Json& object, const std::string_view name) {
            return secondsFrom(positiveIntegerMember(object, name), name);
        });
    patch.shellTimeout = optionalField<std::chrono::seconds>(
        value,
        "shell_timeout_seconds",
        [](const Json& object, const std::string_view name) {
            return secondsFrom(positiveIntegerMember(object, name), name);
        });
    patch.logLevel = optionalField<Domain::LogLevel>(
        value,
        "log_level",
        [](const Json& object, const std::string_view name) {
            return parseLogLevel(stringMember(object, name));
        });
    validatePatch(patch);
    return patch;
}

void validateStatus(const Domain::ManagerStatus& status)
{
    static_cast<void>(serviceStateName(status.state));
    if (status.uptime && *status.uptime < std::chrono::seconds::zero()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager status uptime_seconds must not be negative.");
    }
    if (status.startedAt) static_cast<void>(epochMilliseconds(*status.startedAt));

    Domain::ManagerSettings projected;
    projected.dashboardHost = status.dashboardHost;
    projected.dashboardPort = status.dashboardPort;
    projected.dashboardRefreshInterval = status.dashboardRefreshInterval;
    projected.autoRestart = status.autoRestart;
    projected.watchdogInterval = status.watchdogInterval;
    projected.openBrowserOnStart = status.openBrowserOnStart;
    validateSettings(projected);
}

[[nodiscard]] Json statusJson(const Domain::ManagerStatus& status)
{
    validateStatus(status);
    Json value = Json::object();
    value["auto_restart"] = status.autoRestart;
    value["dashboard_host"] = status.dashboardHost;
    value["dashboard_port"] = status.dashboardPort;
    value["dashboard_refresh_interval_seconds"] =
        status.dashboardRefreshInterval.count();
    value["desired_running"] = status.desiredRunning;
    value["home"] = status.home.value();
    value["http_listening"] = status.httpListening;
    value["is_manager"] = status.isManager;
    value["last_error"] = optionalString(status.lastError);
    value["ok"] = status.ok;
    value["open_browser_on_start"] = status.openBrowserOnStart;
    value["process_id"] = status.processId;
    value["restart_count"] = status.restartCount;
    value["service_active"] = status.serviceActive;
    value["started_at_utc_ms"] = nullptr;
    if (status.startedAt) {
        value["started_at_utc_ms"] = epochMilliseconds(*status.startedAt);
    }
    value["state"] = serviceStateName(status.state);
    value["uptime_seconds"] = optionalDuration(status.uptime);
    value["version"] = status.version;
    value["watchdog_interval_seconds"] = status.watchdogInterval.count();
    return value;
}

[[nodiscard]] Domain::ManagerStatus parseStatus(const Json& value)
{
    requireExactFields(
        value,
        {"auto_restart",
         "dashboard_host",
         "dashboard_port",
         "dashboard_refresh_interval_seconds",
         "desired_running",
         "home",
         "http_listening",
         "is_manager",
         "last_error",
         "ok",
         "open_browser_on_start",
         "process_id",
         "restart_count",
         "service_active",
         "started_at_utc_ms",
         "state",
         "uptime_seconds",
         "version",
         "watchdog_interval_seconds"},
        "Manager status");

    auto home = Domain::PathText::create(stringMember(value, "home"));
    if (!home) reject(home.error().code, home.error().message);

    const auto startedAt = optionalField<Domain::UtcTimePoint>(
        value,
        "started_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(
                nonnegativeIntegerMember(object, name));
        });
    const auto uptime = optionalField<std::chrono::seconds>(
        value,
        "uptime_seconds",
        [](const Json& object, const std::string_view name) {
            return secondsFrom(nonnegativeIntegerMember(object, name), name);
        });
    const auto lastError = optionalField<std::string>(
        value,
        "last_error",
        [](const Json& object, const std::string_view name) {
            return std::string{stringMember(object, name)};
        });

    Domain::ManagerStatus status{
        booleanMember(value, "ok"),
        booleanMember(value, "is_manager"),
        parseServiceState(stringMember(value, "state")),
        booleanMember(value, "desired_running"),
        booleanMember(value, "http_listening"),
        booleanMember(value, "service_active"),
        uint32Member(value, "process_id"),
        startedAt,
        uptime,
        uint32Member(value, "restart_count"),
        lastError,
        booleanMember(value, "auto_restart"),
        secondsFrom(
            positiveIntegerMember(value, "watchdog_interval_seconds"),
            "watchdog_interval_seconds"),
        booleanMember(value, "open_browser_on_start"),
        stringMember(value, "dashboard_host"),
        uint16Member(value, "dashboard_port"),
        secondsFrom(
            positiveIntegerMember(value, "dashboard_refresh_interval_seconds"),
            "dashboard_refresh_interval_seconds"),
        std::move(home).value(),
        stringMember(value, "version")};
    validateStatus(status);
    return status;
}

[[nodiscard]] Json requestDocument(const ManagerRequest& request)
{
    validateVersion(request.version);
    if (request.deadlineUtcMilliseconds < 0) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "deadline_utc_ms must not be negative.");
    }

    Json params = Json::object();
    std::string_view method;
    std::visit(
        [&](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, ManagerStatusRequest>) {
                method = "manager.status";
            } else if constexpr (std::is_same_v<Payload, ManagerSettingsRequest>) {
                method = "manager.settings";
            } else if constexpr (
                std::is_same_v<Payload, Domain::ManagerControlRequest>) {
                method = "manager.control";
                params["action"] = controlActionName(payload.action);
            } else if constexpr (
                std::is_same_v<Payload, ManagerSettingsUpdateRequest>) {
                method = "manager.settings.update";
                params["apply_immediately"] = payload.applyImmediately;
                params["patch"] = patchJson(payload.patch);
            } else if constexpr (std::is_same_v<Payload, ManagerCancelRequest>) {
                method = "manager.cancel";
                params["operation_id"] = payload.operationId.value();
            } else if constexpr (std::is_same_v<Payload, ManagerShutdownRequest>) {
                method = "manager.shutdown";
            }
        },
        request.payload);

    Json root = Json::object();
    root["correlation_id"] = request.correlationId.value();
    root["deadline_utc_ms"] = request.deadlineUtcMilliseconds;
    root["method"] = method;
    root["nonce"] = request.nonce.value();
    root["params"] = std::move(params);
    root["request_id"] = request.requestId.value();
    root["version"] = request.version;
    return root;
}

[[nodiscard]] ManagerRequest parseRequestDocument(const Json& root)
{
    requireExactFields(
        root,
        {"correlation_id",
         "deadline_utc_ms",
         "method",
         "nonce",
         "params",
         "request_id",
         "version"},
        "Manager request");

    const auto version = uint32Member(root, "version");
    validateVersion(version);
    auto requestId = identifierMember<Domain::RequestId>(root, "request_id");
    auto correlationId =
        identifierMember<Domain::CorrelationId>(root, "correlation_id");
    const auto deadline = nonnegativeIntegerMember(root, "deadline_utc_ms");
    auto nonce = identifierMember<Domain::Sha256Digest>(root, "nonce");
    const auto& params = member(root, "params");
    const auto& method = stringMember(root, "method");

    ManagerRequestPayload payload;
    if (method == "manager.status") {
        requireExactFields(params, {}, "manager.status params");
        payload = ManagerStatusRequest{};
    } else if (method == "manager.settings") {
        requireExactFields(params, {}, "manager.settings params");
        payload = ManagerSettingsRequest{};
    } else if (method == "manager.control") {
        requireExactFields(params, {"action"}, "manager.control params");
        payload = Domain::ManagerControlRequest{
            parseControlAction(stringMember(params, "action"))};
    } else if (method == "manager.settings.update") {
        requireExactFields(
            params,
            {"apply_immediately", "patch"},
            "manager.settings.update params");
        payload = ManagerSettingsUpdateRequest{
            parsePatch(member(params, "patch")),
            booleanMember(params, "apply_immediately")};
    } else if (method == "manager.cancel") {
        requireExactFields(
            params, {"operation_id"}, "manager.cancel params");
        payload = ManagerCancelRequest{
            identifierMember<Domain::OperationId>(params, "operation_id")};
    } else if (method == "manager.shutdown") {
        requireExactFields(params, {}, "manager.shutdown params");
        payload = ManagerShutdownRequest{};
    } else {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager request method is unknown.");
    }

    return ManagerRequest{
        version,
        std::move(requestId),
        std::move(correlationId),
        deadline,
        std::move(nonce),
        std::move(payload)};
}

[[nodiscard]] Json errorJson(const Domain::Error& error)
{
    auto code = Domain::validateOpaqueIdentifier(error.code);
    if (!code) reject(code.error().code, code.error().message);
    if (error.evidenceId) {
        auto evidenceId = Domain::EvidenceId::parse(*error.evidenceId);
        if (!evidenceId) {
            reject(evidenceId.error().code, evidenceId.error().message);
        }
    }

    Json value = Json::object();
    value["code"] = error.code;
    value["evidence_id"] = optionalString(error.evidenceId);
    value["message"] = error.message;
    value["retryable"] = error.retryable;
    return value;
}

[[nodiscard]] Domain::Error parseError(const Json& value)
{
    requireExactFields(
        value,
        {"code", "evidence_id", "message", "retryable"},
        "Manager response error");
    auto code = Domain::validateOpaqueIdentifier(stringMember(value, "code"));
    if (!code) reject(code.error().code, code.error().message);

    const auto evidenceId = optionalField<std::string>(
        value,
        "evidence_id",
        [](const Json& object, const std::string_view name) {
            auto parsed = Domain::EvidenceId::parse(stringMember(object, name));
            if (!parsed) reject(parsed.error().code, parsed.error().message);
            return parsed.value().value();
        });
    return Domain::Error{
        std::move(code).value(),
        stringMember(value, "message"),
        booleanMember(value, "retryable"),
        evidenceId};
}

[[nodiscard]] Json resultJson(const ManagerResult& result)
{
    Json wrapper = Json::object();
    std::visit(
        [&](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Domain::ManagerStatus>) {
                wrapper["type"] = "status";
                wrapper["value"] = statusJson(value);
            } else if constexpr (std::is_same_v<Value, Domain::ManagerSettings>) {
                wrapper["type"] = "settings";
                wrapper["value"] = settingsJson(value);
            } else if constexpr (std::is_same_v<Value, ManagerAcknowledgement>) {
                wrapper["type"] = "acknowledgement";
                Json acknowledgement = Json::object();
                acknowledgement["acknowledged"] = value.acknowledged;
                wrapper["value"] = std::move(acknowledgement);
            }
        },
        result);
    return wrapper;
}

[[nodiscard]] ManagerResult parseResult(const Json& result)
{
    requireExactFields(result, {"type", "value"}, "Manager response result");
    const auto& type = stringMember(result, "type");
    const auto& value = member(result, "value");
    if (type == "status") {
        return ManagerResult{parseStatus(value)};
    }
    if (type == "settings") {
        return ManagerResult{parseSettings(value)};
    }
    if (type == "acknowledgement") {
        requireExactFields(
            value, {"acknowledged"}, "Manager acknowledgement");
        return ManagerResult{
            ManagerAcknowledgement{booleanMember(value, "acknowledged")}};
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager response result type is unknown.");
}

[[nodiscard]] Json responseDocument(const ManagerResponse& response)
{
    validateVersion(response.version);
    Json root = Json::object();
    root["correlation_id"] = response.correlationId.value();
    root["request_id"] = response.requestId.value();
    root["version"] = response.version;
    if (std::holds_alternative<ManagerResult>(response.body)) {
        root["result"] = resultJson(std::get<ManagerResult>(response.body));
    } else {
        root["error"] = errorJson(std::get<Domain::Error>(response.body));
    }
    return root;
}

[[nodiscard]] ManagerResponse parseResponseDocument(const Json& root)
{
    requireObject(root, "Manager response");
    const bool hasResult = root.find("result") != root.end();
    const bool hasError = root.find("error") != root.end();
    if (hasResult == hasError) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager response must contain exactly one of result or error.");
    }
    if (hasResult) {
        requireExactFields(
            root,
            {"correlation_id", "request_id", "result", "version"},
            "Manager response");
    } else {
        requireExactFields(
            root,
            {"correlation_id", "error", "request_id", "version"},
            "Manager response");
    }

    const auto version = uint32Member(root, "version");
    validateVersion(version);
    auto requestId = identifierMember<Domain::RequestId>(root, "request_id");
    auto correlationId =
        identifierMember<Domain::CorrelationId>(root, "correlation_id");
    ManagerResponseBody body = hasResult
        ? ManagerResponseBody{parseResult(member(root, "result"))}
        : ManagerResponseBody{parseError(member(root, "error"))};
    return ManagerResponse{
        version,
        std::move(requestId),
        std::move(correlationId),
        std::move(body)};
}

template <typename Value>
[[nodiscard]] Domain::Result<Value> codecFailure(
    const ProtocolCodecException& error)
{
    return Domain::Result<Value>::failure(
        Domain::makeError(error.code(), error.what()));
}

template <typename Value>
[[nodiscard]] Domain::Result<Value> unexpectedFailure(
    const std::string_view message)
{
    return Domain::Result<Value>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        std::string{message}));
}

} // namespace

Domain::Result<std::vector<std::byte>> ManagerProtocolCodec::encodeRequest(
    const ManagerRequest& request,
    const std::size_t maximumFrameBytes) noexcept
{
    try {
        return Domain::Result<std::vector<std::byte>>::success(
            makeFrame(requestDocument(request), maximumFrameBytes));
    } catch (const ProtocolCodecException& error) {
        return codecFailure<std::vector<std::byte>>(error);
    } catch (...) {
        return unexpectedFailure<std::vector<std::byte>>(
            "Manager protocol request encoding failed unexpectedly.");
    }
}

Domain::Result<ManagerRequest> ManagerProtocolCodec::decodeRequest(
    const std::span<const std::byte> frame,
    const std::size_t maximumFrameBytes) noexcept
{
    try {
        return Domain::Result<ManagerRequest>::success(
            parseRequestDocument(parseFrame(frame, maximumFrameBytes)));
    } catch (const ProtocolCodecException& error) {
        return codecFailure<ManagerRequest>(error);
    } catch (...) {
        return unexpectedFailure<ManagerRequest>(
            "Manager protocol request decoding failed unexpectedly.");
    }
}

Domain::Result<std::vector<std::byte>> ManagerProtocolCodec::encodeResponse(
    const ManagerResponse& response,
    const std::size_t maximumFrameBytes) noexcept
{
    try {
        return Domain::Result<std::vector<std::byte>>::success(
            makeFrame(responseDocument(response), maximumFrameBytes));
    } catch (const ProtocolCodecException& error) {
        return codecFailure<std::vector<std::byte>>(error);
    } catch (...) {
        return unexpectedFailure<std::vector<std::byte>>(
            "Manager protocol response encoding failed unexpectedly.");
    }
}

Domain::Result<ManagerResponse> ManagerProtocolCodec::decodeResponse(
    const std::span<const std::byte> frame,
    const std::size_t maximumFrameBytes) noexcept
{
    try {
        return Domain::Result<ManagerResponse>::success(
            parseResponseDocument(parseFrame(frame, maximumFrameBytes)));
    } catch (const ProtocolCodecException& error) {
        return codecFailure<ManagerResponse>(error);
    } catch (...) {
        return unexpectedFailure<ManagerResponse>(
            "Manager protocol response decoding failed unexpectedly.");
    }
}

} // namespace ForgeConductor::Manager
