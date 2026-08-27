#include "ForgeConductor/Dashboard/DashboardStreamQueryDecoder.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view StreamPath{"/api/stream"};
constexpr std::string_view HertzPrefix{"hz="};
constexpr std::string_view IntervalPrefix{"interval="};

struct NumericParseResult final {
    double value{};
    DashboardStreamQueryDecodeError error{
        DashboardStreamQueryDecodeError::None};
};

[[nodiscard]] constexpr DashboardStreamQueryDecodeResult reject(
    const DashboardStreamQueryDecodeError error) noexcept
{
    return DashboardStreamQueryDecodeResult::rejected(error);
}

[[nodiscard]] constexpr bool isControlByte(const char character) noexcept
{
    const auto byte = static_cast<unsigned char>(character);
    return byte < 0x20U || byte == 0x7fU;
}

[[nodiscard]] constexpr char lowerAscii(const char character) noexcept
{
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character + ('a' - 'A'))
        : character;
}

[[nodiscard]] bool equalsAsciiCaseInsensitive(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index{}; index < left.size(); ++index) {
        if (lowerAscii(left[index]) != lowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isNonFiniteSpelling(std::string_view value) noexcept
{
    if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
        value.remove_prefix(1U);
    }
    return equalsAsciiCaseInsensitive(value, "nan") ||
        equalsAsciiCaseInsensitive(value, "inf") ||
        equalsAsciiCaseInsensitive(value, "infinity");
}

struct NumericGrammar final {
    bool valid{};
    bool significandIsZero{true};
    std::int64_t scientificExponent{};
};

[[nodiscard]] NumericGrammar inspectNumericGrammar(
    const std::string_view value) noexcept
{
    NumericGrammar result;
    if (value.empty()) {
        return result;
    }

    std::size_t cursor{};
    if (value[cursor] == '-') {
        ++cursor;
        if (cursor == value.size()) {
            return result;
        }
    }

    const std::size_t integerStart = cursor;
    if (value[cursor] == '0') {
        ++cursor;
        if (cursor < value.size() && value[cursor] >= '0' &&
            value[cursor] <= '9') {
            return result;
        }
    } else if (value[cursor] >= '1' && value[cursor] <= '9') {
        do {
            ++cursor;
        } while (cursor < value.size() && value[cursor] >= '0' &&
                 value[cursor] <= '9');
    } else {
        return result;
    }
    const std::size_t integerEnd = cursor;

    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        const std::size_t fractionalStart = cursor;
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == fractionalStart) {
            return result;
        }
    }
    const std::size_t significandEnd = cursor;

    std::int64_t explicitExponent{};
    if (cursor < value.size() &&
        (value[cursor] == 'e' || value[cursor] == 'E')) {
        ++cursor;
        bool exponentNegative{};
        if (cursor < value.size() &&
            (value[cursor] == '+' || value[cursor] == '-')) {
            exponentNegative = value[cursor] == '-';
            ++cursor;
        }
        const std::size_t exponentStart = cursor;
        while (cursor < value.size() && value[cursor] >= '0' &&
               value[cursor] <= '9') {
            const auto digit = static_cast<std::int64_t>(value[cursor] - '0');
            constexpr auto Saturation = static_cast<std::int64_t>(1'000'000);
            if (explicitExponent < Saturation) {
                explicitExponent = (std::min)(
                    Saturation,
                    explicitExponent * 10 + digit);
            }
            ++cursor;
        }
        if (cursor == exponentStart) {
            return result;
        }
        if (exponentNegative) {
            explicitExponent = -explicitExponent;
        }
    }
    if (cursor != value.size()) {
        return result;
    }

    std::size_t digitOrdinal{};
    std::size_t firstNonzeroOrdinal{};
    bool foundNonzero{};
    for (std::size_t index = integerStart; index < significandEnd; ++index) {
        if (value[index] == '.') {
            continue;
        }
        if (!foundNonzero && value[index] != '0') {
            firstNonzeroOrdinal = digitOrdinal;
            foundNonzero = true;
        }
        ++digitOrdinal;
    }

    result.valid = true;
    result.significandIsZero = !foundNonzero;
    if (foundNonzero) {
        const auto integerDigits = static_cast<std::int64_t>(
            integerEnd - integerStart);
        const auto firstNonzero = static_cast<std::int64_t>(firstNonzeroOrdinal);
        result.scientificExponent = integerDigits - firstNonzero - 1 +
            explicitExponent;
    }
    return result;
}

[[nodiscard]] NumericParseResult parseNumber(
    const std::string_view value) noexcept
{
    if (value.empty()) {
        return {0.0, DashboardStreamQueryDecodeError::MissingValue};
    }
    if (isNonFiniteSpelling(value)) {
        return {0.0, DashboardStreamQueryDecodeError::NonFiniteNumericValue};
    }

    const auto grammar = inspectNumericGrammar(value);
    if (!grammar.valid) {
        return {0.0, DashboardStreamQueryDecodeError::InvalidNumericSyntax};
    }
    if (grammar.significandIsZero) {
        return {0.0, DashboardStreamQueryDecodeError::None};
    }

    double parsed{};
    const auto conversion = std::from_chars(
        value.data(), value.data() + value.size(), parsed,
        std::chars_format::general);
    if (conversion.ec == std::errc::result_out_of_range) {
        return {
            0.0,
            grammar.scientificExponent < 0
                ? DashboardStreamQueryDecodeError::NumericUnderflow
                : DashboardStreamQueryDecodeError::NumericOverflow};
    }
    if (conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size()) {
        return {0.0, DashboardStreamQueryDecodeError::InvalidNumericSyntax};
    }
    if (!std::isfinite(parsed)) {
        return {0.0, DashboardStreamQueryDecodeError::NonFiniteNumericValue};
    }
    return {parsed, DashboardStreamQueryDecodeError::None};
}

[[nodiscard]] DashboardRouteQuery classifyRawQuery(
    const std::string_view rawQuery,
    const bool hasQuery) noexcept
{
    if (!hasQuery) {
        return DashboardRouteQuery::None;
    }
    const bool hasExtraDelimiter =
        rawQuery.find('&') != std::string_view::npos ||
        rawQuery.find('?') != std::string_view::npos;
    if (rawQuery.starts_with(HertzPrefix) && !hasExtraDelimiter &&
        rawQuery.find('=', HertzPrefix.size()) == std::string_view::npos) {
        return DashboardRouteQuery::StreamHertz;
    }
    if (rawQuery.starts_with(IntervalPrefix) && !hasExtraDelimiter &&
        rawQuery.find('=', IntervalPrefix.size()) == std::string_view::npos) {
        return DashboardRouteQuery::LegacyStreamInterval;
    }
    return DashboardRouteQuery::UnsupportedStreamQuery;
}

[[nodiscard]] DashboardStreamQueryDecodeError unsupportedQueryError(
    const std::string_view rawQuery) noexcept
{
    if (rawQuery.empty()) {
        return DashboardStreamQueryDecodeError::MalformedQuery;
    }
    if (rawQuery.find('&') != std::string_view::npos ||
        rawQuery.find('?') != std::string_view::npos ||
        rawQuery.find('=', rawQuery.find('=') + 1U) != std::string_view::npos) {
        return DashboardStreamQueryDecodeError::ExtraQueryComponent;
    }
    const auto equals = rawQuery.find('=');
    if (equals == std::string_view::npos || equals == 0U) {
        return DashboardStreamQueryDecodeError::MalformedQuery;
    }
    return DashboardStreamQueryDecodeError::UnsupportedQuery;
}

[[nodiscard]] bool validResourceCap(const double value) noexcept
{
    return value == 1.0 || value == 2.0;
}

[[nodiscard]] DashboardStreamQueryDecodeResult accepted(
    const double normalizedRequestedHz,
    const DashboardStreamRateSource source,
    const bool compatibilityClamped,
    const double resourceCap) noexcept
{
    const bool resourceCapped = normalizedRequestedHz > resourceCap;
    return DashboardStreamQueryDecodeResult::accepted(
        DashboardStreamRateSelection{
            normalizedRequestedHz,
            (std::min)(normalizedRequestedHz, resourceCap),
            source,
            compatibilityClamped,
            resourceCapped});
}

} // namespace

DashboardStreamQueryDecodeResult DashboardStreamQueryDecoder::decode(
    const DashboardRouteQuery queryClassification,
    const std::string_view rawTarget,
    const Domain::ResourceBudgets& budgets) noexcept
{
    if (rawTarget.size() > MaximumTargetBytes) {
        return reject(DashboardStreamQueryDecodeError::TargetTooLong);
    }
    for (const char character : rawTarget) {
        if (isControlByte(character)) {
            return reject(
                DashboardStreamQueryDecodeError::ControlCharacterNotAllowed);
        }
        if (character == '%') {
            return reject(
                DashboardStreamQueryDecodeError::PercentEncodingNotAllowed);
        }
    }

    const auto queryDelimiter = rawTarget.find('?');
    const bool hasQuery = queryDelimiter != std::string_view::npos;
    const auto rawPath = rawTarget.substr(0U, queryDelimiter);
    if (rawPath != StreamPath || rawTarget.empty() ||
        rawTarget.front() != '/' || rawTarget.find('#') != std::string_view::npos ||
        rawTarget.find('\\') != std::string_view::npos) {
        return reject(DashboardStreamQueryDecodeError::MalformedTarget);
    }
    const auto rawQuery = hasQuery
        ? rawTarget.substr(queryDelimiter + 1U)
        : std::string_view{};
    const auto actualClassification = classifyRawQuery(rawQuery, hasQuery);
    if (queryClassification != actualClassification) {
        return reject(
            DashboardStreamQueryDecodeError::QueryClassificationMismatch);
    }
    if (actualClassification == DashboardRouteQuery::UnsupportedStreamQuery) {
        return reject(unsupportedQueryError(rawQuery));
    }
    if (!validResourceCap(budgets.telemetrySampleHz)) {
        return reject(DashboardStreamQueryDecodeError::InvalidResourceBudget);
    }

    if (actualClassification == DashboardRouteQuery::None) {
        return accepted(
            DefaultRequestedHz,
            DashboardStreamRateSource::ProfileDefault,
            false,
            budgets.telemetrySampleHz);
    }

    const auto value = actualClassification == DashboardRouteQuery::StreamHertz
        ? rawQuery.substr(HertzPrefix.size())
        : rawQuery.substr(IntervalPrefix.size());
    const auto parsed = parseNumber(value);
    if (parsed.error != DashboardStreamQueryDecodeError::None) {
        return reject(parsed.error);
    }

    if (actualClassification == DashboardRouteQuery::StreamHertz) {
        const auto normalized = (std::clamp)(
            parsed.value, MinimumRequestedHz, MaximumRequestedHz);
        return accepted(
            normalized,
            DashboardStreamRateSource::ExplicitHertz,
            normalized != parsed.value,
            budgets.telemetrySampleHz);
    }

    const auto period = (std::clamp)(
        parsed.value,
        MinimumLegacyIntervalSeconds,
        MaximumLegacyIntervalSeconds);
    return accepted(
        1.0 / period,
        DashboardStreamRateSource::LegacyInterval,
        period != parsed.value,
        budgets.telemetrySampleHz);
}

} // namespace ForgeConductor::Dashboard
