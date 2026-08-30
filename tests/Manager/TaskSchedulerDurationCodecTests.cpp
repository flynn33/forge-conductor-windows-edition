#include "Infrastructure/TestSupport.h"

#include "Infrastructure/Windows/Detail/TaskSchedulerDurationCodec.h"

#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::Detail::TaskSchedulerDurationCodec;
using Manager::ManagerStartupTaskDuration;
using namespace std::chrono_literals;

static_assert(std::is_final_v<TaskSchedulerDurationCodec>);
static_assert(!std::is_constructible_v<TaskSchedulerDurationCodec>);

void requireFixed(
    const std::wstring_view source,
    const std::chrono::seconds expected,
    const std::string_view message)
{
    const auto parsed = take(TaskSchedulerDurationCodec::parse(source));
    require(parsed.fixedSeconds() != nullptr, message);
    require(*parsed.fixedSeconds() == expected, message);
    require(parsed.preservedText() == nullptr, message);
}

void requirePreserved(
    const std::wstring_view source,
    const std::string_view expected,
    const std::string_view message)
{
    const auto parsed = take(TaskSchedulerDurationCodec::parse(source));
    require(parsed.fixedSeconds() == nullptr, message);
    require(parsed.preservedText() != nullptr, message);
    require(*parsed.preservedText() == expected, message);
    require(take(TaskSchedulerDurationCodec::format(parsed)) == source, message);
}

void zeroAndEquivalentFormsNormalize()
{
    for (const std::wstring_view source : {
             L"PT0S", L"PT00S", L"P0D", L"P0DT0H0M0S", L"PT0H0M",
             L"P0Y", L"P0M", L"P0Y0M", L"PT0.000S"}) {
        requireFixed(source, 0s, "An equivalent zero duration was not normalized.");
    }
    require(
        take(TaskSchedulerDurationCodec::format(ManagerStartupTaskDuration{0s})) ==
            L"PT0S",
        "Zero seconds did not use the deterministic Task Scheduler representation.");
}

void dayHourMinuteSecondFormsNormalize()
{
    requireFixed(L"P1D", 24h, "A whole day was not normalized to seconds.");
    requireFixed(L"PT24H", 24h, "Equivalent hours were not normalized to seconds.");
    requireFixed(L"PT1440M", 24h, "Equivalent minutes were not normalized to seconds.");
    requireFixed(L"PT86400S", 24h, "Equivalent seconds were not normalized.");
    requireFixed(
        L"P0Y0M1D",
        24h,
        "zero calendar components caused false duration drift");
    requireFixed(
        L"PT1.000S",
        1s,
        "zero fractional precision caused false duration drift");
    requireFixed(
        L"P2DT25H61M61S",
        2 * 24h + 25h + 61min + 61s,
        "A combined day/hour/minute/second duration was not normalized.");

    constexpr auto expected = 1 * 24h + 2h + 3min + 4s;
    requireFixed(
        L"P1DT2H3M4S",
        expected,
        "An ordered day/hour/minute/second duration was not normalized.");
    requireFixed(
        L"P3DT4H5M6.000S",
        3 * 24h + 4h + 5min + 6s,
        "A zero fractional component caused false duration drift.");
    require(
        take(TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration{expected})) == L"PT93784S",
        "A fixed duration did not use deterministic whole seconds.");
}

void calendarAndFractionalFormsArePreserved()
{
    requirePreserved(L"P1Y", "P1Y", "A year duration was not preserved.");
    requirePreserved(L"P1M", "P1M", "A calendar-month duration was not preserved.");
    requirePreserved(
        L"P1Y2M3DT4H5M6S",
        "P1Y2M3DT4H5M6S",
        "A mixed calendar duration was not preserved exactly.");
    requirePreserved(
        L"PT0.500S",
        "PT0.500S",
        "A fractional-second duration was not preserved exactly.");
    requirePreserved(
        L"PT1.001S",
        "PT1.001S",
        "A nonzero fractional second was not preserved exactly.");
    require(
        take(TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved("PT60S"))) == L"PT60S",
        "A valid explicitly preserved lexical form was not passed through.");
}

void malformedFormsAreRejected()
{
    constexpr std::array malformed{
        std::wstring_view{L""},
        std::wstring_view{L"P"},
        std::wstring_view{L"PT"},
        std::wstring_view{L"1D"},
        std::wstring_view{L"+PT1S"},
        std::wstring_view{L"P1H"},
        std::wstring_view{L"P1S"},
        std::wstring_view{L"PT1D"},
        std::wstring_view{L"P1DT"},
        std::wstring_view{L"P1D2Y"},
        std::wstring_view{L"P1Y2Y"},
        std::wstring_view{L"PT1M2H"},
        std::wstring_view{L"PT1.S"},
        std::wstring_view{L"PT.5S"},
        std::wstring_view{L"PT1.2M"},
        std::wstring_view{L"P1DT1Hjunk"}};
    for (const auto source : malformed) {
        requireError(
            TaskSchedulerDurationCodec::parse(source),
            Domain::ErrorCodes::InvalidRequest,
            "A malformed XSD duration was accepted.");
    }

    std::wstring embeddedNull{L"PT1S"};
    embeddedNull.insert(2U, 1U, L'\0');
    requireError(
        TaskSchedulerDurationCodec::parse(embeddedNull),
        Domain::ErrorCodes::InvalidRequest,
        "An XSD duration containing an embedded NUL was accepted.");
    requireError(
        TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved("P")),
        Domain::ErrorCodes::InvalidRequest,
        "Formatting accepted a malformed preserved duration.");
}

void negativeFormsAreRejected()
{
    for (const std::wstring_view source : {L"-PT0S", L"-PT1S", L"-P1Y"}) {
        requireError(
            TaskSchedulerDurationCodec::parse(source),
            Domain::ErrorCodes::InvalidRequest,
            "A negative Task Scheduler duration was accepted.");
    }
    requireError(
        TaskSchedulerDurationCodec::format(ManagerStartupTaskDuration{-1s}),
        Domain::ErrorCodes::InvalidRequest,
        "Formatting accepted negative fixed seconds.");
    requireError(
        TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved("-PT1S")),
        Domain::ErrorCodes::InvalidRequest,
        "Formatting accepted a negative preserved duration.");
}

void invalidUtf16IsRejected()
{
    const std::wstring unpairedHighSurrogate{
        static_cast<wchar_t>(0xd800)};
    requireError(
        TaskSchedulerDurationCodec::parse(unpairedHighSurrogate),
        Domain::ErrorCodes::InvalidRequest,
        "An unpaired UTF-16 surrogate reached the duration parser.");

    std::string invalidUtf8{"PT"};
    invalidUtf8.push_back(static_cast<char>(0xff));
    invalidUtf8.push_back('S');
    requireError(
        TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved(std::move(invalidUtf8))),
        Domain::ErrorCodes::InvalidRequest,
        "Invalid UTF-8 was emitted as a Task Scheduler duration.");
}

void overflowAndTextBoundsAreRejected()
{
    const auto maximumSeconds = (std::chrono::seconds::max)().count();
    const std::wstring maximumText = L"PT" + std::to_wstring(maximumSeconds) + L"S";
    requireFixed(
        maximumText,
        std::chrono::seconds::max(),
        "The maximum fixed duration was rejected.");
    require(
        take(TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration{std::chrono::seconds::max()})) == maximumText,
        "The maximum fixed duration did not round trip.");

    const std::wstring decimalOverflow =
        L"PT" + std::to_wstring(maximumSeconds) + L"0S";
    requireError(
        TaskSchedulerDurationCodec::parse(decimalOverflow),
        Domain::ErrorCodes::LimitExceeded,
        "A decimal duration overflow was accepted.");
    requireError(
        TaskSchedulerDurationCodec::parse(
            L"P" + std::to_wstring(maximumSeconds) + L"D"),
        Domain::ErrorCodes::LimitExceeded,
        "A scaled day duration overflow was accepted.");
    requireError(
        TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved(
                "PT" + std::to_string(maximumSeconds) + "0S")),
        Domain::ErrorCodes::LimitExceeded,
        "Formatting accepted an overflowing preserved duration.");

    std::wstring oversized(
        TaskSchedulerDurationCodec::MaximumTextUtf16Units + 1U,
        L'0');
    oversized.front() = L'P';
    oversized.back() = L'D';
    requireError(
        TaskSchedulerDurationCodec::parse(oversized),
        Domain::ErrorCodes::LimitExceeded,
        "An over-limit UTF-16 duration reached lexical parsing.");

    std::wstring exactBound(
        TaskSchedulerDurationCodec::MaximumTextUtf16Units,
        L'0');
    exactBound.front() = L'P';
    exactBound.back() = L'D';
    requireFixed(
        exactBound,
        0s,
        "A valid duration at the exact text bound was rejected.");
    std::string exactBoundUtf8(
        TaskSchedulerDurationCodec::MaximumTextUtf16Units,
        '0');
    exactBoundUtf8.front() = 'P';
    exactBoundUtf8.back() = 'D';
    require(
        take(TaskSchedulerDurationCodec::format(
            ManagerStartupTaskDuration::preserved(exactBoundUtf8))) ==
            exactBound,
        "A preserved duration at the exact text bound was rejected.");
}

} // namespace

void registerTaskSchedulerDurationCodecTests(TestRegistry& tests)
{
    addTest(tests, "manager_startup.duration.zero-equivalence",
            zeroAndEquivalentFormsNormalize);
    addTest(tests, "manager_startup.duration.fixed-components",
            dayHourMinuteSecondFormsNormalize);
    addTest(tests, "manager_startup.duration.preserved-forms",
            calendarAndFractionalFormsArePreserved);
    addTest(tests, "manager_startup.duration.malformed",
            malformedFormsAreRejected);
    addTest(tests, "manager_startup.duration.negative",
            negativeFormsAreRejected);
    addTest(tests, "manager_startup.duration.invalid-utf16",
            invalidUtf16IsRejected);
    addTest(tests, "manager_startup.duration.overflow",
            overflowAndTextBoundsAreRejected);
}

} // namespace ForgeConductor::Tests
