#pragma once

#include "ForgeConductor/Dashboard/DashboardRouteCatalog.h"
#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ForgeConductor::Dashboard {

enum class DashboardStreamRateSource : std::uint8_t {
    ProfileDefault,
    ExplicitHertz,
    LegacyInterval,
};

enum class DashboardStreamQueryDecodeError : std::uint8_t {
    None,
    TargetTooLong,
    MalformedTarget,
    QueryClassificationMismatch,
    UnsupportedQuery,
    MalformedQuery,
    ExtraQueryComponent,
    MissingValue,
    PercentEncodingNotAllowed,
    ControlCharacterNotAllowed,
    InvalidNumericSyntax,
    NonFiniteNumericValue,
    NumericUnderflow,
    NumericOverflow,
    InvalidResourceBudget,
};

// The normalized rate preserves the macOS query contract before the selected
// Windows resource profile imposes its lower delivery ceiling.
struct DashboardStreamRateSelection final {
    double normalizedRequestedHz{};
    double deliveryHz{};
    DashboardStreamRateSource source{DashboardStreamRateSource::ProfileDefault};
    bool compatibilityClamped{};
    bool resourceCapped{};

    bool operator==(const DashboardStreamRateSelection&) const = default;
};

// Fixed-size result: decoding never copies the untrusted request target or
// allocates storage proportional to it.
class DashboardStreamQueryDecodeResult final {
public:
    [[nodiscard]] static constexpr DashboardStreamQueryDecodeResult accepted(
        const DashboardStreamRateSelection selection) noexcept
    {
        return DashboardStreamQueryDecodeResult{selection};
    }

    [[nodiscard]] static constexpr DashboardStreamQueryDecodeResult rejected(
        const DashboardStreamQueryDecodeError error) noexcept
    {
        return DashboardStreamQueryDecodeResult{error};
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept
    {
        return error_ == DashboardStreamQueryDecodeError::None;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] constexpr const DashboardStreamRateSelection* selection()
        const noexcept
    {
        return hasValue() ? &selection_ : nullptr;
    }

    [[nodiscard]] constexpr DashboardStreamQueryDecodeError error() const noexcept
    {
        return error_;
    }

    bool operator==(const DashboardStreamQueryDecodeResult&) const = default;

private:
    constexpr explicit DashboardStreamQueryDecodeResult(
        const DashboardStreamRateSelection selection) noexcept
        : selection_{selection}
    {
    }

    constexpr explicit DashboardStreamQueryDecodeResult(
        const DashboardStreamQueryDecodeError error) noexcept
        : error_{error}
    {
    }

    DashboardStreamRateSelection selection_{};
    DashboardStreamQueryDecodeError error_{};
};

class DashboardStreamQueryDecoder final {
public:
    static constexpr std::size_t MaximumTargetBytes = 8U * 1024U;
    static constexpr double DefaultRequestedHz = 20.0;
    static constexpr double MinimumRequestedHz = 1.0;
    static constexpr double MaximumRequestedHz = 60.0;
    static constexpr double MinimumLegacyIntervalSeconds = 0.016;
    static constexpr double MaximumLegacyIntervalSeconds = 0.1;

    // rawTarget is the undecoded origin-form target classified by
    // DashboardRouteCatalog. The caller must provide the classification from
    // that same target; a mismatch is rejected rather than reinterpreted.
    [[nodiscard]] static DashboardStreamQueryDecodeResult decode(
        DashboardRouteQuery queryClassification,
        std::string_view rawTarget,
        const Domain::ResourceBudgets& budgets) noexcept;
};

} // namespace ForgeConductor::Dashboard
