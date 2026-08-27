#include "ForgeConductor/Dashboard/DashboardManagerJsonCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
    REQUIRE(!result.error().message.empty());
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    std::transform(
        value.begin(),
        value.end(),
        std::back_inserter(result),
        [](const char character) {
            return static_cast<std::byte>(
                static_cast<unsigned char>(character));
        });
    return result;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    std::string result(value.size(), '\0');
    std::transform(
        value.begin(),
        value.end(),
        result.begin(),
        [](const std::byte character) {
            return static_cast<char>(std::to_integer<unsigned char>(character));
        });
    return result;
}

[[nodiscard]] Json document(Domain::Result<std::vector<std::byte>> result)
{
    return Json::parse(text(take(std::move(result))));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::ManagerSettings sampleSettings()
{
    Domain::ManagerSettings settings;
    settings.dashboardHost = "::1";
    settings.dashboardPort = 44'444U;
    settings.dashboardRefreshInterval = 17s;
    settings.autoRestart = false;
    settings.watchdogInterval = 11s;
    settings.openBrowserOnStart = true;
    settings.sessionIdleTtl = 23'456s;
    settings.shellTimeout = 119s;
    settings.logLevel = Domain::LogLevel::Critical;
    return settings;
}

[[nodiscard]] Domain::ManagerStatus sampleStatus()
{
    const auto& settings = sampleSettings();
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        42'424U,
        Domain::UtcTimePoint{1'735'787'045s + 987ms},
        987s,
        9U,
        std::optional<std::string>{"prior restart recovered"},
        settings.autoRestart,
        settings.watchdogInterval,
        settings.openBrowserOnStart,
        settings.dashboardHost,
        settings.dashboardPort,
        settings.dashboardRefreshInterval,
        path("C:\\Users\\tester\\.forge-conductor"),
        "0.9.0-alpha"};
}

[[nodiscard]] Domain::ManagerSettingsUpdateOutcome sampleOutcome(
    const bool applied,
    const bool bindingChanged)
{
    return Domain::ManagerSettingsUpdateOutcome{
        sampleSettings(), applied, bindingChanged, sampleStatus()};
}

void requireExactKeys(
    const Json& value,
    const std::initializer_list<std::string_view> expected)
{
    REQUIRE(value.is_object());
    REQUIRE(value.size() == expected.size());
    for (const auto name : expected) {
        REQUIRE(value.contains(std::string{name}));
    }
}

[[nodiscard]] Dashboard::DashboardManagerSettingsMutation mutation(
    const std::string_view input)
{
    const auto body = bytes(input);
    return take(Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(body));
}

void rejectMutation(
    const std::string_view input,
    const std::string_view expectedCode = Domain::ErrorCodes::InvalidRequest)
{
    const auto body = bytes(input);
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(body),
        expectedCode);
}

void exposesBoundedImmutableContract()
{
    static_assert(std::is_final_v<Dashboard::DashboardManagerJsonCodec>);
    static_assert(std::is_final_v<Dashboard::DashboardManagerSettingsMutation>);
    static_assert(
        Dashboard::DashboardManagerJsonCodec::MaximumMutationBytes == 1'048'576U);
    static_assert(
        Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes == 2'097'152U);
    static_assert(
        Dashboard::DashboardManagerJsonCodec::MaximumJsonNesting == 16U);
    static_assert(std::is_same_v<
        decltype(std::declval<const Dashboard::DashboardManagerSettingsMutation&>()
                     .patch()),
        const Domain::ManagerSettingsPatch&>);
    static_assert(noexcept(
        std::declval<const Dashboard::DashboardManagerSettingsMutation&>().apply()));

    const auto empty = mutation("{}");
    REQUIRE(empty.apply());
    REQUIRE(!empty.patch().dashboardHost);
    REQUIRE(!empty.patch().dashboardPort);
    REQUIRE(!empty.patch().dashboardRefreshInterval);
    REQUIRE(!empty.patch().autoRestart);
    REQUIRE(!empty.patch().watchdogInterval);
    REQUIRE(!empty.patch().openBrowserOnStart);
    REQUIRE(!empty.patch().sessionIdleTtl);
    REQUIRE(!empty.patch().shellTimeout);
    REQUIRE(!empty.patch().logLevel);
}

void encodesExactManagerStatusSchema()
{
    const auto encoded = take(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(sampleStatus()));
    REQUIRE(!encoded.empty());
    const auto value = Json::parse(text(encoded));
    requireExactKeys(
        value,
        {"ok",
         "manager",
         "state",
         "desired_running",
         "http_listening",
         "service_active",
         "pid",
         "started_at",
         "uptime_sec",
         "restart_count",
         "last_error",
         "auto_restart",
         "watchdog_interval_sec",
         "open_browser_on_start",
         "dashboard",
         "home",
         "version"});
    requireExactKeys(
        value.at("dashboard"),
        {"host", "port", "url", "refresh_interval_sec"});
    REQUIRE(value.at("ok") == true);
    REQUIRE(value.at("manager") == true);
    REQUIRE(value.at("state") == "running");
    REQUIRE(value.at("desired_running") == true);
    REQUIRE(value.at("http_listening") == true);
    REQUIRE(value.at("service_active") == true);
    REQUIRE(value.at("pid") == 42'424U);
    REQUIRE(value.at("started_at") == "2025-01-02T03:04:05Z");
    REQUIRE(value.at("uptime_sec") == 987);
    REQUIRE(value.at("restart_count") == 9U);
    REQUIRE(value.at("last_error") == "prior restart recovered");
    REQUIRE(value.at("auto_restart") == false);
    REQUIRE(value.at("watchdog_interval_sec") == 11);
    REQUIRE(value.at("open_browser_on_start") == true);
    REQUIRE(value.at("dashboard").at("host") == "::1");
    REQUIRE(value.at("dashboard").at("port") == 44'444U);
    REQUIRE(value.at("dashboard").at("url") == "http://[::1]:44444/");
    REQUIRE(value.at("dashboard").at("refresh_interval_sec") == 17);
    REQUIRE(value.at("home") == "C:\\Users\\tester\\.forge-conductor");
    REQUIRE(value.at("version") == "0.9.0-alpha");

    auto absent = sampleStatus();
    absent.startedAt.reset();
    absent.uptime.reset();
    absent.lastError.reset();
    absent.dashboardHost = "127.0.0.1";
    absent.dashboardPort = 7788U;
    const auto absentValue = document(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(absent));
    REQUIRE(absentValue.at("started_at").is_null());
    REQUIRE(absentValue.at("uptime_sec").is_null());
    REQUIRE(absentValue.at("last_error").is_null());
    REQUIRE(
        absentValue.at("dashboard").at("url") ==
        "http://127.0.0.1:7788/");

    const std::vector<std::pair<Domain::ManagerServiceState, std::string>> states{
        {Domain::ManagerServiceState::Stopped, "stopped"},
        {Domain::ManagerServiceState::Starting, "starting"},
        {Domain::ManagerServiceState::Running, "running"},
        {Domain::ManagerServiceState::Restarting, "restarting"},
        {Domain::ManagerServiceState::Stopping, "stopping"},
        {Domain::ManagerServiceState::Failed, "failed"}};
    for (const auto& [state, name] : states) {
        auto status = sampleStatus();
        status.state = state;
        REQUIRE(document(
                    Dashboard::DashboardManagerJsonCodec::encodeStatus(status))
                    .at("state") == name);
    }
}

void rejectsInvalidOrUnboundedStatus()
{
    auto invalidState = sampleStatus();
    invalidState.state = static_cast<Domain::ManagerServiceState>(999);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(invalidState),
        Domain::ErrorCodes::InvalidRequest);

    auto negativeUptime = sampleStatus();
    negativeUptime.uptime = -1s;
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(negativeUptime),
        Domain::ErrorCodes::InvalidRequest);

    auto preEpoch = sampleStatus();
    preEpoch.startedAt = Domain::UtcTimePoint{-1s};
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(preEpoch),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidHost = sampleStatus();
    invalidHost.dashboardHost = "localhost";
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(invalidHost),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidText = sampleStatus();
    invalidText.lastError = std::string{"bad"} + static_cast<char>(0xff);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(invalidText),
        Domain::ErrorCodes::InvalidRequest);

    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(sampleStatus(), 64U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(sampleStatus(), 0U),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(
            sampleStatus(),
            Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes + 1U),
        Domain::ErrorCodes::InvalidRequest);
}

void preflightsResponseBudgetsBeforeDomConstruction()
{
    const auto status = sampleStatus();
    const auto statusBytes = take(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(status));
    REQUIRE(take(Dashboard::DashboardManagerJsonCodec::encodeStatus(
                status, statusBytes.size())) == statusBytes);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(
            status, statusBytes.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    auto hardLimitText = sampleStatus();
    hardLimitText.version.assign(
        Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes,
        'v');
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(hardLimitText),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeStatus(
            hardLimitText, 128U),
        Domain::ErrorCodes::PayloadTooLarge);

    hardLimitText.state = static_cast<Domain::ManagerServiceState>(999);
    const auto invalidZeroLimit =
        Dashboard::DashboardManagerJsonCodec::encodeStatus(hardLimitText, 0U);
    requireError(invalidZeroLimit, Domain::ErrorCodes::InvalidRequest);
    REQUIRE(invalidZeroLimit.error().message.find("byte limit") !=
            std::string::npos);
    const auto invalidOverLimit =
        Dashboard::DashboardManagerJsonCodec::encodeStatus(
            hardLimitText,
            Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes + 1U);
    requireError(invalidOverLimit, Domain::ErrorCodes::InvalidRequest);
    REQUIRE(invalidOverLimit.error().message.find("byte limit") !=
            std::string::npos);

    const auto settings = sampleSettings();
    const auto settingsBytes = take(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(settings));
    REQUIRE(take(Dashboard::DashboardManagerJsonCodec::encodeSettings(
                settings, settingsBytes.size())) == settingsBytes);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(
            settings, settingsBytes.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(settings, 32U),
        Domain::ErrorCodes::PayloadTooLarge);

    auto invalidSettings = sampleSettings();
    invalidSettings.dashboardHost = "localhost";
    const auto invalidSettingsLimit =
        Dashboard::DashboardManagerJsonCodec::encodeSettings(
            invalidSettings, 0U);
    requireError(invalidSettingsLimit, Domain::ErrorCodes::InvalidRequest);
    REQUIRE(invalidSettingsLimit.error().message.find("byte limit") !=
            std::string::npos);

    const auto outcome = sampleOutcome(true, true);
    const auto outcomeBytes = take(
        Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
            outcome));
    REQUIRE(take(
                Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
                    outcome, outcomeBytes.size())) == outcomeBytes);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
            outcome, outcomeBytes.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    auto aggregate = sampleOutcome(true, true);
    aggregate.status.version.assign(
        Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes / 2U,
        'v');
    aggregate.status.lastError = std::string(
        Dashboard::DashboardManagerJsonCodec::MaximumResponseBytes / 2U,
        'e');
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
            aggregate),
        Domain::ErrorCodes::PayloadTooLarge);
}

void encodesExactManagerSettingsSchema()
{
    const auto value = document(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(sampleSettings()));
    requireExactKeys(
        value,
        {"ok", "dashboard", "manager", "sessions", "shell", "log_level"});
    requireExactKeys(
        value.at("dashboard"),
        {"host", "port", "refresh_interval_sec"});
    requireExactKeys(
        value.at("manager"),
        {"auto_restart", "watchdog_interval_sec", "open_browser_on_start"});
    requireExactKeys(value.at("sessions"), {"idle_ttl_sec"});
    requireExactKeys(value.at("shell"), {"default_timeout_sec"});
    REQUIRE(value.at("ok") == true);
    REQUIRE(value.at("dashboard").at("host") == "::1");
    REQUIRE(value.at("dashboard").at("port") == 44'444U);
    REQUIRE(value.at("dashboard").at("refresh_interval_sec") == 17);
    REQUIRE(!value.at("dashboard").contains("url"));
    REQUIRE(value.at("manager").at("auto_restart") == false);
    REQUIRE(value.at("manager").at("watchdog_interval_sec") == 11);
    REQUIRE(value.at("manager").at("open_browser_on_start") == true);
    REQUIRE(value.at("sessions").at("idle_ttl_sec") == 23'456);
    REQUIRE(value.at("shell").at("default_timeout_sec") == 119);
    REQUIRE(value.at("log_level") == "critical");

    const std::vector<std::pair<Domain::LogLevel, std::string>> levels{
        {Domain::LogLevel::Trace, "trace"},
        {Domain::LogLevel::Debug, "debug"},
        {Domain::LogLevel::Info, "info"},
        {Domain::LogLevel::Warning, "warn"},
        {Domain::LogLevel::Error, "error"},
        {Domain::LogLevel::Critical, "critical"}};
    for (const auto& [level, name] : levels) {
        auto settings = sampleSettings();
        settings.logLevel = level;
        REQUIRE(document(
                    Dashboard::DashboardManagerJsonCodec::encodeSettings(settings))
                    .at("log_level") == name);
    }

    auto invalid = sampleSettings();
    invalid.shellTimeout = 121s;
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(invalid),
        Domain::ErrorCodes::InvalidRequest);
    invalid = sampleSettings();
    invalid.logLevel = static_cast<Domain::LogLevel>(999);
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettings(invalid),
        Domain::ErrorCodes::InvalidRequest);
}

void flattensAtomicUpdateOutcomeAndPreservesNewUrl()
{
    for (const bool applied : {false, true}) {
        for (const bool bindingChanged : {false, true}) {
            const auto value = document(
                Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
                    sampleOutcome(applied, bindingChanged)));
            requireExactKeys(
                value,
                {"ok",
                 "dashboard",
                 "manager",
                 "sessions",
                 "shell",
                 "log_level",
                 "applied",
                 "bind_changed",
                 "status"});
            REQUIRE(value.at("applied") == applied);
            REQUIRE(value.at("bind_changed") == bindingChanged);
            REQUIRE(!value.contains("binding_changed"));
            REQUIRE(!value.contains("settings"));
            requireExactKeys(
                value.at("status"),
                {"ok",
                 "manager",
                 "state",
                 "desired_running",
                 "http_listening",
                 "service_active",
                 "pid",
                 "started_at",
                 "uptime_sec",
                 "restart_count",
                 "last_error",
                 "auto_restart",
                 "watchdog_interval_sec",
                 "open_browser_on_start",
                 "dashboard",
                 "home",
                 "version"});
            REQUIRE(
                value.at("status").at("dashboard").at("url") ==
                "http://[::1]:44444/");
        }
    }

    auto changed = sampleOutcome(true, true);
    changed.settings.dashboardHost = "127.0.0.1";
    changed.settings.dashboardPort = 65'535U;
    changed.status.dashboardHost = "127.0.0.1";
    changed.status.dashboardPort = 65'535U;
    const auto changedValue = document(
        Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(changed));
    REQUIRE(
        changedValue.at("status").at("dashboard").at("url") ==
        "http://127.0.0.1:65535/");
    REQUIRE(changedValue.at("dashboard").at("host") == "127.0.0.1");
    REQUIRE(changedValue.at("dashboard").at("port") == 65'535U);

    auto mismatched = sampleOutcome(true, true);
    mismatched.status.dashboardPort = 7'788U;
    requireError(
        Dashboard::DashboardManagerJsonCodec::encodeSettingsUpdateOutcome(
            mismatched),
        Domain::ErrorCodes::InvalidRequest);
}

void decodesNestedAndTopLevelPatches()
{
    const auto nested = mutation(R"json({
        "apply":false,
        "settings":{
            "dashboard":{"host":"::1","port":65535,"refresh_interval_sec":12},
            "manager":{"auto_restart":false,"watchdog_interval_sec":4,"open_browser_on_start":true},
            "sessions":{"idle_ttl_sec":8888},
            "shell":{"default_timeout_sec":77},
            "log_level":"debug"
        }
    })json");
    REQUIRE(!nested.apply());
    REQUIRE(nested.patch().dashboardHost == "::1");
    REQUIRE(nested.patch().dashboardPort == 65'535U);
    REQUIRE(nested.patch().dashboardRefreshInterval == 12s);
    REQUIRE(nested.patch().autoRestart == false);
    REQUIRE(nested.patch().watchdogInterval == 4s);
    REQUIRE(nested.patch().openBrowserOnStart == true);
    REQUIRE(nested.patch().sessionIdleTtl == 8'888s);
    REQUIRE(nested.patch().shellTimeout == 77s);
    REQUIRE(nested.patch().logLevel == Domain::LogLevel::Debug);

    const auto topLevel = mutation(R"json({
        "dashboard":{"host":"127.0.0.1","port":9000},
        "manager":{"auto_restart":true},
        "sessions":{},
        "shell":{},
        "log_level":"warn"
    })json");
    REQUIRE(topLevel.apply());
    REQUIRE(topLevel.patch().dashboardHost == "127.0.0.1");
    REQUIRE(topLevel.patch().dashboardPort == 9'000U);
    REQUIRE(!topLevel.patch().dashboardRefreshInterval);
    REQUIRE(topLevel.patch().autoRestart == true);
    REQUIRE(!topLevel.patch().watchdogInterval);
    REQUIRE(!topLevel.patch().openBrowserOnStart);
    REQUIRE(!topLevel.patch().sessionIdleTtl);
    REQUIRE(!topLevel.patch().shellTimeout);
    REQUIRE(topLevel.patch().logLevel == Domain::LogLevel::Warning);

    const auto nestedDefault = mutation(R"json({"settings":{}})json");
    REQUIRE(nestedDefault.apply());
    const auto applyOnly = mutation(R"json({"apply":false})json");
    REQUIRE(!applyOnly.apply());

    const std::vector<std::pair<std::string, Domain::LogLevel>> levels{
        {"trace", Domain::LogLevel::Trace},
        {"debug", Domain::LogLevel::Debug},
        {"info", Domain::LogLevel::Info},
        {"warn", Domain::LogLevel::Warning},
        {"error", Domain::LogLevel::Error},
        {"critical", Domain::LogLevel::Critical}};
    for (const auto& [name, level] : levels) {
        const auto parsed = mutation(
            std::string{"{\"log_level\":\""} + name + "\"}");
        REQUIRE(parsed.patch().logLevel == level);
    }
}

void enforcesManagerIntervalBoundaries()
{
    const auto requireSettingsValidity = [](
                                             const std::chrono::seconds refresh,
                                             const std::chrono::seconds watchdog,
                                             const std::chrono::seconds idleTtl,
                                             const bool expected) {
        auto settings = sampleSettings();
        settings.dashboardRefreshInterval = refresh;
        settings.watchdogInterval = watchdog;
        settings.sessionIdleTtl = idleTtl;
        const auto encoded =
            Dashboard::DashboardManagerJsonCodec::encodeSettings(settings);
        REQUIRE(static_cast<bool>(encoded) == expected);
        if (!expected) {
            REQUIRE(encoded.error().code == Domain::ErrorCodes::InvalidRequest);
        }
    };

    requireSettingsValidity(1s, 1s, 60s, false);
    requireSettingsValidity(2s, 1s, 60s, true);
    requireSettingsValidity(300s, 1s, 60s, true);
    requireSettingsValidity(301s, 1s, 60s, false);
    requireSettingsValidity(2s, 1s, 60s, true);
    requireSettingsValidity(2s, 60s, 60s, true);
    requireSettingsValidity(2s, 61s, 60s, false);
    requireSettingsValidity(2s, 1s, 59s, false);
    requireSettingsValidity(2s, 1s, 60s, true);

    rejectMutation(
        R"json({"dashboard":{"refresh_interval_sec":1}})json");
    REQUIRE(
        mutation(R"json({"dashboard":{"refresh_interval_sec":2}})json")
            .patch()
            .dashboardRefreshInterval == 2s);
    REQUIRE(
        mutation(R"json({"dashboard":{"refresh_interval_sec":300}})json")
            .patch()
            .dashboardRefreshInterval == 300s);
    rejectMutation(
        R"json({"dashboard":{"refresh_interval_sec":301}})json");
    REQUIRE(
        mutation(R"json({"manager":{"watchdog_interval_sec":1}})json")
            .patch()
            .watchdogInterval == 1s);
    REQUIRE(
        mutation(R"json({"manager":{"watchdog_interval_sec":60}})json")
            .patch()
            .watchdogInterval == 60s);
    rejectMutation(
        R"json({"manager":{"watchdog_interval_sec":61}})json");
    rejectMutation(R"json({"sessions":{"idle_ttl_sec":59}})json");
    REQUIRE(
        mutation(R"json({"sessions":{"idle_ttl_sec":60}})json")
            .patch()
            .sessionIdleTtl == 60s);
}

void rejectsAmbiguousDuplicateAndUnknownKeys()
{
    rejectMutation(
        R"json({"settings":{},"dashboard":{}})json");
    rejectMutation(
        R"json({"settings":{},"log_level":"info"})json");
    rejectMutation(R"json({"apply":true,"unknown":1})json");
    rejectMutation(R"json({"settings":{"unknown":1}})json");
    rejectMutation(R"json({"dashboard":{"unknown":1}})json");
    rejectMutation(R"json({"manager":{"unknown":1}})json");
    rejectMutation(R"json({"sessions":{"unknown":1}})json");
    rejectMutation(R"json({"shell":{"unknown":1}})json");
    rejectMutation(R"json({"settings":{"apply":true}})json");
    rejectMutation(R"json({"ok":true})json");

    rejectMutation(
        R"json({"apply":true,"apply":false})json",
        Domain::ErrorCodes::MalformedMessage);
    rejectMutation(
        R"json({"dashboard":{"port":7788,"port":7789}})json",
        Domain::ErrorCodes::MalformedMessage);
    rejectMutation(
        R"json({"settings":{"manager":{"auto_restart":true,"auto_restart":false}}})json",
        Domain::ErrorCodes::MalformedMessage);
}

void rejectsWrongJsonTypesAndInvalidRanges()
{
    const std::vector<std::string> wrongShapes{
        "null",
        "[]",
        R"json({"apply":1})json",
        R"json({"settings":null})json",
        R"json({"dashboard":false})json",
        R"json({"manager":[]})json",
        R"json({"sessions":"bad"})json",
        R"json({"shell":1})json",
        R"json({"dashboard":{"host":1}})json",
        R"json({"dashboard":{"port":"7788"}})json",
        R"json({"dashboard":{"refresh_interval_sec":true}})json",
        R"json({"manager":{"auto_restart":1}})json",
        R"json({"manager":{"watchdog_interval_sec":"3"}})json",
        R"json({"manager":{"open_browser_on_start":0}})json",
        R"json({"sessions":{"idle_ttl_sec":false}})json",
        R"json({"shell":{"default_timeout_sec":"30"}})json",
        R"json({"log_level":false})json"};
    for (const auto& input : wrongShapes) rejectMutation(input);

    const std::vector<std::string> invalidValues{
        R"json({"dashboard":{"host":"localhost"}})json",
        R"json({"dashboard":{"host":"127.0.0.1 "}})json",
        R"json({"dashboard":{"port":0}})json",
        R"json({"dashboard":{"port":-1}})json",
        R"json({"dashboard":{"port":65536}})json",
        R"json({"dashboard":{"port":1.0}})json",
        R"json({"dashboard":{"refresh_interval_sec":0}})json",
        R"json({"dashboard":{"refresh_interval_sec":-1}})json",
        R"json({"dashboard":{"refresh_interval_sec":1.5}})json",
        R"json({"manager":{"watchdog_interval_sec":0}})json",
        R"json({"manager":{"watchdog_interval_sec":-1}})json",
        R"json({"sessions":{"idle_ttl_sec":0}})json",
        R"json({"sessions":{"idle_ttl_sec":-1}})json",
        R"json({"shell":{"default_timeout_sec":0}})json",
        R"json({"shell":{"default_timeout_sec":121}})json",
        R"json({"log_level":"warning"})json",
        R"json({"log_level":"INFO"})json",
        R"json({"sessions":{"idle_ttl_sec":9223372036854775808}})json"};
    for (const auto& input : invalidValues) rejectMutation(input);
}

void rejectsMalformedOversizedOrHostileInputBeforeMapping()
{
    rejectMutation({}, Domain::ErrorCodes::MalformedMessage);
    rejectMutation("{", Domain::ErrorCodes::MalformedMessage);
    rejectMutation("{} trailing", Domain::ErrorCodes::MalformedMessage);
    rejectMutation(
        R"json({"sett\u0000ings":{}})json",
        Domain::ErrorCodes::MalformedMessage);
    rejectMutation(
        R"json({"dashboard":{"host":"127.0.0.1\u0000"}})json",
        Domain::ErrorCodes::MalformedMessage);

    auto rawNul = bytes(R"json({"apply":true})json");
    rawNul.insert(rawNul.begin() + 2, std::byte{0});
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(rawNul),
        Domain::ErrorCodes::MalformedMessage);

    auto invalidUtf8 = bytes(R"json({"log_level":"info"})json");
    invalidUtf8.insert(invalidUtf8.end() - 2, std::byte{0xff});
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(invalidUtf8),
        Domain::ErrorCodes::MalformedMessage);

    std::string deep{"{\"unknown\":"};
    deep.append(
        Dashboard::DashboardManagerJsonCodec::MaximumJsonNesting + 1U,
        '[');
    deep += '0';
    deep.append(
        Dashboard::DashboardManagerJsonCodec::MaximumJsonNesting + 1U,
        ']');
    deep += '}';
    rejectMutation(deep, Domain::ErrorCodes::LimitExceeded);

    std::vector<std::byte> oversized(
        Dashboard::DashboardManagerJsonCodec::MaximumMutationBytes + 1U,
        static_cast<std::byte>('x'));
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(oversized),
        Domain::ErrorCodes::PayloadTooLarge);

    const auto emptyObject = bytes("{}");
    REQUIRE(take(
                Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(
                    emptyObject, 2U))
                .apply());
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(
            emptyObject, 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(
            emptyObject, 0U),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(
            emptyObject,
            Dashboard::DashboardManagerJsonCodec::MaximumMutationBytes + 1U),
        Domain::ErrorCodes::InvalidRequest);
}

} // namespace

int main()
{
    try {
        exposesBoundedImmutableContract();
        encodesExactManagerStatusSchema();
        rejectsInvalidOrUnboundedStatus();
        preflightsResponseBudgetsBeforeDomConstruction();
        encodesExactManagerSettingsSchema();
        flattensAtomicUpdateOutcomeAndPreservesNewUrl();
        decodesNestedAndTopLevelPatches();
        enforcesManagerIntervalBoundaries();
        rejectsAmbiguousDuplicateAndUnknownKeys();
        rejectsWrongJsonTypesAndInvalidRanges();
        rejectsMalformedOversizedOrHostileInputBeforeMapping();
        std::cout << "Dashboard manager JSON codec tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard manager JSON codec tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard manager JSON codec tests failed with an unknown error.\n";
        return 1;
    }
}
