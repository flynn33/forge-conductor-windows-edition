#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cmath>
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

[[nodiscard]] std::uint64_t uint64Member(
    const Json& value,
    const std::string_view name)
{
    const auto number = nonnegativeIntegerMember(value, name);
    return static_cast<std::uint64_t>(number);
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

[[nodiscard]] std::vector<std::string> stringArray(
    const Json& value,
    const std::string_view schema)
{
    if (!value.is_array()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{schema} + " must be a JSON array.");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                std::string{schema} + " entries must be JSON strings.");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
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

[[nodiscard]] std::string_view managedRunStateName(
    const Domain::ManagedRunState state)
{
    switch (state) {
    case Domain::ManagedRunState::Running: return "running";
    case Domain::ManagedRunState::Cancelling: return "cancelling";
    case Domain::ManagedRunState::Completed: return "completed";
    case Domain::ManagedRunState::Failed: return "failed";
    case Domain::ManagedRunState::Cancelled: return "cancelled";
    case Domain::ManagedRunState::Paused: return "paused";
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Managed run snapshot contains an invalid state.");
}

[[nodiscard]] Domain::ManagedRunState parseManagedRunState(
    const std::string_view value)
{
    if (value == "running") return Domain::ManagedRunState::Running;
    if (value == "cancelling") return Domain::ManagedRunState::Cancelling;
    if (value == "completed") return Domain::ManagedRunState::Completed;
    if (value == "failed") return Domain::ManagedRunState::Failed;
    if (value == "cancelled") return Domain::ManagedRunState::Cancelled;
    if (value == "paused") return Domain::ManagedRunState::Paused;
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Managed run snapshot contains an unknown state.");
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
    value["local_model_host"] = settings.localModelHost;
    value["local_model_port"] = settings.localModelPort;
    value["local_model_secure"] = settings.localModelSecure;
    value["local_model_name"] = settings.localModelName;
    value["effective_context_capacity"] = settings.effectiveContextCapacity;
    value["next_response_reserve"] = settings.nextResponseReserve;
    value["handoff_reserve"] = settings.handoffReserve;
    value["estimation_safety_margin"] = settings.estimationSafetyMargin;
    value["open_browser_on_start"] = settings.openBrowserOnStart;
    value["session_idle_ttl_seconds"] = settings.sessionIdleTtl.count();
    value["shell_timeout_seconds"] = settings.shellTimeout.count();
    value["shell_enabled"] = settings.shellEnabled;
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
         "effective_context_capacity",
         "estimation_safety_margin",
         "handoff_reserve",
         "log_level",
         "local_model_host",
         "local_model_name",
         "local_model_port",
         "local_model_secure",
         "next_response_reserve",
         "open_browser_on_start",
         "session_idle_ttl_seconds",
         "shell_enabled",
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
    settings.shellEnabled = booleanMember(value, "shell_enabled");
    settings.logLevel = parseLogLevel(stringMember(value, "log_level"));
    settings.localModelHost = stringMember(value, "local_model_host");
    settings.localModelPort = uint16Member(value, "local_model_port");
    settings.localModelSecure = booleanMember(value, "local_model_secure");
    settings.localModelName = stringMember(value, "local_model_name");
    settings.effectiveContextCapacity =
        uint32Member(value, "effective_context_capacity");
    settings.nextResponseReserve = uint32Member(value, "next_response_reserve");
    settings.handoffReserve = uint32Member(value, "handoff_reserve");
    settings.estimationSafetyMargin =
        uint32Member(value, "estimation_safety_margin");
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
    value["local_model_host"] = optionalString(patch.localModelHost);
    value["local_model_name"] = optionalString(patch.localModelName);
    value["local_model_port"] = nullptr;
    if (patch.localModelPort) value["local_model_port"] = *patch.localModelPort;
    value["local_model_secure"] = nullptr;
    if (patch.localModelSecure) value["local_model_secure"] = *patch.localModelSecure;
    value["effective_context_capacity"] = nullptr;
    if (patch.effectiveContextCapacity) {
        value["effective_context_capacity"] = *patch.effectiveContextCapacity;
    }
    value["next_response_reserve"] = nullptr;
    if (patch.nextResponseReserve) {
        value["next_response_reserve"] = *patch.nextResponseReserve;
    }
    value["handoff_reserve"] = nullptr;
    if (patch.handoffReserve) value["handoff_reserve"] = *patch.handoffReserve;
    value["estimation_safety_margin"] = nullptr;
    if (patch.estimationSafetyMargin) {
        value["estimation_safety_margin"] = *patch.estimationSafetyMargin;
    }
    value["open_browser_on_start"] = nullptr;
    if (patch.openBrowserOnStart) {
        value["open_browser_on_start"] = *patch.openBrowserOnStart;
    }
    value["session_idle_ttl_seconds"] = optionalDuration(patch.sessionIdleTtl);
    value["shell_timeout_seconds"] = optionalDuration(patch.shellTimeout);
    value["shell_enabled"] = nullptr;
    if (patch.shellEnabled) value["shell_enabled"] = *patch.shellEnabled;
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
         "effective_context_capacity",
         "estimation_safety_margin",
         "handoff_reserve",
         "log_level",
         "local_model_host",
         "local_model_name",
         "local_model_port",
         "local_model_secure",
         "next_response_reserve",
         "open_browser_on_start",
         "session_idle_ttl_seconds",
         "shell_enabled",
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
    patch.shellEnabled = optionalField<bool>(
        value, "shell_enabled", booleanMember);
    patch.logLevel = optionalField<Domain::LogLevel>(
        value,
        "log_level",
        [](const Json& object, const std::string_view name) {
            return parseLogLevel(stringMember(object, name));
        });
    patch.localModelHost = optionalField<std::string>(
        value, "local_model_host",
        [](const Json& object, const std::string_view name) {
            return std::string{stringMember(object, name)};
        });
    patch.localModelName = optionalField<std::string>(
        value, "local_model_name",
        [](const Json& object, const std::string_view name) {
            return std::string{stringMember(object, name)};
        });
    patch.localModelPort = optionalField<std::uint16_t>(
        value, "local_model_port", uint16Member);
    patch.localModelSecure = optionalField<bool>(
        value, "local_model_secure", booleanMember);
    patch.effectiveContextCapacity = optionalField<std::uint32_t>(
        value, "effective_context_capacity", uint32Member);
    patch.nextResponseReserve = optionalField<std::uint32_t>(
        value, "next_response_reserve", uint32Member);
    patch.handoffReserve = optionalField<std::uint32_t>(
        value, "handoff_reserve", uint32Member);
    patch.estimationSafetyMargin = optionalField<std::uint32_t>(
        value, "estimation_safety_margin", uint32Member);
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

void validateSettingsUpdateOutcome(
    const Domain::ManagerSettingsUpdateOutcome& outcome)
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

[[nodiscard]] Json settingsUpdateOutcomeJson(
    const Domain::ManagerSettingsUpdateOutcome& outcome)
{
    validateSettingsUpdateOutcome(outcome);
    Json value = Json::object();
    value["applied"] = outcome.applied;
    value["binding_changed"] = outcome.bindingChanged;
    value["settings"] = settingsJson(outcome.settings);
    value["status"] = statusJson(outcome.status);
    return value;
}

[[nodiscard]] Domain::ManagerSettingsUpdateOutcome parseSettingsUpdateOutcome(
    const Json& value)
{
    requireExactFields(
        value,
        {"applied", "binding_changed", "settings", "status"},
        "Manager settings update outcome");
    Domain::ManagerSettingsUpdateOutcome outcome{
        parseSettings(member(value, "settings")),
        booleanMember(value, "applied"),
        booleanMember(value, "binding_changed"),
        parseStatus(member(value, "status"))};
    validateSettingsUpdateOutcome(outcome);
    return outcome;
}

[[nodiscard]] std::size_t sizeMember(
    const Json& value,
    std::string_view name);

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
            } else if constexpr (std::is_same_v<Payload, ManagerTelemetryRequest>) {
                method = "manager.telemetry";
                params["run_id"] = nullptr;
                if (payload.runId) {
                    params["run_id"] = payload.runId->value();
                }
            } else if constexpr (
                std::is_same_v<Payload, ManagerProjectsListRequest>) {
                method = "projects.list";
                params["maximum_count"] = payload.maximumCount;
            } else if constexpr (
                std::is_same_v<Payload, ManagerProjectInitializeRequest>) {
                method = "projects.initialize";
                params["project_path"] = payload.projectPath.value();
                params["display_name"] = payload.displayName
                    ? Json(*payload.displayName) : Json(nullptr);
                params["repository_identity"] = payload.repositoryIdentity
                    ? Json(*payload.repositoryIdentity) : Json(nullptr);
            } else if constexpr (
                std::is_same_v<Payload, ManagerProjectMemoryRequest>) {
                method = "projects.memory";
                params["project_id"] = payload.projectId.value();
                params["query"] = payload.query;
                params["maximum_count"] = payload.maximumCount;
            } else if constexpr (
                std::is_same_v<Payload, ManagerProjectRememberRequest>) {
                method = "projects.remember";
                params["project_id"] = payload.projectId.value();
                params["title"] = payload.title;
                params["summary"] = payload.summary;
                params["body"] = payload.body ? Json(*payload.body) : Json(nullptr);
                params["tags"] = payload.tags;
            } else if constexpr (
                std::is_same_v<Payload, ManagerLmStudioStatusRequest>) {
                method = "lmstudio.status";
            } else if constexpr (
                std::is_same_v<Payload, ManagerLmStudioRepairRequest>) {
                method = "lmstudio.repair";
            } else if constexpr (
                std::is_same_v<Payload, ManagerLmStudioActivateRequest>) {
                method = "lmstudio.activate";
            } else if constexpr (
                std::is_same_v<Payload, ManagerToolsRequest>) {
                method = "tools.list";
            } else if constexpr (
                std::is_same_v<Payload, ManagerToolInvokeRequest>) {
                method = "tools.invoke";
                params["project_id"] = payload.projectId.value();
                params["tool_name"] = payload.toolName;
                params["arguments"] = payload.canonicalArguments;
            } else if constexpr (
                std::is_same_v<Payload, ManagerOperationalRequest>) {
                method = "operations.page";
                switch (payload.area) {
                case ManagerOperationalArea::Agents: params["area"] = "agents"; break;
                case ManagerOperationalArea::Feed: params["area"] = "feed"; break;
                case ManagerOperationalArea::Runtimes: params["area"] = "runtimes"; break;
                case ManagerOperationalArea::Diagnostics: params["area"] = "diagnostics"; break;
                case ManagerOperationalArea::Manager: params["area"] = "manager"; break;
                case ManagerOperationalArea::Runs: params["area"] = "runs"; break;
                case ManagerOperationalArea::Evidence: params["area"] = "evidence"; break;
                }
                switch (payload.action) {
                case ManagerOperationalAction::Inspect: params["action"] = "inspect"; break;
                case ManagerOperationalAction::PruneSessions: params["action"] = "prune_sessions"; break;
                case ManagerOperationalAction::CloseSession: params["action"] = "close_session"; break;
                }
                params["session_id"] = payload.sessionId
                    ? Json(payload.sessionId->value()) : Json(nullptr);
                params["summary"] = payload.summary;
                params["project_id"] = payload.projectId
                    ? Json(payload.projectId->value()) : Json(nullptr);
            } else if constexpr (
                std::is_same_v<Payload, ManagerMaintenanceRequest>) {
                method = "maintenance.reset";
                switch (payload.scope) {
                case ManagerMaintenanceScope::ProjectMemory: params["scope"] = "project_memory"; break;
                case ManagerMaintenanceScope::ProjectContinuity: params["scope"] = "project_continuity"; break;
                case ManagerMaintenanceScope::ProjectAllData: params["scope"] = "project_all_data"; break;
                case ManagerMaintenanceScope::AllProjectsAllData: params["scope"] = "all_projects_all_data"; break;
                }
                params["project_id"] = payload.projectId
                    ? Json(payload.projectId->value()) : Json(nullptr);
                params["confirmation_token"] = payload.confirmationToken;
            } else if constexpr (
                std::is_same_v<Payload, Domain::ManagerControlRequest>) {
                method = "manager.control";
                params["action"] = controlActionName(payload.action);
            } else if constexpr (
                std::is_same_v<Payload, ManagerSettingsUpdateRequest>) {
                method = "manager.settings.update";
                params["apply_immediately"] = payload.applyImmediately;
                params["patch"] = patchJson(payload.patch);
            } else if constexpr (
                std::is_same_v<Payload, ManagedRunStartRequest>) {
                method = "managed_run.start";
                params["authority_generation"] = payload.authorityGeneration;
                params["allow_tools"] = payload.allowTools;
                params["client_id"] = payload.clientId.value();
                params["project_id"] = payload.projectId.value();
                params["run_id"] = payload.runId.value();
                params["task"] = payload.task;
            } else if constexpr (
                std::is_same_v<Payload, ManagedRunStatusRequest>) {
                method = "managed_run.status";
                params["run_id"] = payload.runId.value();
            } else if constexpr (
                std::is_same_v<Payload, ManagedRunCancelRequest>) {
                method = "managed_run.cancel";
                params["run_id"] = payload.runId.value();
            } else if constexpr (
                std::is_same_v<Payload, ManagedRunPauseRequest>) {
                method = "managed_run.pause";
                params["run_id"] = payload.runId.value();
            } else if constexpr (
                std::is_same_v<Payload, ManagedRunResumeRequest>) {
                method = "managed_run.resume";
                params["run_id"] = payload.runId.value();
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
    } else if (method == "manager.telemetry") {
        requireExactFields(params, {"run_id"}, "manager.telemetry params");
        payload = ManagerTelemetryRequest{
            optionalField<Domain::SessionId>(
                params,
                "run_id",
                [](const Json& object, const std::string_view name) {
                    return identifierMember<Domain::SessionId>(object, name);
                })};
    } else if (method == "projects.list") {
        requireExactFields(params, {"maximum_count"}, "projects.list params");
        payload = ManagerProjectsListRequest{
            sizeMember(params, "maximum_count")};
    } else if (method == "projects.initialize") {
        requireExactFields(
            params,
            {"display_name", "project_path", "repository_identity"},
            "projects.initialize params");
        auto path = Domain::PathText::create(stringMember(params, "project_path"));
        if (!path) reject(path.error().code, path.error().message);
        payload = ManagerProjectInitializeRequest{
            std::move(path).value(),
            optionalField<std::string>(params, "display_name", stringMember),
            optionalField<std::string>(
                params, "repository_identity", stringMember)};
    } else if (method == "projects.memory") {
        requireExactFields(
            params,
            {"maximum_count", "project_id", "query"},
            "projects.memory params");
        payload = ManagerProjectMemoryRequest{
            identifierMember<Domain::ProjectId>(params, "project_id"),
            stringMember(params, "query"),
            sizeMember(params, "maximum_count")};
    } else if (method == "projects.remember") {
        requireExactFields(
            params,
            {"body", "project_id", "summary", "tags", "title"},
            "projects.remember params");
        payload = ManagerProjectRememberRequest{
            identifierMember<Domain::ProjectId>(params, "project_id"),
            stringMember(params, "title"),
            stringMember(params, "summary"),
            optionalField<std::string>(params, "body", stringMember),
            stringArray(member(params, "tags"), "projects.remember tags")};
    } else if (method == "lmstudio.status") {
        requireExactFields(params, {}, "lmstudio.status params");
        payload = ManagerLmStudioStatusRequest{};
    } else if (method == "lmstudio.repair") {
        requireExactFields(params, {}, "lmstudio.repair params");
        payload = ManagerLmStudioRepairRequest{};
    } else if (method == "lmstudio.activate") {
        requireExactFields(params, {}, "lmstudio.activate params");
        payload = ManagerLmStudioActivateRequest{};
    } else if (method == "tools.list") {
        requireExactFields(params, {}, "tools.list params");
        payload = ManagerToolsRequest{};
    } else if (method == "tools.invoke") {
        requireExactFields(
            params,
            {"arguments", "project_id", "tool_name"},
            "tools.invoke params");
        payload = ManagerToolInvokeRequest{
            identifierMember<Domain::ProjectId>(params, "project_id"),
            stringMember(params, "tool_name"),
            stringMember(params, "arguments")};
    } else if (method == "operations.page") {
        requireExactFields(
            params, {"action", "area", "project_id", "session_id", "summary"},
            "operations.page params");
        const auto& areaText = stringMember(params, "area");
        ManagerOperationalArea area;
        if (areaText == "agents") area = ManagerOperationalArea::Agents;
        else if (areaText == "feed") area = ManagerOperationalArea::Feed;
        else if (areaText == "runtimes") area = ManagerOperationalArea::Runtimes;
        else if (areaText == "diagnostics") area = ManagerOperationalArea::Diagnostics;
        else if (areaText == "manager") area = ManagerOperationalArea::Manager;
        else if (areaText == "runs") area = ManagerOperationalArea::Runs;
        else if (areaText == "evidence") area = ManagerOperationalArea::Evidence;
        else reject(Domain::ErrorCodes::InvalidRequest, "Manager operational area is unknown.");
        const auto& actionText = stringMember(params, "action");
        ManagerOperationalAction action;
        if (actionText == "inspect") action = ManagerOperationalAction::Inspect;
        else if (actionText == "prune_sessions") action = ManagerOperationalAction::PruneSessions;
        else if (actionText == "close_session") action = ManagerOperationalAction::CloseSession;
        else reject(Domain::ErrorCodes::InvalidRequest, "Manager operational action is unknown.");
        payload = ManagerOperationalRequest{
            area,
            action,
            optionalField<Domain::SessionId>(
                params, "session_id",
                [](const Json& object, const std::string_view name) {
                    return identifierMember<Domain::SessionId>(object, name);
                }),
            stringMember(params, "summary"),
            optionalField<Domain::ProjectId>(
                params, "project_id",
                [](const Json& object, const std::string_view name) {
                    return identifierMember<Domain::ProjectId>(object, name);
                })};
    } else if (method == "maintenance.reset") {
        requireExactFields(
            params, {"confirmation_token", "project_id", "scope"},
            "maintenance.reset params");
        const auto& scopeText = stringMember(params, "scope");
        ManagerMaintenanceScope scope;
        if (scopeText == "project_memory") scope = ManagerMaintenanceScope::ProjectMemory;
        else if (scopeText == "project_continuity") scope = ManagerMaintenanceScope::ProjectContinuity;
        else if (scopeText == "project_all_data") scope = ManagerMaintenanceScope::ProjectAllData;
        else if (scopeText == "all_projects_all_data") scope = ManagerMaintenanceScope::AllProjectsAllData;
        else reject(Domain::ErrorCodes::InvalidRequest, "Manager maintenance scope is unknown.");
        payload = ManagerMaintenanceRequest{
            scope,
            optionalField<Domain::ProjectId>(
                params, "project_id",
                [](const Json& object, const std::string_view name) {
                    return identifierMember<Domain::ProjectId>(object, name);
                }),
            stringMember(params, "confirmation_token")};
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
    } else if (method == "managed_run.start") {
        if (params.contains("allow_tools")) {
            requireExactFields(
                params,
                {"allow_tools", "authority_generation", "client_id", "project_id",
                 "run_id", "task"},
                "managed_run.start params");
        } else {
            requireExactFields(
                params,
                {"authority_generation", "client_id", "project_id", "run_id", "task"},
                "managed_run.start params");
        }
        payload = ManagedRunStartRequest{
            identifierMember<Domain::SessionId>(params, "run_id"),
            identifierMember<Domain::ProjectId>(params, "project_id"),
            identifierMember<Domain::ClientId>(params, "client_id"),
            uint64Member(params, "authority_generation"),
            stringMember(params, "task"),
            params.contains("allow_tools") ? booleanMember(params, "allow_tools")
                                           : true};
    } else if (method == "managed_run.status") {
        requireExactFields(params, {"run_id"}, "managed_run.status params");
        payload = ManagedRunStatusRequest{
            identifierMember<Domain::SessionId>(params, "run_id")};
    } else if (method == "managed_run.cancel") {
        requireExactFields(params, {"run_id"}, "managed_run.cancel params");
        payload = ManagedRunCancelRequest{
            identifierMember<Domain::SessionId>(params, "run_id")};
    } else if (method == "managed_run.pause") {
        requireExactFields(params, {"run_id"}, "managed_run.pause params");
        payload = ManagedRunPauseRequest{
            identifierMember<Domain::SessionId>(params, "run_id")};
    } else if (method == "managed_run.resume") {
        requireExactFields(params, {"run_id"}, "managed_run.resume params");
        payload = ManagedRunResumeRequest{
            identifierMember<Domain::SessionId>(params, "run_id")};
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

[[nodiscard]] Json managedRunSnapshotJson(
    const Domain::ManagedRunSnapshot& snapshot)
{
    const auto& record = snapshot.record;
    Json value = Json::object();
    value["authority_generation"] = record.authorityGeneration;
    value["allow_tools"] = record.allowTools;
    value["cancellation_requested"] = snapshot.cancellationRequested;
    value["client_id"] = record.clientId.value();
    value["created_at_utc_ms"] = epochMilliseconds(record.createdAt);
    value["input_tokens"] = record.inputTokens;
    if (record.lastError) {
        value["last_error"] = errorJson(*record.lastError);
    } else {
        value["last_error"] = nullptr;
    }
    value["manager_owned"] = snapshot.managerOwned;
    value["pause_requested"] = snapshot.pauseRequested;
    value["output_text"] = optionalString(record.outputText);
    value["output_tokens"] = record.outputTokens;
    value["pending_function_calls"] = Json::array();
    for (const auto& call : record.pendingFunctionCalls) {
        value["pending_function_calls"].push_back(Json{
            {"arguments", call.canonicalArguments},
            {"call_id", call.callId},
            {"name", call.name}});
    }
    value["project_id"] = record.projectId.value();
    if (record.providerResponseId) {
        value["provider_response_id"] = record.providerResponseId->value();
    } else {
        value["provider_response_id"] = nullptr;
    }
    if (record.retainedContextTokens) {
        value["retained_context_tokens"] = *record.retainedContextTokens;
    } else {
        value["retained_context_tokens"] = nullptr;
    }
    value["run_id"] = record.runId.value();
    value["state"] = managedRunStateName(record.state);
    value["task"] = record.task;
    value["updated_at_utc_ms"] = epochMilliseconds(record.updatedAt);
    return value;
}

[[nodiscard]] Domain::ManagedRunSnapshot parseManagedRunSnapshot(
    const Json& value)
{
    if (value.contains("allow_tools")) {
        requireExactFields(
            value,
            {"allow_tools", "authority_generation", "cancellation_requested", "client_id",
             "created_at_utc_ms", "input_tokens", "last_error", "manager_owned",
             "output_text", "output_tokens", "pause_requested", "pending_function_calls",
             "project_id", "provider_response_id", "retained_context_tokens", "run_id",
             "state", "task", "updated_at_utc_ms"},
            "Managed run snapshot");
    } else {
        requireExactFields(
            value,
            {"authority_generation", "cancellation_requested", "client_id", "created_at_utc_ms",
             "input_tokens", "last_error", "manager_owned", "output_text",
             "output_tokens", "pause_requested", "pending_function_calls", "project_id", "provider_response_id",
             "retained_context_tokens", "run_id", "state", "task",
             "updated_at_utc_ms"},
            "Managed run snapshot");
    }
    const auto providerResponseId = optionalField<Domain::ProviderSessionId>(
        value,
        "provider_response_id",
        [](const Json& object, const std::string_view name) {
            return identifierMember<Domain::ProviderSessionId>(object, name);
        });
    const auto retainedContextTokens = optionalField<std::uint64_t>(
        value,
        "retained_context_tokens",
        [](const Json& object, const std::string_view name) {
            return uint64Member(object, name);
        });
    const auto outputText = optionalField<std::string>(
        value,
        "output_text",
        [](const Json& object, const std::string_view name) {
            return stringMember(object, name);
        });
    const auto lastError = optionalField<Domain::Error>(
        value,
        "last_error",
        [](const Json& object, const std::string_view name) {
            return parseError(member(object, name));
        });
    const auto& pendingJson = member(value, "pending_function_calls");
    if (!pendingJson.is_array()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Managed run pending_function_calls must be an array.");
    }
    std::vector<Domain::ManagedFunctionCall> pending;
    for (const auto& call : pendingJson) {
        requireExactFields(
            call, {"arguments", "call_id", "name"},
            "Managed function call");
        pending.push_back({
            stringMember(call, "call_id"),
            stringMember(call, "name"),
            stringMember(call, "arguments")});
    }
    return Domain::ManagedRunSnapshot{
        Domain::ManagedRunRecord{
            identifierMember<Domain::SessionId>(value, "run_id"),
            identifierMember<Domain::ProjectId>(value, "project_id"),
            identifierMember<Domain::ClientId>(value, "client_id"),
            stringMember(value, "task"),
            uint64Member(value, "authority_generation"),
            parseManagedRunState(stringMember(value, "state")),
            providerResponseId,
            uint64Member(value, "input_tokens"),
            uint64Member(value, "output_tokens"),
            retainedContextTokens,
            outputText,
            lastError,
            std::move(pending),
            utcTimePointFromMilliseconds(
                nonnegativeIntegerMember(value, "created_at_utc_ms")),
            utcTimePointFromMilliseconds(
                nonnegativeIntegerMember(value, "updated_at_utc_ms")),
            value.contains("allow_tools") ? booleanMember(value, "allow_tools")
                                           : true},
        booleanMember(value, "manager_owned"),
        booleanMember(value, "cancellation_requested"),
        booleanMember(value, "pause_requested")};
}

[[nodiscard]] double doubleMember(
    const Json& value,
    const std::string_view name)
{
    const auto& field = member(value, name);
    if (!field.is_number()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be a number.");
    }
    const auto number = field.get<double>();
    if (!std::isfinite(number)) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " must be finite.");
    }
    return number;
}

[[nodiscard]] std::size_t sizeMember(
    const Json& value,
    const std::string_view name)
{
    const auto number = uint64Member(value, name);
    if (number > (std::numeric_limits<std::size_t>::max)()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            std::string{name} + " exceeds the platform size range.");
    }
    return static_cast<std::size_t>(number);
}

[[nodiscard]] Domain::TelemetryMetricAvailability parseMetricAvailability(
    const std::string_view value)
{
    if (value == "available") {
        return Domain::TelemetryMetricAvailability::Available;
    }
    if (value == "warming_up") {
        return Domain::TelemetryMetricAvailability::WarmingUp;
    }
    if (value == "unsupported") {
        return Domain::TelemetryMetricAvailability::Unsupported;
    }
    if (value == "temporarily_unavailable") {
        return Domain::TelemetryMetricAvailability::TemporarilyUnavailable;
    }
    if (value == "access_denied") {
        return Domain::TelemetryMetricAvailability::AccessDenied;
    }
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Telemetry metric availability is unknown.");
}

template <typename T>
[[nodiscard]] Json telemetryMetricJson(const Domain::TelemetryMetric<T>& metric)
{
    const auto valid = Domain::validateTelemetryMetric(metric);
    if (!valid) {
        reject(valid.error().code, valid.error().message);
    }
    Json value = Json::object();
    value["availability"] = Domain::telemetryMetricAvailabilityName(
        metric.availability);
    value["captured_at_utc_ms"] = metric.capturedAt
        ? Json(epochMilliseconds(*metric.capturedAt))
        : Json(nullptr);
    value["observed_at_utc_ms"] = metric.observedAt
        ? Json(epochMilliseconds(*metric.observedAt))
        : Json(nullptr);
    value["source"] = metric.source;
    value["stale"] = metric.stale;
    value["unavailable_reason"] = optionalString(metric.unavailableReason);
    value["value"] = metric.value ? Json(*metric.value) : Json(nullptr);
    return value;
}

template <typename T, typename Parser>
[[nodiscard]] Domain::TelemetryMetric<T> parseTelemetryMetric(
    const Json& value,
    const std::string_view schema,
    Parser&& parser)
{
    requireExactFields(
        value,
        {"availability", "captured_at_utc_ms", "observed_at_utc_ms", "source",
         "stale", "unavailable_reason", "value"},
        schema);
    std::optional<T> parsedValue;
    if (!member(value, "value").is_null()) {
        parsedValue = parser(value, "value");
    }
    const auto capturedAt = optionalField<Domain::UtcTimePoint>(
        value,
        "captured_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(
                nonnegativeIntegerMember(object, name));
        });
    const auto observedAt = optionalField<Domain::UtcTimePoint>(
        value,
        "observed_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(
                nonnegativeIntegerMember(object, name));
        });
    const auto reason = optionalField<std::string>(
        value,
        "unavailable_reason",
        [](const Json& object, const std::string_view name) {
            return stringMember(object, name);
        });
    Domain::TelemetryMetric<T> metric{
        std::move(parsedValue),
        parseMetricAvailability(stringMember(value, "availability")),
        booleanMember(value, "stale"),
        capturedAt,
        observedAt,
        stringMember(value, "source"),
        reason};
    const auto valid = Domain::validateTelemetryMetric(metric);
    if (!valid) {
        reject(valid.error().code, valid.error().message);
    }
    return metric;
}

[[nodiscard]] std::string_view telemetryHealthName(
    const Domain::TelemetryHealth health)
{
    switch (health) {
    case Domain::TelemetryHealth::Ok: return "ok";
    case Domain::TelemetryHealth::Warn: return "warn";
    case Domain::TelemetryHealth::Error: return "error";
    case Domain::TelemetryHealth::Down: return "down";
    case Domain::TelemetryHealth::Config: return "config";
    }
    reject(Domain::ErrorCodes::InvalidRequest, "Telemetry health is unknown.");
}

[[nodiscard]] Domain::TelemetryHealth parseTelemetryHealth(
    const std::string_view value)
{
    if (value == "ok") return Domain::TelemetryHealth::Ok;
    if (value == "warn") return Domain::TelemetryHealth::Warn;
    if (value == "error") return Domain::TelemetryHealth::Error;
    if (value == "down") return Domain::TelemetryHealth::Down;
    if (value == "config") return Domain::TelemetryHealth::Config;
    reject(Domain::ErrorCodes::InvalidRequest, "Telemetry health is unknown.");
}

[[nodiscard]] std::string_view pressureName(
    const Domain::ResourcePressureLevel pressure)
{
    switch (pressure) {
    case Domain::ResourcePressureLevel::Nominal: return "nominal";
    case Domain::ResourcePressureLevel::Warning: return "warning";
    case Domain::ResourcePressureLevel::Critical: return "critical";
    }
    reject(Domain::ErrorCodes::InvalidRequest, "Resource pressure is unknown.");
}

[[nodiscard]] Domain::ResourcePressureLevel parsePressure(
    const std::string_view value)
{
    if (value == "nominal") return Domain::ResourcePressureLevel::Nominal;
    if (value == "warning") return Domain::ResourcePressureLevel::Warning;
    if (value == "critical") return Domain::ResourcePressureLevel::Critical;
    reject(Domain::ErrorCodes::InvalidRequest, "Resource pressure is unknown.");
}

[[nodiscard]] Json gpuJson(const Domain::GpuMetrics& gpu)
{
    Json engines = Json::array();
    for (const auto& engine : gpu.engines) {
        engines.push_back(Json{{"name", engine.name},
                               {"utilization_percent", engine.utilizationPercent}});
    }
    return Json{
        {"adapter_id", gpu.adapterId},
        {"captured_at_utc_ms", gpu.capturedAt
            ? Json(epochMilliseconds(*gpu.capturedAt)) : Json(nullptr)},
        {"dedicated_bytes_total", gpu.dedicatedBytesTotal},
        {"dedicated_bytes_used", gpu.dedicatedBytesUsed},
        {"direct3d_available", gpu.direct3dAvailable},
        {"engines", std::move(engines)},
        {"memory_scope", gpu.memoryScope},
        {"name", gpu.name},
        {"shared_bytes_used", gpu.sharedBytesUsed},
        {"utilization_percent", gpu.utilizationPercent},
        {"utilization_source", gpu.utilizationSource},
        {"vendor", gpu.vendor}};
}

[[nodiscard]] Domain::GpuMetrics parseGpu(const Json& value)
{
    requireExactFields(
        value,
        {"adapter_id", "captured_at_utc_ms", "dedicated_bytes_total",
         "dedicated_bytes_used", "direct3d_available", "engines", "memory_scope",
         "name", "shared_bytes_used", "utilization_percent", "utilization_source",
         "vendor"},
        "Manager GPU telemetry");
    const auto& engineValues = member(value, "engines");
    if (!engineValues.is_array() || engineValues.size() > 512U) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "Manager GPU engine telemetry exceeds its bound.");
    }
    std::vector<Domain::GpuEngineMetrics> engines;
    for (const auto& engine : engineValues) {
        requireExactFields(engine, {"name", "utilization_percent"},
                           "Manager GPU engine telemetry");
        engines.push_back(Domain::GpuEngineMetrics{
            stringMember(engine, "name"),
            doubleMember(engine, "utilization_percent")});
    }
    Domain::GpuMetrics result{
        stringMember(value, "vendor"),
        stringMember(value, "name"),
        optionalField<double>(value, "utilization_percent", doubleMember),
        optionalField<std::uint64_t>(value, "dedicated_bytes_used", uint64Member),
        optionalField<std::uint64_t>(value, "dedicated_bytes_total", uint64Member),
        optionalField<std::uint64_t>(value, "shared_bytes_used", uint64Member),
        booleanMember(value, "direct3d_available")};
    result.adapterId = stringMember(value, "adapter_id");
    result.engines = std::move(engines);
    result.capturedAt = optionalField<Domain::UtcTimePoint>(
        value, "captured_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(nonnegativeIntegerMember(object, name));
        });
    result.utilizationSource = stringMember(value, "utilization_source");
    result.memoryScope = stringMember(value, "memory_scope");
    return result;
}

[[nodiscard]] Json processJson(const Domain::ProcessMetrics& process)
{
    return Json{
        {"cpu_percent", process.cpuPercent},
        {"captured_at_utc_ms", process.capturedAt
            ? Json(epochMilliseconds(*process.capturedAt)) : Json(nullptr)},
        {"handle_count", process.handleCount},
        {"name", process.name},
        {"private_bytes", process.privateBytes},
        {"process_id", process.processId},
        {"source", process.source},
        {"thread_count", process.threadCount},
        {"working_set_bytes", process.workingSetBytes}};
}

[[nodiscard]] Domain::ProcessMetrics parseProcess(const Json& value)
{
    requireExactFields(
        value,
        {"captured_at_utc_ms", "cpu_percent", "handle_count", "name",
         "private_bytes", "process_id", "source", "thread_count",
         "working_set_bytes"},
        "Manager process telemetry");
    Domain::ProcessMetrics result{
        uint32Member(value, "process_id"),
        stringMember(value, "name"),
        doubleMember(value, "cpu_percent"),
        uint64Member(value, "working_set_bytes"),
        uint64Member(value, "private_bytes"),
        uint32Member(value, "thread_count"),
        uint32Member(value, "handle_count"),
        stringMember(value, "source")};
    result.capturedAt = optionalField<Domain::UtcTimePoint>(
        value, "captured_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(nonnegativeIntegerMember(object, name));
        });
    return result;
}

[[nodiscard]] Json historyJson(const Domain::HistoryPoint& point)
{
    return Json{
        {"cpu_percent", point.cpuPercent},
        {"disk_bytes_per_second", point.diskBytesPerSecond},
        {"gpu_percent", point.gpuPercent},
        {"mcp_events", point.mcpEvents},
        {"orchestration_health", telemetryHealthName(point.orchestrationHealth)},
        {"ram_percent", point.ramPercent},
        {"timestamp_utc_ms", epochMilliseconds(point.timestamp)}};
}

[[nodiscard]] Domain::HistoryPoint parseHistory(const Json& value)
{
    requireExactFields(
        value,
        {"cpu_percent", "disk_bytes_per_second", "gpu_percent", "mcp_events",
         "orchestration_health", "ram_percent", "timestamp_utc_ms"},
        "Manager telemetry history point");
    return Domain::HistoryPoint{
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "timestamp_utc_ms")),
        doubleMember(value, "cpu_percent"),
        doubleMember(value, "ram_percent"),
        optionalField<double>(value, "gpu_percent", doubleMember),
        doubleMember(value, "disk_bytes_per_second"),
        sizeMember(value, "mcp_events"),
        parseTelemetryHealth(stringMember(value, "orchestration_health"))};
}

[[nodiscard]] Json diskIoJson(const Domain::DiskIoMetrics& disk)
{
    return Json{
        {"read_bytes_per_second", disk.readBytesPerSecond},
        {"read_operations_per_second", disk.readOperationsPerSecond},
        {"write_bytes_per_second", disk.writeBytesPerSecond},
        {"write_operations_per_second", disk.writeOperationsPerSecond}};
}

[[nodiscard]] Domain::DiskIoMetrics parseDiskIo(
    const Json& value,
    const std::string_view name)
{
    const auto& disk = member(value, name);
    requireExactFields(
        disk,
        {"read_bytes_per_second", "read_operations_per_second",
         "write_bytes_per_second", "write_operations_per_second"},
        "Manager disk I/O telemetry");
    return Domain::DiskIoMetrics{
        doubleMember(disk, "read_bytes_per_second"),
        doubleMember(disk, "write_bytes_per_second"),
        doubleMember(disk, "read_operations_per_second"),
        doubleMember(disk, "write_operations_per_second")};
}

[[nodiscard]] Json diskIoMetricJson(
    const Domain::TelemetryMetric<Domain::DiskIoMetrics>& metric)
{
    const auto valid = Domain::validateTelemetryMetric(metric);
    if (!valid) reject(valid.error().code, valid.error().message);
    return Json{
        {"availability", Domain::telemetryMetricAvailabilityName(metric.availability)},
        {"captured_at_utc_ms", metric.capturedAt
            ? Json(epochMilliseconds(*metric.capturedAt)) : Json(nullptr)},
        {"observed_at_utc_ms", metric.observedAt
            ? Json(epochMilliseconds(*metric.observedAt)) : Json(nullptr)},
        {"source", metric.source},
        {"stale", metric.stale},
        {"unavailable_reason", optionalString(metric.unavailableReason)},
        {"value", metric.value ? diskIoJson(*metric.value) : Json(nullptr)}};
}

[[nodiscard]] Json volumeJson(const Domain::DiskVolume& volume)
{
    return Json{
        {"available_bytes", volume.availableBytes},
        {"captured_at_utc_ms", volume.capturedAt
            ? Json(epochMilliseconds(*volume.capturedAt)) : Json(nullptr)},
        {"device", volume.device},
        {"file_system", volume.fileSystem},
        {"mount", volume.mount.value()},
        {"percent", volume.percent},
        {"source", volume.source},
        {"total_bytes", volume.totalBytes},
        {"used_bytes", volume.usedBytes}};
}

[[nodiscard]] Domain::DiskVolume parseVolume(const Json& value)
{
    requireExactFields(
        value,
        {"available_bytes", "captured_at_utc_ms", "device", "file_system",
         "mount", "percent", "source", "total_bytes", "used_bytes"},
        "Manager volume telemetry");
    auto mount = Domain::PathText::create(stringMember(value, "mount"));
    if (!mount) reject(mount.error().code, mount.error().message);
    Domain::DiskVolume result{
        stringMember(value, "device"), std::move(mount).value(),
        stringMember(value, "file_system"), uint64Member(value, "total_bytes"),
        uint64Member(value, "used_bytes"), uint64Member(value, "available_bytes"),
        doubleMember(value, "percent")};
    result.capturedAt = optionalField<Domain::UtcTimePoint>(
        value, "captured_at_utc_ms",
        [](const Json& object, const std::string_view name) {
            return utcTimePointFromMilliseconds(nonnegativeIntegerMember(object, name));
        });
    result.source = stringMember(value, "source");
    return result;
}

[[nodiscard]] Json resourceSnapshotJson(
    const Domain::ManagerResourceSnapshot& snapshot)
{
    Json gpus = Json::array();
    for (const auto& gpu : snapshot.gpus) gpus.push_back(gpuJson(gpu));
    Json processes = Json::array();
    for (const auto& process : snapshot.processes) {
        processes.push_back(processJson(process));
    }
    Json history = Json::array();
    for (const auto& point : snapshot.history) {
        history.push_back(historyJson(point));
    }
    Json disks = Json::array();
    for (const auto& disk : snapshot.disks) disks.push_back(volumeJson(disk));
    return Json{
        {"architecture", snapshot.architecture},
        {"captured_at_utc_ms", epochMilliseconds(snapshot.capturedAt)},
        {"cpu_frequency_mhz", telemetryMetricJson(snapshot.cpuFrequencyMhz)},
        {"cpu_per_logical_frequency_mhz",
            telemetryMetricJson(snapshot.cpuPerLogicalFrequencyMhz)},
        {"cpu_per_logical_percent",
            telemetryMetricJson(snapshot.cpuPerLogicalProcessor)},
        {"cpu_percent", telemetryMetricJson(snapshot.cpuPercent)},
        {"disk_io", diskIoMetricJson(snapshot.diskIo)},
        {"disks", std::move(disks)},
        {"gpus", std::move(gpus)},
        {"history", std::move(history)},
        {"host", snapshot.host},
        {"platform", snapshot.platform},
        {"processes", std::move(processes)},
        {"ram_available_bytes", telemetryMetricJson(snapshot.ramAvailableBytes)},
        {"ram_percent", telemetryMetricJson(snapshot.ramPercent)},
        {"ram_total_bytes", telemetryMetricJson(snapshot.ramTotalBytes)},
        {"ram_used_bytes", telemetryMetricJson(snapshot.ramUsedBytes)},
        {"measured_sample_interval_ms", snapshot.measuredSampleIntervalMilliseconds},
        {"sampling_policy", snapshot.samplingPolicy},
        {"target_sample_interval_ms", snapshot.targetSampleIntervalMilliseconds}};
}

[[nodiscard]] Domain::ManagerResourceSnapshot parseResourceSnapshot(
    const Json& value)
{
    requireExactFields(
        value,
        {"architecture", "captured_at_utc_ms", "cpu_frequency_mhz",
         "cpu_per_logical_frequency_mhz", "cpu_per_logical_percent", "cpu_percent",
         "disk_io", "disks", "gpus", "history", "host",
         "measured_sample_interval_ms", "platform", "processes",
         "ram_available_bytes", "ram_percent", "ram_total_bytes", "ram_used_bytes",
         "sampling_policy", "target_sample_interval_ms"},
        "Manager resource telemetry");
    const auto parseDoubleMetric = [](const Json& metric, const std::string_view schema) {
        return parseTelemetryMetric<double>(metric, schema, doubleMember);
    };
    const auto parseUnsignedMetric = [](const Json& metric, const std::string_view schema) {
        return parseTelemetryMetric<std::uint64_t>(metric, schema, uint64Member);
    };
    const auto& gpuValues = member(value, "gpus");
    const auto& processValues = member(value, "processes");
    const auto& historyValues = member(value, "history");
    const auto& diskValues = member(value, "disks");
    if (!gpuValues.is_array() || !processValues.is_array() ||
        !historyValues.is_array() || !diskValues.is_array()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "Manager resource telemetry collections must be arrays.");
    }
    if (gpuValues.size() > 64U || processValues.size() > 4'096U ||
        historyValues.size() > 7'200U || diskValues.size() > 256U) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "Manager resource telemetry collection exceeds its bound.");
    }
    std::vector<Domain::GpuMetrics> gpus;
    for (const auto& item : gpuValues) gpus.push_back(parseGpu(item));
    std::vector<Domain::ProcessMetrics> processes;
    for (const auto& item : processValues) processes.push_back(parseProcess(item));
    std::vector<Domain::HistoryPoint> history;
    for (const auto& item : historyValues) history.push_back(parseHistory(item));
    std::vector<Domain::DiskVolume> disks;
    for (const auto& item : diskValues) disks.push_back(parseVolume(item));
    Domain::ManagerResourceSnapshot result{
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "captured_at_utc_ms")),
        stringMember(value, "host"),
        stringMember(value, "platform"),
        stringMember(value, "architecture"),
        parseDoubleMetric(member(value, "cpu_percent"), "Manager CPU metric"),
        parseDoubleMetric(member(value, "ram_percent"), "Manager RAM percent metric"),
        parseUnsignedMetric(member(value, "ram_used_bytes"), "Manager RAM used metric"),
        parseUnsignedMetric(member(value, "ram_total_bytes"), "Manager RAM total metric"),
        parseUnsignedMetric(member(value, "ram_available_bytes"), "Manager RAM available metric"),
        std::move(gpus),
        std::move(processes),
        std::move(history)};
    result.cpuPerLogicalProcessor = parseTelemetryMetric<std::vector<double>>(
        member(value, "cpu_per_logical_percent"),
        "Manager logical CPU metric",
        [](const Json& object, const std::string_view name) {
            const auto& values = member(object, name);
            if (!values.is_array() || values.size() > 256U) {
                reject(Domain::ErrorCodes::LimitExceeded,
                       "Manager logical CPU metric exceeds its bound.");
            }
            std::vector<double> result;
            for (const auto& item : values) {
                if (!item.is_number()) {
                    reject(Domain::ErrorCodes::InvalidRequest,
                           "Manager logical CPU metric contains a non-number.");
                }
                result.push_back(item.get<double>());
            }
            return result;
        });
    result.cpuFrequencyMhz = parseTelemetryMetric<std::uint32_t>(
        member(value, "cpu_frequency_mhz"), "Manager CPU frequency metric",
        uint32Member);
    result.cpuPerLogicalFrequencyMhz =
        parseTelemetryMetric<std::vector<std::uint32_t>>(
            member(value, "cpu_per_logical_frequency_mhz"),
            "Manager logical CPU frequency metric",
            [](const Json& object, const std::string_view name) {
                const auto& values = member(object, name);
                if (!values.is_array() || values.size() > 256U) {
                    reject(Domain::ErrorCodes::LimitExceeded,
                           "Manager logical CPU frequency metric exceeds its bound.");
                }
                std::vector<std::uint32_t> result;
                for (const auto& item : values) {
                    if (!item.is_number_unsigned() ||
                        item.get<std::uint64_t>() >
                            (std::numeric_limits<std::uint32_t>::max)()) {
                        reject(Domain::ErrorCodes::InvalidRequest,
                               "Manager logical CPU frequency metric is invalid.");
                    }
                    result.push_back(item.get<std::uint32_t>());
                }
                return result;
            });
    result.disks = std::move(disks);
    result.diskIo = parseTelemetryMetric<Domain::DiskIoMetrics>(
        member(value, "disk_io"), "Manager disk I/O metric", parseDiskIo);
    result.targetSampleIntervalMilliseconds =
        uint32Member(value, "target_sample_interval_ms");
    result.measuredSampleIntervalMilliseconds = optionalField<double>(
        value, "measured_sample_interval_ms", doubleMember);
    result.samplingPolicy = stringMember(value, "sampling_policy");
    return result;
}

[[nodiscard]] Json runtimeDiagnosticsJson(
    const Domain::RuntimeDiagnosticSnapshot& runtime)
{
    return Json{
        {"active_timers", runtime.activeTimers},
        {"background_threads", runtime.backgroundThreads},
        {"child_processes", runtime.childProcesses},
        {"open_databases", runtime.openDatabases},
        {"open_repositories", runtime.openRepositories},
        {"owned_operations", runtime.ownedOperations},
        {"pending_callbacks", runtime.pendingCallbacks},
        {"pressure", pressureName(runtime.pressure)},
        {"process_readers", runtime.processReaders},
        {"telemetry_pending_snapshots", runtime.telemetryPendingSnapshots},
        {"timestamp_utc_ms", epochMilliseconds(runtime.timestamp)}};
}

[[nodiscard]] Domain::RuntimeDiagnosticSnapshot parseRuntimeDiagnostics(
    const Json& value)
{
    requireExactFields(
        value,
        {"active_timers", "background_threads", "child_processes", "open_databases",
         "open_repositories", "owned_operations", "pending_callbacks", "pressure",
         "process_readers", "telemetry_pending_snapshots", "timestamp_utc_ms"},
        "Manager runtime diagnostics");
    return Domain::RuntimeDiagnosticSnapshot{
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "timestamp_utc_ms")),
        sizeMember(value, "owned_operations"),
        sizeMember(value, "pending_callbacks"),
        sizeMember(value, "background_threads"),
        sizeMember(value, "open_repositories"),
        sizeMember(value, "telemetry_pending_snapshots"),
        parsePressure(stringMember(value, "pressure")),
        sizeMember(value, "active_timers"),
        sizeMember(value, "child_processes"),
        sizeMember(value, "process_readers"),
        sizeMember(value, "open_databases")};
}

[[nodiscard]] Json auditEventJson(const Domain::AuditEvent& event)
{
    return Json{
        {"arguments_digest", event.argumentsDigest
             ? Json(event.argumentsDigest->value()) : Json(nullptr)},
        {"client_id", event.clientId
             ? Json(event.clientId->value()) : Json(nullptr)},
        {"duration_ms", event.duration
             ? Json(event.duration->count()) : Json(nullptr)},
        {"error", optionalString(event.error)},
        {"status", event.status},
        {"timestamp_utc_ms", epochMilliseconds(event.timestamp)},
        {"tool", event.tool}};
}

[[nodiscard]] Domain::AuditEvent parseAuditEvent(const Json& value)
{
    requireExactFields(
        value,
        {"arguments_digest", "client_id", "duration_ms", "error", "status",
         "timestamp_utc_ms", "tool"},
        "Manager telemetry audit event");
    return Domain::AuditEvent{
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "timestamp_utc_ms")),
        optionalField<Domain::ClientId>(value, "client_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::ClientId>(object, name);
            }),
        stringMember(value, "tool"),
        optionalField<Domain::Sha256Digest>(value, "arguments_digest",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::Sha256Digest>(object, name);
            }),
        stringMember(value, "status"),
        optionalField<std::chrono::milliseconds>(value, "duration_ms",
            [](const Json& object, const std::string_view name) {
                return std::chrono::milliseconds{
                    nonnegativeIntegerMember(object, name)};
            }),
        optionalField<std::string>(value, "error", stringMember)};
}

[[nodiscard]] Json managerTelemetrySnapshotJson(
    const Domain::ManagerTelemetrySnapshot& snapshot)
{
    if (snapshot.projects.size() > Domain::MaximumManagerTelemetryProjects ||
        snapshot.tools.size() > Domain::MaximumManagerTelemetryTools ||
        snapshot.recentEvents.size() > Domain::MaximumManagerTelemetryEvents) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "Manager operational telemetry exceeds its collection bound.");
    }
    Json projects = Json::array();
    for (const auto& project : snapshot.projects) {
        projects.push_back(project.value());
    }
    Json tools = snapshot.tools;
    Json events = Json::array();
    for (const auto& event : snapshot.recentEvents) {
        events.push_back(auditEventJson(event));
    }
    Json provider = Json{
        {"host", snapshot.provider.host},
        {"model", optionalString(snapshot.provider.model)},
        {"port", snapshot.provider.port},
        {"response_id", snapshot.provider.responseId
             ? Json(snapshot.provider.responseId->value()) : Json(nullptr)},
        {"secure", snapshot.provider.secure}};
    Json context = Json{
        {"authoritative", snapshot.context.authoritative},
        {"capacity_tokens", snapshot.context.capacityTokens},
        {"estimation_safety_margin_tokens", snapshot.context.estimationSafetyMarginTokens},
        {"handoff_reserve_tokens", snapshot.context.handoffReserveTokens},
        {"headroom_tokens", snapshot.context.headroomTokens},
        {"input_tokens", snapshot.context.inputTokens},
        {"next_response_reserve_tokens", snapshot.context.nextResponseReserveTokens},
        {"output_tokens", snapshot.context.outputTokens},
        {"retained_tokens", snapshot.context.retainedTokens}};
    Json continuity = Json{
        {"canonical_response_id", snapshot.continuity.canonicalResponseId
             ? Json(snapshot.continuity.canonicalResponseId->value()) : Json(nullptr)},
        {"context_only", snapshot.continuity.contextOnly},
        {"manager_owned", snapshot.continuity.managerOwned},
        {"project_id", snapshot.continuity.projectId
             ? Json(snapshot.continuity.projectId->value()) : Json(nullptr)},
        {"run_id", snapshot.continuity.runId
             ? Json(snapshot.continuity.runId->value()) : Json(nullptr)},
        {"run_state", snapshot.continuity.runState
             ? Json(std::string{managedRunStateName(*snapshot.continuity.runState)})
             : Json(nullptr)}};
    return Json{
        {"captured_at_utc_ms", epochMilliseconds(snapshot.capturedAt)},
        {"context", std::move(context)},
        {"continuity", std::move(continuity)},
        {"manager", statusJson(snapshot.manager)},
        {"open_session_count", snapshot.openSessionCount},
        {"presence_count", snapshot.presenceCount},
        {"projects", std::move(projects)},
        {"provider", std::move(provider)},
        {"recent_events", std::move(events)},
        {"recent_session_count", snapshot.recentSessionCount},
        {"resources", resourceSnapshotJson(snapshot.resources)},
        {"runtime", snapshot.runtime},
        {"runtime_diagnostics", snapshot.runtimeDiagnostics
             ? runtimeDiagnosticsJson(*snapshot.runtimeDiagnostics) : Json(nullptr)},
        {"selected_run", snapshot.selectedRun
             ? managedRunSnapshotJson(*snapshot.selectedRun) : Json(nullptr)},
        {"store_healthy", telemetryMetricJson(snapshot.storeHealthy)},
        {"tools", std::move(tools)}};
}

[[nodiscard]] Domain::ManagerTelemetrySnapshot parseManagerTelemetrySnapshot(
    const Json& value)
{
    requireExactFields(
        value,
        {"captured_at_utc_ms", "context", "continuity", "manager",
         "open_session_count", "presence_count", "projects", "provider",
         "recent_events", "recent_session_count", "resources", "runtime",
         "runtime_diagnostics", "selected_run", "store_healthy", "tools"},
        "Manager operational telemetry");

    const auto& providerValue = member(value, "provider");
    requireExactFields(
        providerValue, {"host", "model", "port", "response_id", "secure"},
        "Manager telemetry provider");
    Domain::ManagerProviderSnapshot provider{
        stringMember(providerValue, "host"),
        uint16Member(providerValue, "port"),
        booleanMember(providerValue, "secure"),
        optionalField<std::string>(providerValue, "model", stringMember),
        optionalField<Domain::ProviderSessionId>(providerValue, "response_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::ProviderSessionId>(object, name);
            })};

    const auto& contextValue = member(value, "context");
    requireExactFields(
        contextValue,
        {"authoritative", "capacity_tokens", "estimation_safety_margin_tokens",
         "handoff_reserve_tokens", "headroom_tokens", "input_tokens",
         "next_response_reserve_tokens", "output_tokens", "retained_tokens"},
        "Manager telemetry context");
    Domain::ManagerContextSnapshot context{
        uint64Member(contextValue, "capacity_tokens"),
        uint64Member(contextValue, "next_response_reserve_tokens"),
        uint64Member(contextValue, "handoff_reserve_tokens"),
        uint64Member(contextValue, "estimation_safety_margin_tokens"),
        optionalField<std::uint64_t>(contextValue, "input_tokens", uint64Member),
        optionalField<std::uint64_t>(contextValue, "output_tokens", uint64Member),
        optionalField<std::uint64_t>(contextValue, "retained_tokens", uint64Member),
        optionalField<std::uint64_t>(contextValue, "headroom_tokens", uint64Member),
        booleanMember(contextValue, "authoritative")};

    const auto& continuityValue = member(value, "continuity");
    requireExactFields(
        continuityValue,
        {"canonical_response_id", "context_only", "manager_owned", "project_id",
         "run_id", "run_state"},
        "Manager telemetry continuity");
    Domain::ManagerContinuitySnapshot continuity{
        booleanMember(continuityValue, "context_only"),
        booleanMember(continuityValue, "manager_owned"),
        optionalField<Domain::SessionId>(continuityValue, "run_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::SessionId>(object, name);
            }),
        optionalField<Domain::ProjectId>(continuityValue, "project_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::ProjectId>(object, name);
            }),
        optionalField<Domain::ManagedRunState>(continuityValue, "run_state",
            [](const Json& object, const std::string_view name) {
                return parseManagedRunState(stringMember(object, name));
            }),
        optionalField<Domain::ProviderSessionId>(continuityValue,
            "canonical_response_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::ProviderSessionId>(object, name);
            })};

    const auto& projectValues = member(value, "projects");
    const auto& toolValues = member(value, "tools");
    const auto& eventValues = member(value, "recent_events");
    if (!projectValues.is_array() || !toolValues.is_array() ||
        !eventValues.is_array()) {
        reject(Domain::ErrorCodes::InvalidRequest,
               "Manager operational telemetry collections must be arrays.");
    }
    if (projectValues.size() > Domain::MaximumManagerTelemetryProjects ||
        toolValues.size() > Domain::MaximumManagerTelemetryTools ||
        eventValues.size() > Domain::MaximumManagerTelemetryEvents) {
        reject(Domain::ErrorCodes::LimitExceeded,
               "Manager operational telemetry collection exceeds its bound.");
    }
    std::vector<Domain::ProjectId> projects;
    for (const auto& item : projectValues) {
        if (!item.is_string()) {
            reject(Domain::ErrorCodes::InvalidRequest,
                   "Manager telemetry project identifiers must be strings.");
        }
        auto project = Domain::ProjectId::parse(item.get<std::string>());
        if (!project) reject(project.error().code, project.error().message);
        projects.push_back(std::move(project).value());
    }
    std::vector<std::string> tools;
    for (const auto& item : toolValues) {
        if (!item.is_string()) {
            reject(Domain::ErrorCodes::InvalidRequest,
                   "Manager telemetry tool names must be strings.");
        }
        tools.push_back(item.get<std::string>());
    }
    std::vector<Domain::AuditEvent> events;
    for (const auto& item : eventValues) events.push_back(parseAuditEvent(item));

    auto runtimeDiagnostics = optionalField<Domain::RuntimeDiagnosticSnapshot>(
        value, "runtime_diagnostics",
        [](const Json& object, const std::string_view name) {
            return parseRuntimeDiagnostics(member(object, name));
        });
    auto selectedRun = optionalField<Domain::ManagedRunSnapshot>(
        value, "selected_run",
        [](const Json& object, const std::string_view name) {
            return parseManagedRunSnapshot(member(object, name));
        });
    return Domain::ManagerTelemetrySnapshot{
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "captured_at_utc_ms")),
        parseResourceSnapshot(member(value, "resources")),
        parseStatus(member(value, "manager")),
        std::move(runtimeDiagnostics),
        std::move(provider),
        std::move(context),
        std::move(continuity),
        std::move(selectedRun),
        std::move(projects),
        std::move(tools),
        sizeMember(value, "open_session_count"),
        sizeMember(value, "recent_session_count"),
        sizeMember(value, "presence_count"),
        std::move(events),
        parseTelemetryMetric<bool>(
            member(value, "store_healthy"),
            "Manager store health metric",
            booleanMember),
        stringMember(value, "runtime")};
}

[[nodiscard]] Json projectDescriptorJson(
    const Domain::ProjectMemoryDescriptor& project)
{
    Json aliases = Json::array();
    for (const auto& alias : project.aliases) aliases.push_back(alias.value());
    return Json{
        {"id", project.id.value()},
        {"display_name", project.displayName},
        {"repository_identity", project.repositoryIdentity
            ? Json(*project.repositoryIdentity) : Json(nullptr)},
        {"authorized_folders", std::move(aliases)}};
}

[[nodiscard]] Domain::ProjectMemoryDescriptor parseProjectDescriptor(
    const Json& value)
{
    requireExactFields(
        value,
        {"authorized_folders", "display_name", "id", "repository_identity"},
        "Manager project descriptor");
    std::vector<Domain::PathText> aliases;
    const auto& aliasValues = member(value, "authorized_folders");
    if (!aliasValues.is_array()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager project authorized_folders must be a JSON array.");
    }
    aliases.reserve(aliasValues.size());
    for (const auto& item : aliasValues) {
        if (!item.is_string()) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Manager project authorized folders must be strings.");
        }
        auto path = Domain::PathText::create(item.get<std::string>());
        if (!path) reject(path.error().code, path.error().message);
        aliases.push_back(std::move(path).value());
    }
    return Domain::ProjectMemoryDescriptor{
        identifierMember<Domain::ProjectId>(value, "id"),
        stringMember(value, "display_name"),
        optionalField<std::string>(
            value, "repository_identity", stringMember),
        std::move(aliases)};
}

[[nodiscard]] Json projectMemoryRecordJson(
    const ManagerProjectMemoryRecord& record)
{
    return Json{
        {"id", record.id.value()},
        {"version", record.version},
        {"kind", record.kind},
        {"title", record.title},
        {"summary", record.summary},
        {"body", record.body ? Json(*record.body) : Json(nullptr)},
        {"tags", record.tags},
        {"updated_at_utc_ms", epochMilliseconds(record.updatedAt)}};
}

[[nodiscard]] ManagerProjectMemoryRecord parseProjectMemoryRecord(
    const Json& value)
{
    requireExactFields(
        value,
        {"body", "id", "kind", "summary", "tags", "title",
         "updated_at_utc_ms", "version"},
        "Manager project memory record");
    return ManagerProjectMemoryRecord{
        identifierMember<Domain::MemoryRecordId>(value, "id"),
        uint32Member(value, "version"),
        stringMember(value, "kind"),
        stringMember(value, "title"),
        stringMember(value, "summary"),
        optionalField<std::string>(value, "body", stringMember),
        stringArray(member(value, "tags"), "Manager project memory tags"),
        utcTimePointFromMilliseconds(
            nonnegativeIntegerMember(value, "updated_at_utc_ms"))};
}

[[nodiscard]] Json projectsSnapshotJson(
    const ManagerProjectsSnapshot& snapshot)
{
    Json projects = Json::array();
    for (const auto& project : snapshot.projects) {
        projects.push_back(projectDescriptorJson(project));
    }
    return Json{{"projects", std::move(projects)}};
}

[[nodiscard]] ManagerProjectsSnapshot parseProjectsSnapshot(const Json& value)
{
    requireExactFields(value, {"projects"}, "Manager projects snapshot");
    const auto& projectValues = member(value, "projects");
    if (!projectValues.is_array()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager projects must be a JSON array.");
    }
    ManagerProjectsSnapshot snapshot;
    snapshot.projects.reserve(projectValues.size());
    for (const auto& project : projectValues) {
        snapshot.projects.push_back(parseProjectDescriptor(project));
    }
    return snapshot;
}

[[nodiscard]] Json projectWorkspaceSnapshotJson(
    const ManagerProjectWorkspaceSnapshot& snapshot)
{
    Json records = Json::array();
    for (const auto& record : snapshot.records) {
        records.push_back(projectMemoryRecordJson(record));
    }
    return Json{
        {"project", projectDescriptorJson(snapshot.project)},
        {"record_count", snapshot.recordCount},
        {"tombstone_count", snapshot.tombstoneCount},
        {"event_count", snapshot.eventCount},
        {"database_bytes", snapshot.databaseBytes},
        {"write_ahead_log_bytes", snapshot.writeAheadLogBytes},
        {"full_text_search_available", snapshot.fullTextSearchAvailable},
        {"integrity_ok", snapshot.integrityOk},
        {"records", std::move(records)},
        {"next_cursor", snapshot.nextCursor
            ? Json(*snapshot.nextCursor) : Json(nullptr)},
        {"truncated", snapshot.truncated},
        {"written_record_id", snapshot.writtenRecordId
            ? Json(snapshot.writtenRecordId->value()) : Json(nullptr)}};
}

[[nodiscard]] ManagerProjectWorkspaceSnapshot parseProjectWorkspaceSnapshot(
    const Json& value)
{
    requireExactFields(
        value,
        {"database_bytes", "event_count", "full_text_search_available",
         "integrity_ok", "next_cursor", "project", "record_count", "records",
         "tombstone_count", "truncated", "write_ahead_log_bytes",
         "written_record_id"},
        "Manager project workspace snapshot");
    const auto& recordValues = member(value, "records");
    if (!recordValues.is_array()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Manager project memory records must be a JSON array.");
    }
    std::vector<ManagerProjectMemoryRecord> records;
    records.reserve(recordValues.size());
    for (const auto& record : recordValues) {
        records.push_back(parseProjectMemoryRecord(record));
    }
    return ManagerProjectWorkspaceSnapshot{
        parseProjectDescriptor(member(value, "project")),
        sizeMember(value, "record_count"),
        sizeMember(value, "tombstone_count"),
        sizeMember(value, "event_count"),
        uint64Member(value, "database_bytes"),
        uint64Member(value, "write_ahead_log_bytes"),
        booleanMember(value, "full_text_search_available"),
        booleanMember(value, "integrity_ok"),
        std::move(records),
        optionalField<std::string>(value, "next_cursor", stringMember),
        booleanMember(value, "truncated"),
        optionalField<Domain::MemoryRecordId>(
            value, "written_record_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::MemoryRecordId>(object, name);
            })};
}

[[nodiscard]] std::string_view toolEffectName(
    const Domain::ToolEffect effect) noexcept
{
    switch (effect) {
    case Domain::ToolEffect::Read: return "read";
    case Domain::ToolEffect::Write: return "write";
    case Domain::ToolEffect::Execute: return "execute";
    case Domain::ToolEffect::Destructive: return "destructive";
    }
    return "read";
}

[[nodiscard]] Domain::ToolEffect parseToolEffect(const std::string_view value)
{
    if (value == "read") return Domain::ToolEffect::Read;
    if (value == "write") return Domain::ToolEffect::Write;
    if (value == "execute") return Domain::ToolEffect::Execute;
    if (value == "destructive") return Domain::ToolEffect::Destructive;
    reject(Domain::ErrorCodes::InvalidRequest, "Manager tool effect is unknown.");
}

[[nodiscard]] std::string_view toolAvailabilityName(
    const Domain::ToolAvailability availability) noexcept
{
    switch (availability) {
    case Domain::ToolAvailability::Available: return "available";
    case Domain::ToolAvailability::Disabled: return "disabled";
    case Domain::ToolAvailability::MissingDependency: return "missing_dependency";
    case Domain::ToolAvailability::Unhealthy: return "unhealthy";
    }
    return "unhealthy";
}

[[nodiscard]] Domain::ToolAvailability parseToolAvailability(
    const std::string_view value)
{
    if (value == "available") return Domain::ToolAvailability::Available;
    if (value == "disabled") return Domain::ToolAvailability::Disabled;
    if (value == "missing_dependency") {
        return Domain::ToolAvailability::MissingDependency;
    }
    if (value == "unhealthy") return Domain::ToolAvailability::Unhealthy;
    reject(
        Domain::ErrorCodes::InvalidRequest,
        "Manager tool availability is unknown.");
}

[[nodiscard]] Json lmStudioSnapshotJson(
    const ManagerLmStudioSnapshot& snapshot)
{
    return Json{
        {"lmstudio_present", snapshot.lmStudioPresent},
        {"primary_plugin_installed", snapshot.primaryPluginInstalled},
        {"fallback_plugin_installed", snapshot.fallbackPluginInstalled},
        {"continuity_plugin_installed", snapshot.continuityPluginInstalled},
        {"mcp_configuration_registered", snapshot.mcpConfigurationRegistered},
        {"binary_executable", snapshot.binaryExecutable},
        {"binary_path", snapshot.binaryPath},
        {"primary_plugin_path", snapshot.primaryPluginPath},
        {"fallback_plugin_path", snapshot.fallbackPluginPath},
        {"continuity_plugin_path", snapshot.continuityPluginPath},
        {"mcp_configuration_path", snapshot.mcpConfigurationPath},
        {"deployment_id", snapshot.deploymentId
            ? Json(snapshot.deploymentId->value()) : Json(nullptr)},
        {"connection_check_performed", snapshot.connectionCheckPerformed},
        {"primary_connector_ready", snapshot.primaryConnectorReady},
        {"fallback_connector_ready", snapshot.fallbackConnectorReady},
        {"continuity_connector_ready", snapshot.continuityConnectorReady},
        {"connected_client_observed", snapshot.connectedClientObserved},
        {"managed_continuity_projects", snapshot.managedContinuityProjects},
        {"detail", snapshot.detail},
        {"action_detail", snapshot.actionDetail},
        {"tool_audit_checked", snapshot.toolAuditChecked},
        {"primary_tool_outcome_recorded", snapshot.primaryToolOutcomeRecorded},
        {"fallback_tool_outcome_recorded", snapshot.fallbackToolOutcomeRecorded},
        {"continuity_tool_outcome_recorded", snapshot.continuityToolOutcomeRecorded},
        {"tool_audit_detail", snapshot.toolAuditDetail}};
}

[[nodiscard]] ManagerLmStudioSnapshot parseLmStudioSnapshot(const Json& value)
{
    requireExactFields(
        value,
        {"action_detail", "binary_executable", "binary_path",
         "connected_client_observed", "connection_check_performed", "deployment_id",
         "continuity_connector_ready", "continuity_plugin_installed",
         "continuity_plugin_path", "detail", "fallback_connector_ready", "fallback_plugin_installed",
         "fallback_plugin_path", "lmstudio_present", "managed_continuity_projects",
         "mcp_configuration_path", "mcp_configuration_registered",
         "primary_connector_ready", "primary_plugin_installed",
         "primary_plugin_path", "tool_audit_checked",
         "primary_tool_outcome_recorded", "fallback_tool_outcome_recorded",
         "continuity_tool_outcome_recorded", "tool_audit_detail"},
        "Manager LM Studio snapshot");
    return ManagerLmStudioSnapshot{
        booleanMember(value, "lmstudio_present"),
        booleanMember(value, "primary_plugin_installed"),
        booleanMember(value, "fallback_plugin_installed"),
        booleanMember(value, "continuity_plugin_installed"),
        booleanMember(value, "mcp_configuration_registered"),
        booleanMember(value, "binary_executable"),
        stringMember(value, "binary_path"),
        stringMember(value, "primary_plugin_path"),
        stringMember(value, "fallback_plugin_path"),
        stringMember(value, "continuity_plugin_path"),
        stringMember(value, "mcp_configuration_path"),
        optionalField<Domain::DeploymentId>(
            value, "deployment_id",
            [](const Json& object, const std::string_view name) {
                return identifierMember<Domain::DeploymentId>(object, name);
            }),
        booleanMember(value, "connection_check_performed"),
        booleanMember(value, "primary_connector_ready"),
        booleanMember(value, "fallback_connector_ready"),
        booleanMember(value, "continuity_connector_ready"),
        booleanMember(value, "connected_client_observed"),
        sizeMember(value, "managed_continuity_projects"),
        stringMember(value, "detail"),
        stringMember(value, "action_detail"),
        booleanMember(value, "tool_audit_checked"),
        booleanMember(value, "primary_tool_outcome_recorded"),
        booleanMember(value, "fallback_tool_outcome_recorded"),
        booleanMember(value, "continuity_tool_outcome_recorded"),
        stringMember(value, "tool_audit_detail")};
}

[[nodiscard]] Json toolDescriptorJson(const ManagerToolDescriptor& descriptor)
{
    return Json{
        {"name", descriptor.name},
        {"description", descriptor.description},
        {"pack", descriptor.pack},
        {"effect", toolEffectName(descriptor.effect)},
        {"availability", toolAvailabilityName(descriptor.availability)},
        {"requires_project", descriptor.requiresProject},
        {"requires_shell", descriptor.requiresShell},
        {"input_schema", descriptor.inputSchema}};
}

[[nodiscard]] ManagerToolDescriptor parseToolDescriptor(const Json& value)
{
    requireExactFields(
        value,
        {"availability", "description", "effect", "input_schema", "name", "pack",
         "requires_project", "requires_shell"},
        "Manager tool descriptor");
    return ManagerToolDescriptor{
        stringMember(value, "name"),
        stringMember(value, "description"),
        stringMember(value, "pack"),
        parseToolEffect(stringMember(value, "effect")),
        parseToolAvailability(stringMember(value, "availability")),
        booleanMember(value, "requires_project"),
        booleanMember(value, "requires_shell"),
        stringMember(value, "input_schema")};
}

[[nodiscard]] Json toolsSnapshotJson(const ManagerToolsSnapshot& snapshot)
{
    Json tools = Json::array();
    for (const auto& descriptor : snapshot.tools) {
        tools.push_back(toolDescriptorJson(descriptor));
    }
    return Json{{"shell_enabled", snapshot.shellEnabled}, {"tools", std::move(tools)}};
}

[[nodiscard]] ManagerToolsSnapshot parseToolsSnapshot(const Json& value)
{
    requireExactFields(value, {"shell_enabled", "tools"}, "Manager tools snapshot");
    const auto& items = member(value, "tools");
    if (!items.is_array()) {
        reject(Domain::ErrorCodes::InvalidRequest, "Manager tools must be an array.");
    }
    ManagerToolsSnapshot snapshot;
    snapshot.shellEnabled = booleanMember(value, "shell_enabled");
    snapshot.tools.reserve(items.size());
    for (const auto& item : items) snapshot.tools.push_back(parseToolDescriptor(item));
    return snapshot;
}

[[nodiscard]] Json toolOutcomeSnapshotJson(
    const ManagerToolOutcomeSnapshot& snapshot)
{
    return Json{
        {"project_id", snapshot.projectId.value()},
        {"tool_name", snapshot.toolName},
        {"ok", snapshot.ok},
        {"canonical_payload", snapshot.canonicalPayload},
        {"error", snapshot.error ? errorJson(*snapshot.error) : Json(nullptr)},
        {"elapsed_ms", snapshot.elapsed.count()}};
}

[[nodiscard]] ManagerToolOutcomeSnapshot parseToolOutcomeSnapshot(const Json& value)
{
    requireExactFields(
        value,
        {"canonical_payload", "elapsed_ms", "error", "ok", "project_id", "tool_name"},
        "Manager tool outcome");
    return ManagerToolOutcomeSnapshot{
        identifierMember<Domain::ProjectId>(value, "project_id"),
        stringMember(value, "tool_name"),
        booleanMember(value, "ok"),
        stringMember(value, "canonical_payload"),
        optionalField<Domain::Error>(
            value, "error",
            [](const Json& object, const std::string_view name) {
                return parseError(member(object, name));
            }),
        std::chrono::milliseconds{nonnegativeIntegerMember(value, "elapsed_ms")}};
}

[[nodiscard]] std::string_view operationalAreaName(
    const ManagerOperationalArea area) noexcept
{
    switch (area) {
    case ManagerOperationalArea::Agents: return "agents";
    case ManagerOperationalArea::Feed: return "feed";
    case ManagerOperationalArea::Runtimes: return "runtimes";
    case ManagerOperationalArea::Diagnostics: return "diagnostics";
    case ManagerOperationalArea::Manager: return "manager";
    case ManagerOperationalArea::Runs: return "runs";
    case ManagerOperationalArea::Evidence: return "evidence";
    }
    return "manager";
}

[[nodiscard]] Json operationalSnapshotJson(
    const ManagerOperationalSnapshot& snapshot)
{
    return Json{{"area", operationalAreaName(snapshot.area)},
                {"title", snapshot.title}, {"lines", snapshot.lines}};
}

[[nodiscard]] ManagerOperationalSnapshot parseOperationalSnapshot(const Json& value)
{
    requireExactFields(value, {"area", "lines", "title"}, "Manager operational snapshot");
    const auto& areaText = stringMember(value, "area");
    ManagerOperationalArea area;
    if (areaText == "agents") area = ManagerOperationalArea::Agents;
    else if (areaText == "feed") area = ManagerOperationalArea::Feed;
    else if (areaText == "runtimes") area = ManagerOperationalArea::Runtimes;
    else if (areaText == "diagnostics") area = ManagerOperationalArea::Diagnostics;
    else if (areaText == "manager") area = ManagerOperationalArea::Manager;
    else if (areaText == "runs") area = ManagerOperationalArea::Runs;
    else if (areaText == "evidence") area = ManagerOperationalArea::Evidence;
    else reject(Domain::ErrorCodes::InvalidRequest, "Manager operational area is unknown.");
    return ManagerOperationalSnapshot{
        area,
        stringMember(value, "title"),
        stringArray(member(value, "lines"), "Manager operational lines")};
}

[[nodiscard]] std::string_view maintenanceScopeName(
    const ManagerMaintenanceScope scope) noexcept
{
    switch (scope) {
    case ManagerMaintenanceScope::ProjectMemory: return "project_memory";
    case ManagerMaintenanceScope::ProjectContinuity: return "project_continuity";
    case ManagerMaintenanceScope::ProjectAllData: return "project_all_data";
    case ManagerMaintenanceScope::AllProjectsAllData: return "all_projects_all_data";
    }
    return "project_memory";
}

[[nodiscard]] Json maintenanceSnapshotJson(
    const ManagerMaintenanceSnapshot& snapshot)
{
    return Json{{"scope", maintenanceScopeName(snapshot.scope)},
                {"affected_scope", snapshot.affectedScope},
                {"projects_affected", snapshot.projectsAffected},
                {"records_removed", snapshot.recordsRemoved},
                {"links_removed", snapshot.linksRemoved},
                {"events_removed", snapshot.eventsRemoved},
                {"verified", snapshot.verified},
                {"detail", snapshot.detail}};
}

[[nodiscard]] ManagerMaintenanceSnapshot parseMaintenanceSnapshot(const Json& value)
{
    requireExactFields(
        value,
        {"affected_scope", "detail", "events_removed", "links_removed",
         "projects_affected", "records_removed", "scope", "verified"},
        "Manager maintenance snapshot");
    const auto& scopeText = stringMember(value, "scope");
    ManagerMaintenanceScope scope;
    if (scopeText == "project_memory") scope = ManagerMaintenanceScope::ProjectMemory;
    else if (scopeText == "project_continuity") scope = ManagerMaintenanceScope::ProjectContinuity;
    else if (scopeText == "project_all_data") scope = ManagerMaintenanceScope::ProjectAllData;
    else if (scopeText == "all_projects_all_data") scope = ManagerMaintenanceScope::AllProjectsAllData;
    else reject(Domain::ErrorCodes::InvalidRequest, "Manager maintenance scope is unknown.");
    return ManagerMaintenanceSnapshot{
        scope,
        stringMember(value, "affected_scope"),
        sizeMember(value, "projects_affected"),
        sizeMember(value, "records_removed"),
        sizeMember(value, "links_removed"),
        sizeMember(value, "events_removed"),
        booleanMember(value, "verified"),
        stringMember(value, "detail")};
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
            } else if constexpr (
                std::is_same_v<Value, Domain::ManagerSettingsUpdateOutcome>) {
                wrapper["type"] = "settings_update";
                wrapper["value"] = settingsUpdateOutcomeJson(value);
            } else if constexpr (
                std::is_same_v<Value, Domain::ManagedRunSnapshot>) {
                wrapper["type"] = "managed_run";
                wrapper["value"] = managedRunSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, Domain::ManagerTelemetrySnapshot>) {
                wrapper["type"] = "telemetry";
                wrapper["value"] = managerTelemetrySnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerProjectsSnapshot>) {
                wrapper["type"] = "projects";
                wrapper["value"] = projectsSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerProjectWorkspaceSnapshot>) {
                wrapper["type"] = "project_workspace";
                wrapper["value"] = projectWorkspaceSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerLmStudioSnapshot>) {
                wrapper["type"] = "lmstudio";
                wrapper["value"] = lmStudioSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerToolsSnapshot>) {
                wrapper["type"] = "tools";
                wrapper["value"] = toolsSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerToolOutcomeSnapshot>) {
                wrapper["type"] = "tool_outcome";
                wrapper["value"] = toolOutcomeSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerOperationalSnapshot>) {
                wrapper["type"] = "operational";
                wrapper["value"] = operationalSnapshotJson(value);
            } else if constexpr (
                std::is_same_v<Value, ManagerMaintenanceSnapshot>) {
                wrapper["type"] = "maintenance";
                wrapper["value"] = maintenanceSnapshotJson(value);
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
    if (type == "settings_update") {
        return ManagerResult{parseSettingsUpdateOutcome(value)};
    }
    if (type == "managed_run") {
        return ManagerResult{parseManagedRunSnapshot(value)};
    }
    if (type == "telemetry") {
        return ManagerResult{parseManagerTelemetrySnapshot(value)};
    }
    if (type == "projects") {
        return ManagerResult{parseProjectsSnapshot(value)};
    }
    if (type == "project_workspace") {
        return ManagerResult{parseProjectWorkspaceSnapshot(value)};
    }
    if (type == "lmstudio") {
        return ManagerResult{parseLmStudioSnapshot(value)};
    }
    if (type == "tools") {
        return ManagerResult{parseToolsSnapshot(value)};
    }
    if (type == "tool_outcome") {
        return ManagerResult{parseToolOutcomeSnapshot(value)};
    }
    if (type == "operational") {
        return ManagerResult{parseOperationalSnapshot(value)};
    }
    if (type == "maintenance") {
        return ManagerResult{parseMaintenanceSnapshot(value)};
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
