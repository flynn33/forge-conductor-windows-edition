#include "ForgeConductor/Dashboard/DashboardSessionCloseRequestDecoder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {
namespace {

using Json = nlohmann::json;

class SessionCloseDecodeException final : public std::runtime_error {
public:
    SessionCloseDecodeException(std::string code, std::string message)
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
    throw SessionCloseDecodeException{
        std::string{code}, std::string{message}};
}

[[nodiscard]] std::optional<std::size_t> strictUtf8UnitCount(
    const std::string_view value) noexcept
{
    std::size_t index{};
    std::size_t units{};
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            ++units;
            continue;
        }

        const auto continuation = [&](const std::size_t offset) noexcept {
            return index + offset < value.size() &&
                (static_cast<unsigned char>(value[index + offset]) & 0xc0U) ==
                    0x80U;
        };
        if (first >= 0xc2U && first <= 0xdfU) {
            if (!continuation(1U)) return std::nullopt;
            index += 2U;
        } else if (first == 0xe0U) {
            if (index + 2U >= value.size()) return std::nullopt;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0xa0U || second > 0xbfU || !continuation(2U)) {
                return std::nullopt;
            }
            index += 3U;
        } else if ((first >= 0xe1U && first <= 0xecU) ||
                   (first >= 0xeeU && first <= 0xefU)) {
            if (!continuation(1U) || !continuation(2U)) return std::nullopt;
            index += 3U;
        } else if (first == 0xedU) {
            if (index + 2U >= value.size()) return std::nullopt;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x9fU || !continuation(2U)) {
                return std::nullopt;
            }
            index += 3U;
        } else if (first == 0xf0U) {
            if (index + 3U >= value.size()) return std::nullopt;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x90U || second > 0xbfU || !continuation(2U) ||
                !continuation(3U)) {
                return std::nullopt;
            }
            index += 4U;
        } else if (first >= 0xf1U && first <= 0xf3U) {
            if (!continuation(1U) || !continuation(2U) ||
                !continuation(3U)) {
                return std::nullopt;
            }
            index += 4U;
        } else if (first == 0xf4U) {
            if (index + 3U >= value.size()) return std::nullopt;
            const auto second = static_cast<unsigned char>(value[index + 1U]);
            if (second < 0x80U || second > 0x8fU || !continuation(2U) ||
                !continuation(3U)) {
                return std::nullopt;
            }
            index += 4U;
        } else {
            return std::nullopt;
        }
        ++units;
    }
    return units;
}

[[nodiscard]] bool containsNul(const std::string_view value) noexcept
{
    return value.find('\0') != std::string_view::npos;
}

class StrictSessionCloseJsonSax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }

    bool string(string_t& value) override
    {
        if (containsNul(value) || !strictUtf8UnitCount(value)) {
            invalidText_ = true;
            return false;
        }
        return true;
    }

    bool binary(binary_t&) override { return true; }

    bool start_object(std::size_t) override
    {
        if (!startContainer()) return false;
        containers_.push_back(Container{true, {}});
        return true;
    }

    bool key(string_t& name) override
    {
        if (containsNul(name) || !strictUtf8UnitCount(name)) {
            invalidText_ = true;
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
        if (containers_.empty() || !containers_.back().isObject || depth_ == 0U) {
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
        if (containers_.empty() || containers_.back().isObject || depth_ == 0U) {
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
    [[nodiscard]] bool invalidText() const noexcept { return invalidText_; }
    [[nodiscard]] bool tooDeep() const noexcept { return tooDeep_; }
    [[nodiscard]] bool malformed() const noexcept { return malformed_; }

private:
    struct Container final {
        bool isObject{};
        std::unordered_set<std::string> keys;
    };

    [[nodiscard]] bool startContainer() noexcept
    {
        ++depth_;
        if (depth_ >
            DashboardSessionCloseRequestDecoder::MaximumJsonNesting) {
            tooDeep_ = true;
            return false;
        }
        return true;
    }

    std::vector<Container> containers_;
    std::size_t depth_{};
    bool duplicateKey_{};
    bool invalidText_{};
    bool tooDeep_{};
    bool malformed_{};
};

[[nodiscard]] Json parseStrictDocument(
    const std::span<const std::byte> body,
    const std::size_t maximumBytes)
{
    if (maximumBytes == 0U ||
        maximumBytes > DashboardSessionCloseRequestDecoder::MaximumRequestBytes) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard session-close request byte limit is invalid.");
    }
    if (body.size() > maximumBytes) {
        reject(
            Domain::ErrorCodes::PayloadTooLarge,
            "Dashboard session-close request exceeds its byte limit.");
    }
    if (body.empty()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request body is empty.");
    }

    std::string payload(body.size(), '\0');
    std::memcpy(payload.data(), body.data(), body.size());
    if (containsNul(payload) || !strictUtf8UnitCount(payload)) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request is not strict NUL-free UTF-8.");
    }

    StrictSessionCloseJsonSax preflight;
    const bool accepted = Json::sax_parse(payload, &preflight);
    if (preflight.duplicateKey()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request contains a duplicate object key.");
    }
    if (preflight.invalidText()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request contains invalid text.");
    }
    if (preflight.tooDeep()) {
        reject(
            Domain::ErrorCodes::LimitExceeded,
            "Dashboard session-close request exceeds its JSON nesting limit.");
    }
    if (!accepted || preflight.malformed()) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request is malformed JSON.");
    }

    try {
        return Json::parse(payload, nullptr, true, false);
    } catch (const Json::exception&) {
        reject(
            Domain::ErrorCodes::MalformedMessage,
            "Dashboard session-close request is malformed JSON.");
    }
}

[[nodiscard]] DashboardSessionCloseRequest mapRequest(const Json& document)
{
    if (!document.is_object()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard session-close request must be a JSON object.");
    }
    for (const auto& [name, unused] : document.items()) {
        static_cast<void>(unused);
        if (name != "session_id" && name != "summary") {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "Dashboard session-close request contains an unknown field.");
        }
    }

    const auto sessionMember = document.find("session_id");
    if (sessionMember == document.end() || !sessionMember->is_string()) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "session_id required");
    }
    const auto& sessionText = sessionMember->get_ref<const std::string&>();
    auto sessionId = Domain::SessionId::parse(sessionText);
    if (!sessionId) {
        reject(
            Domain::ErrorCodes::InvalidRequest,
            "session_id must be a canonical UUID.");
    }

    std::string summary{DashboardSessionCloseRequest::DefaultSummary};
    const auto summaryMember = document.find("summary");
    if (summaryMember != document.end()) {
        if (!summaryMember->is_string()) {
            reject(
                Domain::ErrorCodes::InvalidRequest,
                "summary must be a JSON string.");
        }
        const auto& summaryText = summaryMember->get_ref<const std::string&>();
        const auto units = strictUtf8UnitCount(summaryText);
        if (containsNul(summaryText) || !units) {
            reject(
                Domain::ErrorCodes::MalformedMessage,
                "summary must be strict UTF-8 without NUL bytes.");
        }
        if (*units > Domain::AgentSessionLimits::MaximumSummaryUnits) {
            reject(
                Domain::ErrorCodes::PayloadTooLarge,
                "summary exceeds the agent-session summary limit.");
        }
        summary = summaryText;
    }

    return DashboardSessionCloseRequest{
        std::move(sessionId).value(), std::move(summary)};
}

} // namespace

Domain::Result<DashboardSessionCloseRequest>
DashboardSessionCloseRequestDecoder::decode(
    const std::span<const std::byte> body,
    const std::size_t maximumBytes) noexcept
{
    try {
        return Domain::Result<DashboardSessionCloseRequest>::success(
            mapRequest(parseStrictDocument(body, maximumBytes)));
    } catch (const SessionCloseDecodeException& error) {
        return Domain::Result<DashboardSessionCloseRequest>::failure(
            Domain::makeError(error.code(), error.what()));
    } catch (...) {
        return Domain::Result<DashboardSessionCloseRequest>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard session-close request could not be decoded."));
    }
}

} // namespace ForgeConductor::Dashboard
