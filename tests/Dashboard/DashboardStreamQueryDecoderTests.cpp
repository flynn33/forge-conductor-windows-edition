#include "ForgeConductor/Dashboard/DashboardStreamQueryDecoder.h"

#include "ForgeConductor/Domain/ResourcePolicy.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

static_assert(std::is_final_v<Dashboard::DashboardStreamQueryDecoder>);
static_assert(std::is_final_v<Dashboard::DashboardStreamQueryDecodeResult>);
static_assert(std::is_final_v<Dashboard::DashboardStreamRateSelection>);
static_assert(std::is_trivially_copyable_v<
              Dashboard::DashboardStreamQueryDecodeResult>);
static_assert(sizeof(Dashboard::DashboardStreamQueryDecodeResult) <= 40U);
static_assert(noexcept(Dashboard::DashboardStreamQueryDecoder::decode(
    std::declval<Dashboard::DashboardRouteQuery>(),
    std::declval<std::string_view>(),
    std::declval<const Domain::ResourceBudgets&>())));

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::size_t line)
{
    throw std::runtime_error{
        "requirement failed at line " + std::to_string(line) + ": " +
        std::string{expression}};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::size_t line)
{
    ++assertions;
    if (!condition) {
        fail(expression, line);
    }
}

#define REQUIRE(condition) \
    require(static_cast<bool>(condition), #condition, __LINE__)

[[nodiscard]] bool close(
    const double left,
    const double right,
    const double tolerance = 1.0e-12) noexcept
{
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] Dashboard::DashboardStreamQueryDecodeResult decode(
    const std::string_view target,
    const Domain::ResourceBudgets& budgets = Domain::budgetsForProfile(
        Domain::ResourceProfile::Standard16GiB)) noexcept
{
    const auto route = Dashboard::DashboardRouteCatalog::classify(
        "GET", target, true);
    return Dashboard::DashboardStreamQueryDecoder::decode(
        route.query(), target, budgets);
}

[[nodiscard]] const Dashboard::DashboardStreamRateSelection& requireAccepted(
    const Dashboard::DashboardStreamQueryDecodeResult& result)
{
    REQUIRE(result.hasValue());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.error() == Dashboard::DashboardStreamQueryDecodeError::None);
    REQUIRE(result.selection() != nullptr);
    return *result.selection();
}

void requireRejected(
    const Dashboard::DashboardStreamQueryDecodeResult& result,
    const Dashboard::DashboardStreamQueryDecodeError error)
{
    REQUIRE(!result.hasValue());
    REQUIRE(!static_cast<bool>(result));
    REQUIRE(result.error() == error);
    REQUIRE(result.selection() == nullptr);
}

void appliesEverySelectedWindowsProfileToTheDefault()
{
    struct ProfileCase final {
        Domain::ResourceProfile profile;
        double expectedHz;
    };
    constexpr std::array profiles{
        ProfileCase{Domain::ResourceProfile::Constrained8GiB, 1.0},
        ProfileCase{Domain::ResourceProfile::Standard16GiB, 2.0},
        ProfileCase{Domain::ResourceProfile::Expanded32GiBPlus, 2.0},
    };

    for (const auto& entry : profiles) {
        const auto budgets = Domain::budgetsForProfile(entry.profile);
        REQUIRE(budgets.telemetrySampleHz == entry.expectedHz);
        const auto& selection = requireAccepted(decode("/api/stream", budgets));
        REQUIRE(selection.source ==
                Dashboard::DashboardStreamRateSource::ProfileDefault);
        REQUIRE(close(selection.normalizedRequestedHz, 20.0));
        REQUIRE(close(selection.deliveryHz, entry.expectedHz));
        REQUIRE(!selection.compatibilityClamped);
        REQUIRE(selection.resourceCapped);
    }
}

void decodesTheExactHertzGrammarAndClampsCompatibly()
{
    struct HertzCase final {
        std::string_view value;
        double normalized;
        double delivered;
        bool compatibilityClamped;
        bool resourceCapped;
    };
    constexpr std::array cases{
        HertzCase{"0", 1.0, 1.0, true, false},
        HertzCase{"-0", 1.0, 1.0, true, false},
        HertzCase{"-1", 1.0, 1.0, true, false},
        HertzCase{"1", 1.0, 1.0, false, false},
        HertzCase{"1.25", 1.25, 1.25, false, false},
        HertzCase{"1.5", 1.5, 1.5, false, false},
        HertzCase{"1e0", 1.0, 1.0, false, false},
        HertzCase{"1E+0", 1.0, 1.0, false, false},
        HertzCase{"0e999999", 1.0, 1.0, true, false},
        HertzCase{"2.0", 2.0, 2.0, false, false},
        HertzCase{"2.0001", 2.0001, 2.0, false, true},
        HertzCase{"6e1", 60.0, 2.0, false, true},
        HertzCase{"61", 60.0, 2.0, true, true},
        HertzCase{"1e308", 60.0, 2.0, true, true},
    };

    for (const auto& entry : cases) {
        const std::string target = "/api/stream?hz=" + std::string{entry.value};
        const auto& selection = requireAccepted(decode(target));
        REQUIRE(selection.source ==
                Dashboard::DashboardStreamRateSource::ExplicitHertz);
        REQUIRE(close(selection.normalizedRequestedHz, entry.normalized));
        REQUIRE(close(selection.deliveryHz, entry.delivered));
        REQUIRE(selection.compatibilityClamped == entry.compatibilityClamped);
        REQUIRE(selection.resourceCapped == entry.resourceCapped);
    }

    const auto constrained = Domain::budgetsForProfile(
        Domain::ResourceProfile::Constrained8GiB);
    const auto& reduced = requireAccepted(decode(
        "/api/stream?hz=1.5", constrained));
    REQUIRE(close(reduced.normalizedRequestedHz, 1.5));
    REQUIRE(close(reduced.deliveryHz, 1.0));
    REQUIRE(!reduced.compatibilityClamped);
    REQUIRE(reduced.resourceCapped);
}

void preservesTheLegacyIntervalUpgradeBeforeTheWindowsCap()
{
    struct IntervalCase final {
        std::string_view value;
        double normalized;
        bool compatibilityClamped;
    };
    constexpr std::array cases{
        IntervalCase{"-2", 62.5, true},
        IntervalCase{"0", 62.5, true},
        IntervalCase{"1e-2", 62.5, true},
        IntervalCase{"0.016", 62.5, false},
        IntervalCase{"0.02", 50.0, false},
        IntervalCase{"5e-2", 20.0, false},
        IntervalCase{"0.1", 10.0, false},
        IntervalCase{"2", 10.0, true},
    };

    for (const auto& entry : cases) {
        const std::string target =
            "/api/stream?interval=" + std::string{entry.value};
        const auto& selection = requireAccepted(decode(target));
        REQUIRE(selection.source ==
                Dashboard::DashboardStreamRateSource::LegacyInterval);
        REQUIRE(close(selection.normalizedRequestedHz, entry.normalized));
        REQUIRE(close(selection.deliveryHz, 2.0));
        REQUIRE(selection.compatibilityClamped == entry.compatibilityClamped);
        REQUIRE(selection.resourceCapped);
    }

    for (const auto profile : {
             Domain::ResourceProfile::Constrained8GiB,
             Domain::ResourceProfile::Standard16GiB,
             Domain::ResourceProfile::Expanded32GiBPlus}) {
        const auto budgets = Domain::budgetsForProfile(profile);
        const auto& upgraded = requireAccepted(decode(
            "/api/stream?interval=2", budgets));
        REQUIRE(close(upgraded.normalizedRequestedHz, 10.0));
        REQUIRE(close(upgraded.deliveryHz, budgets.telemetrySampleHz));
    }
}

void rejectsMissingMalformedAndNonFiniteNumericValues()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    requireRejected(decode("/api/stream?hz="), Error::MissingValue);
    requireRejected(decode("/api/stream?interval="), Error::MissingValue);

    for (const auto value : {
             "+1", ".5", "1.", "01", "-01", "--1", "e1", "1e",
             "1e+", "1e-", "1..0", "1_0", "1,0", " 1", "1 ",
             "0x1", "true", "one"}) {
        requireRejected(
            decode("/api/stream?hz=" + std::string{value}),
            Error::InvalidNumericSyntax);
    }

    for (const auto value : {
             "nan", "NaN", "+nan", "-NAN", "inf", "INF", "+Infinity",
             "-infinity"}) {
        requireRejected(
            decode("/api/stream?hz=" + std::string{value}),
            Error::NonFiniteNumericValue);
    }

    std::string nonAscii = "/api/stream?hz=";
    nonAscii.push_back(static_cast<char>(0xc2U));
    nonAscii.push_back(static_cast<char>(0xa0U));
    requireRejected(decode(nonAscii), Error::InvalidNumericSyntax);
}

void distinguishesNumericUnderflowFromOverflow()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    for (const auto value : {"1e309", "-1e309", "9e999999"}) {
        requireRejected(
            decode("/api/stream?hz=" + std::string{value}),
            Error::NumericOverflow);
    }
    for (const auto value : {"1e-400", "-1e-400", "9e-999999"}) {
        requireRejected(
            decode("/api/stream?hz=" + std::string{value}),
            Error::NumericUnderflow);
    }

    std::string hugeInteger(400U, '9');
    requireRejected(
        decode("/api/stream?hz=" + hugeInteger),
        Error::NumericOverflow);

    std::string tinyFraction = "0.";
    tinyFraction.append(400U, '0');
    tinyFraction.push_back('1');
    requireRejected(
        decode("/api/stream?interval=" + tinyFraction),
        Error::NumericUnderflow);

    const auto& maximumFinite = requireAccepted(decode(
        "/api/stream?hz=1.7976931348623157e308"));
    REQUIRE(close(maximumFinite.normalizedRequestedHz, 60.0));
    requireRejected(
        decode("/api/stream?hz=1.7976931348623159e308"),
        Error::NumericOverflow);
}

void rejectsEncodedControlledExtraAndUnsupportedQueries()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    for (const auto target : {
             "/api/stream?hz=%32", "/api/stream?interval=%2e1",
             "/api/stream?%68z=2", "/api/%73tream?hz=2"}) {
        requireRejected(decode(target), Error::PercentEncodingNotAllowed);
    }

    for (const char control : {'\0', '\x01', '\x1f', '\x7f'}) {
        std::string target = "/api/stream?hz=1";
        target.push_back(control);
        requireRejected(decode(target), Error::ControlCharacterNotAllowed);
    }

    for (const auto target : {
             "/api/stream?hz=1&interval=2",
             "/api/stream?interval=2&hz=1",
             "/api/stream?hz=1&",
             "/api/stream?hz==1",
             "/api/stream?interval==2",
             "/api/stream?hz=1?interval=2"}) {
        requireRejected(decode(target), Error::ExtraQueryComponent);
    }

    for (const auto target : {
             "/api/stream?", "/api/stream?hz", "/api/stream?interval",
             "/api/stream?=2"}) {
        requireRejected(decode(target), Error::MalformedQuery);
    }

    for (const auto target : {
             "/api/stream?HZ=2", "/api/stream?hZ=2",
             "/api/stream?rate=2", "/api/stream?intervals=2"}) {
        requireRejected(decode(target), Error::UnsupportedQuery);
    }
}

void rejectsWrongTargetsAndRouteClassifications()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    for (const auto target : {
             "", "api/stream", "/api/live", "/api/stream/",
             "/api/streaming", "/api/stream#fragment",
             "/api/stream\\child"}) {
        requireRejected(
            Dashboard::DashboardStreamQueryDecoder::decode(
                Dashboard::DashboardRouteQuery::None,
                target,
                Domain::budgetsForProfile(
                    Domain::ResourceProfile::Standard16GiB)),
            Error::MalformedTarget);
    }

    const auto budgets = Domain::budgetsForProfile(
        Domain::ResourceProfile::Standard16GiB);
    struct MismatchCase final {
        Dashboard::DashboardRouteQuery classification;
        std::string_view target;
    };
    constexpr std::array mismatches{
        MismatchCase{
            Dashboard::DashboardRouteQuery::None,
            "/api/stream?hz=1"},
        MismatchCase{
            Dashboard::DashboardRouteQuery::StreamHertz,
            "/api/stream"},
        MismatchCase{
            Dashboard::DashboardRouteQuery::StreamHertz,
            "/api/stream?interval=1"},
        MismatchCase{
            Dashboard::DashboardRouteQuery::LegacyStreamInterval,
            "/api/stream?hz=1"},
        MismatchCase{
            Dashboard::DashboardRouteQuery::UnsupportedStreamQuery,
            "/api/stream?hz=1"},
    };
    for (const auto& entry : mismatches) {
        requireRejected(
            Dashboard::DashboardStreamQueryDecoder::decode(
                entry.classification, entry.target, budgets),
            Error::QueryClassificationMismatch);
    }
    requireRejected(
        Dashboard::DashboardStreamQueryDecoder::decode(
            static_cast<Dashboard::DashboardRouteQuery>(255U),
            "/api/stream",
            budgets),
        Error::QueryClassificationMismatch);
}

void rejectsNonProfileResourceCaps()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    const auto valid = Domain::budgetsForProfile(
        Domain::ResourceProfile::Standard16GiB);
    for (const auto invalid : {
             -1.0,
             0.0,
             0.999,
             1.5,
             2.001,
             3.0,
             (std::numeric_limits<double>::infinity)(),
             (std::numeric_limits<double>::quiet_NaN)()}) {
        auto budgets = valid;
        budgets.telemetrySampleHz = invalid;
        requireRejected(decode("/api/stream?hz=1", budgets),
                        Error::InvalidResourceBudget);
    }
}

void honorsThe8192ByteTargetBoundWithoutCopyingInput()
{
    using Error = Dashboard::DashboardStreamQueryDecodeError;
    constexpr std::string_view Prefix{"/api/stream?hz="};
    std::string atLimit{Prefix};
    atLimit.push_back('1');
    atLimit.append(
        Dashboard::DashboardStreamQueryDecoder::MaximumTargetBytes -
            atLimit.size(),
        '0');
    REQUIRE(atLimit.size() ==
            Dashboard::DashboardStreamQueryDecoder::MaximumTargetBytes);
    requireRejected(decode(atLimit), Error::NumericOverflow);

    atLimit.push_back('0');
    REQUIRE(atLimit.size() ==
            Dashboard::DashboardStreamQueryDecoder::MaximumTargetBytes + 1U);
    requireRejected(decode(atLimit), Error::TargetTooLong);
}

} // namespace

int main()
{
    try {
        appliesEverySelectedWindowsProfileToTheDefault();
        decodesTheExactHertzGrammarAndClampsCompatibly();
        preservesTheLegacyIntervalUpgradeBeforeTheWindowsCap();
        rejectsMissingMalformedAndNonFiniteNumericValues();
        distinguishesNumericUnderflowFromOverflow();
        rejectsEncodedControlledExtraAndUnsupportedQueries();
        rejectsWrongTargetsAndRouteClassifications();
        rejectsNonProfileResourceCaps();
        honorsThe8192ByteTargetBoundWithoutCopyingInput();
        std::cout << "Dashboard stream query decoder tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard stream query decoder tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard stream query decoder tests failed with an unknown error.\n";
        return 1;
    }
}
