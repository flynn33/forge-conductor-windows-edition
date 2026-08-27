#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;
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
}

template <typename Identifier>
[[nodiscard]] Identifier identifier(const std::string_view value)
{
    return take(Identifier::parse(value));
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] std::vector<std::byte> frameFromText(const std::string_view text)
{
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::runtime_error{"Test frame exceeds the protocol prefix."};
    }
    const auto length = static_cast<std::uint32_t>(text.size());
    std::vector<std::byte> frame(text.size() + 4U);
    frame[0] = static_cast<std::byte>(length & 0xffU);
    frame[1] = static_cast<std::byte>((length >> 8U) & 0xffU);
    frame[2] = static_cast<std::byte>((length >> 16U) & 0xffU);
    frame[3] = static_cast<std::byte>((length >> 24U) & 0xffU);
    if (!text.empty()) {
        std::memcpy(frame.data() + 4U, text.data(), text.size());
    }
    return frame;
}

[[nodiscard]] std::vector<std::byte> frameFromJson(const Json& value)
{
    return frameFromText(value.dump());
}

[[nodiscard]] std::string payloadText(const std::vector<std::byte>& frame)
{
    REQUIRE(frame.size() >= 4U);
    std::string payload(frame.size() - 4U, '\0');
    if (!payload.empty()) {
        std::memcpy(payload.data(), frame.data() + 4U, payload.size());
    }
    return payload;
}

void replaceOne(
    std::string& value,
    const std::string_view before,
    const std::string_view after)
{
    const auto offset = value.find(before);
    REQUIRE(offset != std::string::npos);
    value.replace(offset, before.size(), after);
}

[[nodiscard]] Domain::ManagerSettings sampleSettings()
{
    Domain::ManagerSettings settings;
    settings.dashboardHost = "::1";
    settings.dashboardPort = 65'535U;
    settings.dashboardRefreshInterval = 17s;
    settings.autoRestart = false;
    settings.watchdogInterval = 11s;
    settings.openBrowserOnStart = true;
    settings.sessionIdleTtl = 23'456s;
    settings.shellTimeout = 119s;
    settings.logLevel = Domain::LogLevel::Critical;
    return settings;
}

[[nodiscard]] Domain::ManagerSettingsPatch samplePatch()
{
    Domain::ManagerSettingsPatch patch;
    patch.dashboardHost = "::1";
    patch.dashboardPort = static_cast<std::uint16_t>(44'444U);
    patch.dashboardRefreshInterval = 12s;
    patch.autoRestart = false;
    patch.watchdogInterval = 4s;
    patch.openBrowserOnStart = true;
    patch.sessionIdleTtl = 8'888s;
    patch.shellTimeout = 77s;
    patch.logLevel = Domain::LogLevel::Debug;
    return patch;
}

[[nodiscard]] Domain::ManagerStatus sampleStatus()
{
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        42'424U,
        std::optional<Domain::UtcTimePoint>{
            Domain::UtcTimePoint{std::chrono::milliseconds{1'767'225'600'123LL}}},
        std::optional<std::chrono::seconds>{987s},
        9U,
        std::optional<std::string>{"prior restart recovered"},
        false,
        11s,
        true,
        "::1",
        65'535U,
        17s,
        path("C:\\Users\\tester\\.forge-conductor"),
        "0.9.0-alpha"};
}

[[nodiscard]] Domain::ManagerSettingsUpdateOutcome sampleSettingsUpdateOutcome(
    const bool applied,
    const bool bindingChanged)
{
    return Domain::ManagerSettingsUpdateOutcome{
        sampleSettings(), applied, bindingChanged, sampleStatus()};
}

[[nodiscard]] Manager::ManagerRequest request(
    Manager::ManagerRequestPayload payload)
{
    return Manager::ManagerRequest{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        1'767'225'630'000LL,
        identifier<Domain::Sha256Digest>(std::string(64U, 'a')),
        std::move(payload)};
}

[[nodiscard]] Manager::ManagerResponse response(Manager::ManagerResult result)
{
    return Manager::ManagerResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{std::move(result)}};
}

[[nodiscard]] Json requestJson(const Manager::ManagerRequestPayload& payload)
{
    return Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeRequest(request(payload)))));
}

[[nodiscard]] Json responseJson(const Manager::ManagerResult& result)
{
    return Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeResponse(response(result)))));
}

void testTypeAndPrefixContract()
{
    static_assert(std::is_final_v<Manager::ManagerProtocolCodec>);
    static_assert(Manager::ManagerProtocolVersion == 1U);
    static_assert(
        Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes == 2'097'152U);
    static_assert(Manager::ManagerProtocolCodec::MaximumJsonNesting == 64U);

    const auto encoded = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerStatusRequest{})));
    const auto payloadBytes = encoded.size() - 4U;
    const auto prefix =
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[0])) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[1])) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(encoded[3])) << 24U);
    REQUIRE(prefix == payloadBytes);

    const auto payload = payloadText(encoded);
    REQUIRE(!payload.empty() && payload.front() == '{' && payload.back() == '}');
    REQUIRE(payload.find('\n') == std::string::npos);
    REQUIRE(payload.starts_with("{\"correlation_id\":"));
}

void testEveryRequestMethodRoundTripsDeterministically()
{
    std::vector<Manager::ManagerRequestPayload> payloads;
    payloads.emplace_back(Manager::ManagerStatusRequest{});
    payloads.emplace_back(Manager::ManagerSettingsRequest{});
    payloads.emplace_back(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Repair});
    payloads.emplace_back(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), true});
    payloads.emplace_back(Manager::ManagerCancelRequest{
        identifier<Domain::OperationId>(
            "10000000-0000-4000-8000-000000000016")});
    payloads.emplace_back(Manager::ManagerShutdownRequest{});

    const std::vector<std::string> methods{
        "manager.status",
        "manager.settings",
        "manager.control",
        "manager.settings.update",
        "manager.cancel",
        "manager.shutdown"};

    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const auto original = request(payloads[index]);
        const auto first = take(
            Manager::ManagerProtocolCodec::encodeRequest(original));
        const auto second = take(
            Manager::ManagerProtocolCodec::encodeRequest(original));
        REQUIRE(first == second);

        const auto decoded = take(
            Manager::ManagerProtocolCodec::decodeRequest(first));
        REQUIRE(decoded.version == Manager::ManagerProtocolVersion);
        REQUIRE(decoded.requestId.value() == original.requestId.value());
        REQUIRE(decoded.correlationId.value() == original.correlationId.value());
        REQUIRE(decoded.deadlineUtcMilliseconds == original.deadlineUtcMilliseconds);
        REQUIRE(decoded.nonce.value() == original.nonce.value());
        REQUIRE(decoded.payload.index() == original.payload.index());
        REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(decoded)) == first);

        const auto json = Json::parse(payloadText(first));
        REQUIRE(json.size() == 7U);
        REQUIRE(json.at("method").get<std::string>() == methods[index]);
    }

    const auto update = take(Manager::ManagerProtocolCodec::decodeRequest(
        take(Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerSettingsUpdateRequest{samplePatch(), true})))));
    const auto& updatePayload =
        std::get<Manager::ManagerSettingsUpdateRequest>(update.payload);
    REQUIRE(updatePayload.applyImmediately);
    REQUIRE(updatePayload.patch.dashboardHost == "::1");
    REQUIRE(updatePayload.patch.dashboardPort == 44'444U);
    REQUIRE(updatePayload.patch.dashboardRefreshInterval == 12s);
    REQUIRE(updatePayload.patch.autoRestart == false);
    REQUIRE(updatePayload.patch.watchdogInterval == 4s);
    REQUIRE(updatePayload.patch.openBrowserOnStart == true);
    REQUIRE(updatePayload.patch.sessionIdleTtl == 8'888s);
    REQUIRE(updatePayload.patch.shellTimeout == 77s);
    REQUIRE(updatePayload.patch.logLevel == Domain::LogLevel::Debug);
}

void testResponseResultAndErrorRoundTrips()
{
    const auto statusFrame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleStatus()})));
    const auto decodedStatus = take(
        Manager::ManagerProtocolCodec::decodeResponse(statusFrame));
    REQUIRE(std::holds_alternative<Manager::ManagerResult>(decodedStatus.body));
    const auto& statusResult =
        std::get<Manager::ManagerResult>(decodedStatus.body);
    REQUIRE(std::holds_alternative<Domain::ManagerStatus>(statusResult));
    const auto& status = std::get<Domain::ManagerStatus>(statusResult);
    REQUIRE(status.ok && status.isManager);
    REQUIRE(status.state == Domain::ManagerServiceState::Running);
    REQUIRE(status.processId == 42'424U);
    REQUIRE(status.startedAt == sampleStatus().startedAt);
    REQUIRE(status.uptime == 987s);
    REQUIRE(status.lastError == "prior restart recovered");
    REQUIRE(status.dashboardHost == "::1");
    REQUIRE(status.dashboardPort == 65'535U);
    REQUIRE(status.home.value() == "C:\\Users\\tester\\.forge-conductor");
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedStatus)) ==
            statusFrame);

    const auto statusRoot = Json::parse(payloadText(statusFrame));
    const auto& statusValue = statusRoot.at("result").at("value");
    REQUIRE(statusValue.size() == 19U);
    REQUIRE(statusValue.contains("started_at_utc_ms"));
    REQUIRE(statusValue.contains("dashboard_refresh_interval_seconds"));
    REQUIRE(statusValue.at("state") == "running");

    const auto settingsFrame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{sampleSettings()})));
    const auto decodedSettings = take(
        Manager::ManagerProtocolCodec::decodeResponse(settingsFrame));
    const auto& settings = std::get<Domain::ManagerSettings>(
        std::get<Manager::ManagerResult>(decodedSettings.body));
    REQUIRE(settings.dashboardHost == "::1");
    REQUIRE(settings.dashboardPort == 65'535U);
    REQUIRE(settings.dashboardRefreshInterval == 17s);
    REQUIRE(!settings.autoRestart);
    REQUIRE(settings.watchdogInterval == 11s);
    REQUIRE(settings.openBrowserOnStart);
    REQUIRE(settings.sessionIdleTtl == 23'456s);
    REQUIRE(settings.shellTimeout == 119s);
    REQUIRE(settings.logLevel == Domain::LogLevel::Critical);
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedSettings)) ==
            settingsFrame);

    const auto acknowledgementFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{Manager::ManagerAcknowledgement{false}})));
    const auto acknowledgement = take(
        Manager::ManagerProtocolCodec::decodeResponse(acknowledgementFrame));
    REQUIRE(!std::get<Manager::ManagerAcknowledgement>(
                 std::get<Manager::ManagerResult>(acknowledgement.body))
                 .acknowledged);

    Manager::ManagerResponse errorResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{Domain::makeError(
            Domain::ErrorCodes::Conflict,
            "manager transition conflict",
            true,
            "manager-evidence-17")}};
    const auto errorFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(errorResponse));
    const auto decodedError = take(
        Manager::ManagerProtocolCodec::decodeResponse(errorFrame));
    REQUIRE(std::holds_alternative<Domain::Error>(decodedError.body));
    REQUIRE(std::get<Domain::Error>(decodedError.body) ==
            std::get<Domain::Error>(errorResponse.body));
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeResponse(decodedError)) ==
            errorFrame);
}

void testSettingsUpdateOutcomeRoundTrips()
{
    for (const bool applied : {false, true}) {
        for (const bool bindingChanged : {false, true}) {
            const auto original = sampleSettingsUpdateOutcome(
                applied, bindingChanged);
            const auto frame = take(
                Manager::ManagerProtocolCodec::encodeResponse(response(
                    Manager::ManagerResult{original})));
            const auto root = Json::parse(payloadText(frame));
            REQUIRE(root.at("result").at("type") == "settings_update");
            const auto& encoded = root.at("result").at("value");
            REQUIRE(encoded.size() == 4U);
            REQUIRE(encoded.at("applied") == applied);
            REQUIRE(encoded.at("binding_changed") == bindingChanged);
            REQUIRE(encoded.at("settings").is_object());
            REQUIRE(encoded.at("status").is_object());

            const auto decoded = take(
                Manager::ManagerProtocolCodec::decodeResponse(frame));
            const auto& result = std::get<Manager::ManagerResult>(decoded.body);
            REQUIRE(std::holds_alternative<
                    Domain::ManagerSettingsUpdateOutcome>(result));
            const auto& outcome =
                std::get<Domain::ManagerSettingsUpdateOutcome>(result);
            REQUIRE(outcome.applied == applied);
            REQUIRE(outcome.bindingChanged == bindingChanged);
            REQUIRE(outcome.settings.dashboardHost == "::1");
            REQUIRE(outcome.settings.dashboardPort == 65'535U);
            REQUIRE(outcome.settings.logLevel == Domain::LogLevel::Critical);
            REQUIRE(outcome.status.state == Domain::ManagerServiceState::Running);
            REQUIRE(outcome.status.processId == 42'424U);
            REQUIRE(take(
                Manager::ManagerProtocolCodec::encodeResponse(decoded)) == frame);
        }
    }
}

void testNullOptionalFieldsAreLossless()
{
    auto status = sampleStatus();
    status.startedAt.reset();
    status.uptime.reset();
    status.lastError.reset();
    const auto frame = take(Manager::ManagerProtocolCodec::encodeResponse(
        response(Manager::ManagerResult{status})));
    const auto root = Json::parse(payloadText(frame));
    const auto& value = root.at("result").at("value");
    REQUIRE(value.at("started_at_utc_ms").is_null());
    REQUIRE(value.at("uptime_seconds").is_null());
    REQUIRE(value.at("last_error").is_null());
    const auto decoded = take(Manager::ManagerProtocolCodec::decodeResponse(frame));
    const auto& actual = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(decoded.body));
    REQUIRE(!actual.startedAt && !actual.uptime && !actual.lastError);

    Domain::ManagerSettingsPatch emptyPatch;
    const auto patchFrame = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerSettingsUpdateRequest{emptyPatch, false})));
    const auto patchRoot = Json::parse(payloadText(patchFrame));
    const auto& patch = patchRoot.at("params").at("patch");
    REQUIRE(patch.size() == 9U);
    for (const auto& field : patch) REQUIRE(field.is_null());
    const auto decodedPatch = take(
        Manager::ManagerProtocolCodec::decodeRequest(patchFrame));
    const auto& actualPatch =
        std::get<Manager::ManagerSettingsUpdateRequest>(decodedPatch.payload).patch;
    REQUIRE(!actualPatch.dashboardHost && !actualPatch.dashboardPort);
    REQUIRE(!actualPatch.autoRestart && !actualPatch.logLevel);
}

void testTimestampPrecisionAndRepresentableBounds()
{
    using ClockDuration = Domain::UtcTimePoint::duration;

    const auto epochMilliseconds = std::chrono::milliseconds{1'767'225'600'123LL};
    const auto epochClockDuration =
        std::chrono::duration_cast<ClockDuration>(epochMilliseconds);
    const auto subMillisecondTick = ClockDuration{1};
    REQUIRE(subMillisecondTick > ClockDuration::zero());
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(
                subMillisecondTick) == std::chrono::milliseconds::zero());

    auto status = sampleStatus();
    status.startedAt = Domain::UtcTimePoint{
        epochClockDuration + subMillisecondTick};
    const auto subMillisecondFrame = take(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{status})));
    const auto subMillisecondJson = Json::parse(payloadText(subMillisecondFrame));
    REQUIRE(subMillisecondJson.at("result").at("value").at(
                "started_at_utc_ms") == epochMilliseconds.count());
    const auto subMillisecondDecoded = take(
        Manager::ManagerProtocolCodec::decodeResponse(subMillisecondFrame));
    const auto& truncatedStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(subMillisecondDecoded.body));
    REQUIRE(truncatedStatus.startedAt ==
            std::optional<Domain::UtcTimePoint>{
                Domain::UtcTimePoint{epochClockDuration}});

    const auto maximumMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ClockDuration::max());
    const auto maximumCount =
        static_cast<std::int64_t>(maximumMilliseconds.count());
    REQUIRE(maximumCount < (std::numeric_limits<std::int64_t>::max)());

    auto root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["started_at_utc_ms"] = 0;
    const auto epochResponse = take(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)));
    const auto& epochStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(epochResponse.body));
    REQUIRE(epochStatus.startedAt == Domain::UtcTimePoint{});

    root["result"]["value"]["started_at_utc_ms"] = maximumCount;
    const auto maximumResponse = take(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)));
    const auto& maximumStatus = std::get<Domain::ManagerStatus>(
        std::get<Manager::ManagerResult>(maximumResponse.body));
    REQUIRE(maximumStatus.startedAt ==
            Domain::UtcTimePoint{std::chrono::duration_cast<ClockDuration>(
                maximumMilliseconds)});
    const auto maximumRoundTrip = take(
        Manager::ManagerProtocolCodec::encodeResponse(maximumResponse));
    REQUIRE(Json::parse(payloadText(maximumRoundTrip))
                .at("result")
                .at("value")
                .at("started_at_utc_ms") == maximumCount);

    root["result"]["value"]["started_at_utc_ms"] = maximumCount + 1;
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
    root["result"]["value"]["started_at_utc_ms"] =
        (std::numeric_limits<std::int64_t>::max)();
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
    root["result"]["value"]["started_at_utc_ms"] =
        (std::numeric_limits<std::int64_t>::min)();
    requireError(
        Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
        Domain::ErrorCodes::InvalidRequest);
}

void testHostileFramingAndJsonAreRejected()
{
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest({}),
        Domain::ErrorCodes::MalformedMessage);
    for (std::size_t size = 1U; size < 4U; ++size) {
        const std::vector<std::byte> incomplete(size, std::byte{});
        requireError(
            Manager::ManagerProtocolCodec::decodeRequest(incomplete),
            Domain::ErrorCodes::MalformedMessage);
    }
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(
            std::vector<std::byte>(4U, std::byte{})),
        Domain::ErrorCodes::MalformedMessage);

    auto valid = take(Manager::ManagerProtocolCodec::encodeRequest(
        request(Manager::ManagerStatusRequest{})));
    auto incomplete = valid;
    incomplete.pop_back();
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(incomplete),
        Domain::ErrorCodes::MalformedMessage);
    auto trailing = valid;
    trailing.push_back(static_cast<std::byte>('x'));
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(trailing),
        Domain::ErrorCodes::MalformedMessage);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(valid, valid.size() - 5U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(valid, 0U),
        Domain::ErrorCodes::InvalidRequest);

    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText("{not-json")),
        Domain::ErrorCodes::MalformedMessage);
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText("[]")),
        Domain::ErrorCodes::InvalidRequest);

    std::string invalidUtf8{"{\"x\":\""};
    invalidUtf8.push_back(static_cast<char>(0xc0));
    invalidUtf8.push_back(static_cast<char>(0xaf));
    invalidUtf8 += "\"}";
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(invalidUtf8)),
        Domain::ErrorCodes::MalformedMessage);

    std::string embeddedNul{"{\"x\":\"a"};
    embeddedNul.push_back('\0');
    embeddedNul += "b\"}";
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(embeddedNul)),
        Domain::ErrorCodes::MalformedMessage);

    auto escapedNul = requestJson(Manager::ManagerStatusRequest{}).dump();
    replaceOne(
        escapedNul,
        "\"manager-request-7\"",
        "\"manager\\u0000request\"");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(escapedNul)),
        Domain::ErrorCodes::MalformedMessage);

    auto duplicateRoot = requestJson(Manager::ManagerStatusRequest{}).dump();
    duplicateRoot.insert(duplicateRoot.size() - 1U, ",\"version\":1");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(duplicateRoot)),
        Domain::ErrorCodes::MalformedMessage);

    auto duplicateNested =
        requestJson(Domain::ManagerControlRequest{
            Domain::ManagerControlAction::Start})
            .dump();
    replaceOne(
        duplicateNested,
        "\"params\":{\"action\":\"start\"}",
        "\"params\":{\"action\":\"start\",\"action\":\"stop\"}");
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(duplicateNested)),
        Domain::ErrorCodes::MalformedMessage);

    std::string depth64(64U, '[');
    depth64 += "0";
    depth64.append(64U, ']');
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(depth64)),
        Domain::ErrorCodes::InvalidRequest);
    std::string depth65(65U, '[');
    depth65 += "0";
    depth65.append(65U, ']');
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromText(depth65)),
        Domain::ErrorCodes::LimitExceeded);
}

void testHostileRequestSchemaAndIdentityAreRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeRequest(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };

    auto root = requestJson(Manager::ManagerStatusRequest{});
    root["unknown"] = true;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root.erase("method");
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["version"] = 2;
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frameFromJson(root)),
        Domain::ErrorCodes::UnsupportedVersion);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["request_id"] = "has spaces";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["correlation_id"] = "";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["nonce"] = std::string(64U, 'A');
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["nonce"] = std::string(63U, 'a');
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["deadline_utc_ms"] = -1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["deadline_utc_ms"] = 1.5;
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["method"] = "manager.unknown";
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["params"] = Json::array();
    expectInvalid(root);
    root = requestJson(Manager::ManagerStatusRequest{});
    root["params"]["unknown"] = true;
    expectInvalid(root);

    root = requestJson(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Start});
    root["params"]["action"] = "invalid";
    expectInvalid(root);
    root = requestJson(Domain::ManagerControlRequest{
        Domain::ManagerControlAction::Start});
    root["params"]["unknown"] = false;
    expectInvalid(root);

    root = requestJson(Manager::ManagerCancelRequest{
        identifier<Domain::OperationId>(
            "10000000-0000-4000-8000-000000000016")});
    root["params"]["operation_id"] = "not-a-uuid";
    expectInvalid(root);

    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["unknown"] = 1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["unknown"] = 1;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["dashboard_port"] = 0;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["shell_timeout_seconds"] = 121;
    expectInvalid(root);
    root = requestJson(Manager::ManagerSettingsUpdateRequest{
        samplePatch(), false});
    root["params"]["patch"]["log_level"] = "warning";
    expectInvalid(root);
}

void testHostileResponseSchemasAndModelsAreRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };

    auto root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["error"] = Json{
        {"code", "conflict"},
        {"evidence_id", nullptr},
        {"message", "both"},
        {"retryable", false}};
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root.erase("result");
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["type"] = "unknown";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{
        Manager::ManagerAcknowledgement{true}});
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);

    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["shell_timeout_seconds"] = 121;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleSettings()});
    root["result"]["value"]["log_level"] = "warning";
    expectInvalid(root);

    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["unknown"] = 1;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["state"] = "unknown";
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["uptime_seconds"] = -1;
    expectInvalid(root);
    root = responseJson(Manager::ManagerResult{sampleStatus()});
    root["result"]["value"]["process_id"] =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1U;
    expectInvalid(root);

    Manager::ManagerResponse errorResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "failed", false, "evidence-1")}};
    root = Json::parse(payloadText(take(
        Manager::ManagerProtocolCodec::encodeResponse(errorResponse))));
    root["error"]["unknown"] = true;
    expectInvalid(root);
    root.erase("unknown");
    root["error"].erase("unknown");
    root["error"]["code"] = "not a code";
    expectInvalid(root);
    root["error"]["code"] = "conflict";
    root["error"]["evidence_id"] = "bad evidence";
    expectInvalid(root);
}

void testHostileSettingsUpdateOutcomeIsRejected()
{
    const auto expectInvalid = [](Json root) {
        requireError(
            Manager::ManagerProtocolCodec::decodeResponse(frameFromJson(root)),
            Domain::ErrorCodes::InvalidRequest);
    };
    const auto valid = [] {
        return responseJson(Manager::ManagerResult{
            sampleSettingsUpdateOutcome(true, false)});
    };

    auto root = valid();
    root["result"]["value"].erase("settings");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("applied");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("binding_changed");
    expectInvalid(root);
    root = valid();
    root["result"]["value"].erase("status");
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["unknown"] = true;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["applied"] = "true";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["binding_changed"] = 1;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"] = nullptr;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["status"] = Json::array();
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["dashboard_host"] = "0.0.0.0";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["status"]["state"] = "unknown";
    expectInvalid(root);

    root = valid();
    root["result"]["value"]["settings"]["dashboard_host"] = "127.0.0.1";
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["dashboard_port"] = 7788;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]
        ["dashboard_refresh_interval_seconds"] = 18;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["auto_restart"] = true;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["watchdog_interval_seconds"] = 12;
    expectInvalid(root);
    root = valid();
    root["result"]["value"]["settings"]["open_browser_on_start"] = false;
    expectInvalid(root);
}

void testInvalidTypedModelsFailClosed()
{
    auto invalidVersion = request(Manager::ManagerStatusRequest{});
    invalidVersion.version = 2U;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(invalidVersion),
        Domain::ErrorCodes::UnsupportedVersion);

    auto negativeDeadline = request(Manager::ManagerStatusRequest{});
    negativeDeadline.deadlineUtcMilliseconds = -1;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(negativeDeadline),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidControl = request(Domain::ManagerControlRequest{
        static_cast<Domain::ManagerControlAction>(255)});
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(invalidControl),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidPatch = samplePatch();
    invalidPatch.shellTimeout = 121s;
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(request(
            Manager::ManagerSettingsUpdateRequest{invalidPatch, false})),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidSettings = sampleSettings();
    invalidSettings.dashboardHost = "0.0.0.0";
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidSettings})),
        Domain::ErrorCodes::InvalidRequest);

    auto inconsistentOutcome = sampleSettingsUpdateOutcome(false, true);
    inconsistentOutcome.settings.dashboardPort = 7788U;
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{inconsistentOutcome})),
        Domain::ErrorCodes::InvalidRequest);

    auto invalidStatus = sampleStatus();
    invalidStatus.state = static_cast<Domain::ManagerServiceState>(255);
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    invalidStatus = sampleStatus();
    invalidStatus.version = std::string{"bad\0version", 11U};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    invalidStatus = sampleStatus();
    invalidStatus.version = std::string{
        {static_cast<char>(0xc3), static_cast<char>(0x28)}};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(response(
            Manager::ManagerResult{invalidStatus})),
        Domain::ErrorCodes::InvalidRequest);

    Manager::ManagerResponse invalidError{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-7"),
        identifier<Domain::CorrelationId>("manager-correlation-7"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "failed", false, "bad evidence")}};
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(invalidError),
        Domain::ErrorCodes::InvalidRequest);
}

void testExactMaximumBoundBehavior()
{
    const auto normal = request(Manager::ManagerStatusRequest{});
    const auto frame = take(Manager::ManagerProtocolCodec::encodeRequest(normal));
    const auto payloadBytes = frame.size() - 4U;
    REQUIRE(take(Manager::ManagerProtocolCodec::encodeRequest(
                    normal, payloadBytes)) == frame);
    requireError(
        Manager::ManagerProtocolCodec::encodeRequest(normal, payloadBytes - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(Manager::ManagerProtocolCodec::decodeRequest(frame, payloadBytes));
    requireError(
        Manager::ManagerProtocolCodec::decodeRequest(frame, payloadBytes - 1U),
        Domain::ErrorCodes::PayloadTooLarge);

    Manager::ManagerResponse exactResponse{
        Manager::ManagerProtocolVersion,
        identifier<Domain::RequestId>("manager-request-maximum"),
        identifier<Domain::CorrelationId>("manager-correlation-maximum"),
        Manager::ManagerResponseBody{
            Domain::makeError("conflict", "", false, std::nullopt)}};
    const auto baseline = take(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse));
    const auto baselinePayloadBytes = baseline.size() - 4U;
    REQUIRE(baselinePayloadBytes <
            Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes);
    auto& error = std::get<Domain::Error>(exactResponse.body);
    error.message.assign(
        Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes -
            baselinePayloadBytes,
        'x');
    const auto exact = take(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse));
    REQUIRE(exact.size() ==
            Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes + 4U);
    REQUIRE(Manager::ManagerProtocolCodec::decodeResponse(exact));
    error.message.push_back('x');
    requireError(
        Manager::ManagerProtocolCodec::encodeResponse(exactResponse),
        Domain::ErrorCodes::PayloadTooLarge);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"type-and-prefix", testTypeAndPrefixContract},
        {"request-round-trips", testEveryRequestMethodRoundTripsDeterministically},
        {"response-round-trips", testResponseResultAndErrorRoundTrips},
        {"settings-update-outcome-round-trips",
         testSettingsUpdateOutcomeRoundTrips},
        {"optional-fields", testNullOptionalFieldsAreLossless},
        {"timestamp-precision-bounds", testTimestampPrecisionAndRepresentableBounds},
        {"hostile-framing-json", testHostileFramingAndJsonAreRejected},
        {"hostile-request", testHostileRequestSchemaAndIdentityAreRejected},
        {"hostile-response", testHostileResponseSchemasAndModelsAreRejected},
        {"hostile-settings-update-outcome",
         testHostileSettingsUpdateOutcomeIsRejected},
        {"invalid-models", testInvalidTypedModelsFailClosed},
        {"exact-maximum", testExactMaximumBoundBehavior}};

    try {
        for (const auto& [name, run] : tests) {
            run();
            std::cout << "PASS " << name << '\n';
        }
        std::cout << "PASS manager protocol codec assertions=" << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL manager protocol codec: " << error.what() << '\n';
        return 1;
    }
}
