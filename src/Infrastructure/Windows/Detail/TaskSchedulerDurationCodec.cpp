#include "TaskSchedulerDurationCodec.h"

#include "UtfConversion.h"

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

struct LexicalDuration final {
    std::optional<std::string_view> days;
    std::optional<std::string_view> hours;
    std::optional<std::string_view> minutes;
    std::optional<std::string_view> seconds;
    bool requiresPreservation{};
};

[[nodiscard]] bool containsNonzeroDigit(
    const std::string_view digits) noexcept
{
    for (const char digit : digits) {
        if (digit != '0') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Domain::Error malformedDuration()
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "The Task Scheduler duration is not a valid nonnegative XSD duration.");
}

[[nodiscard]] Domain::Error negativeDuration()
{
    return Domain::makeError(
        Domain::ErrorCodes::InvalidRequest,
        "Task Scheduler durations cannot be negative.");
}

[[nodiscard]] Domain::Error oversizedDuration()
{
    return Domain::makeError(
        Domain::ErrorCodes::LimitExceeded,
        "The Task Scheduler duration exceeds its bounded representation.");
}

[[nodiscard]] Domain::Result<LexicalDuration> parseLexicalDuration(
    const std::string_view text)
{
    if (text.starts_with('-')) {
        return Domain::Result<LexicalDuration>::failure(negativeDuration());
    }
    if (text.size() < 2U || text.front() != 'P') {
        return Domain::Result<LexicalDuration>::failure(malformedDuration());
    }

    LexicalDuration parsed;
    std::size_t index = 1U;
    bool dateComponentSeen = false;
    unsigned int priorDateStage = 0U;
    while (index < text.size() && text[index] != 'T') {
        const std::size_t digitsBegin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
        if (digitsBegin == index || index >= text.size()) {
            return Domain::Result<LexicalDuration>::failure(malformedDuration());
        }

        const std::string_view digits = text.substr(digitsBegin, index - digitsBegin);
        const char designator = text[index++];
        unsigned int stage{};
        switch (designator) {
        case 'Y':
            stage = 1U;
            parsed.requiresPreservation =
                parsed.requiresPreservation || containsNonzeroDigit(digits);
            break;
        case 'M':
            stage = 2U;
            parsed.requiresPreservation =
                parsed.requiresPreservation || containsNonzeroDigit(digits);
            break;
        case 'D':
            stage = 3U;
            parsed.days = digits;
            break;
        default:
            return Domain::Result<LexicalDuration>::failure(malformedDuration());
        }
        if (stage <= priorDateStage) {
            return Domain::Result<LexicalDuration>::failure(malformedDuration());
        }
        priorDateStage = stage;
        dateComponentSeen = true;
    }

    bool timeComponentSeen = false;
    if (index < text.size()) {
        if (text[index] != 'T') {
            return Domain::Result<LexicalDuration>::failure(malformedDuration());
        }
        ++index;
        unsigned int priorTimeStage = 0U;
        while (index < text.size()) {
            const std::size_t digitsBegin = index;
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                ++index;
            }
            if (digitsBegin == index || index >= text.size()) {
                return Domain::Result<LexicalDuration>::failure(malformedDuration());
            }

            const std::string_view digits = text.substr(digitsBegin, index - digitsBegin);
            if (text[index] == '.') {
                if (priorTimeStage >= 3U) {
                    return Domain::Result<LexicalDuration>::failure(malformedDuration());
                }
                ++index;
                const std::size_t fractionBegin = index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                    ++index;
                }
                if (fractionBegin == index || index >= text.size() || text[index] != 'S') {
                    return Domain::Result<LexicalDuration>::failure(malformedDuration());
                }
                const std::string_view fraction =
                    text.substr(fractionBegin, index - fractionBegin);
                ++index;
                priorTimeStage = 3U;
                parsed.seconds = digits;
                parsed.requiresPreservation =
                    parsed.requiresPreservation ||
                    containsNonzeroDigit(fraction);
                timeComponentSeen = true;
                continue;
            }

            const char designator = text[index++];
            unsigned int stage{};
            switch (designator) {
            case 'H':
                stage = 1U;
                parsed.hours = digits;
                break;
            case 'M':
                stage = 2U;
                parsed.minutes = digits;
                break;
            case 'S':
                stage = 3U;
                parsed.seconds = digits;
                break;
            default:
                return Domain::Result<LexicalDuration>::failure(malformedDuration());
            }
            if (stage <= priorTimeStage) {
                return Domain::Result<LexicalDuration>::failure(malformedDuration());
            }
            priorTimeStage = stage;
            timeComponentSeen = true;
        }
        if (!timeComponentSeen) {
            return Domain::Result<LexicalDuration>::failure(malformedDuration());
        }
    }

    if (!dateComponentSeen && !timeComponentSeen) {
        return Domain::Result<LexicalDuration>::failure(malformedDuration());
    }
    return Domain::Result<LexicalDuration>::success(parsed);
}

using SecondsRep = std::chrono::seconds::rep;
using UnsignedSecondsRep = std::make_unsigned_t<SecondsRep>;

static_assert(std::numeric_limits<SecondsRep>::is_integer);
static_assert((std::chrono::seconds::max)().count() > 0);

constexpr UnsignedSecondsRep MaximumSeconds =
    static_cast<UnsignedSecondsRep>((std::chrono::seconds::max)().count());

[[nodiscard]] bool addFixedComponent(
    const std::optional<std::string_view>& digits,
    const UnsignedSecondsRep multiplier,
    UnsignedSecondsRep& total) noexcept
{
    if (!digits.has_value()) {
        return true;
    }

    UnsignedSecondsRep component{};
    for (const char digitCharacter : *digits) {
        const auto digit = static_cast<UnsignedSecondsRep>(digitCharacter - '0');
        if (component > (MaximumSeconds - digit) / 10U) {
            return false;
        }
        component = component * 10U + digit;
    }
    if (component > (MaximumSeconds - total) / multiplier) {
        return false;
    }
    total += component * multiplier;
    return true;
}

[[nodiscard]] Domain::Result<std::chrono::seconds> normalizedSeconds(
    const LexicalDuration& value)
{
    UnsignedSecondsRep total{};
    if (!addFixedComponent(value.days, 86'400U, total) ||
        !addFixedComponent(value.hours, 3'600U, total) ||
        !addFixedComponent(value.minutes, 60U, total) ||
        !addFixedComponent(value.seconds, 1U, total)) {
        return Domain::Result<std::chrono::seconds>::failure(oversizedDuration());
    }
    return Domain::Result<std::chrono::seconds>::success(
        std::chrono::seconds{static_cast<SecondsRep>(total)});
}

} // namespace

Domain::Result<Manager::ManagerStartupTaskDuration>
TaskSchedulerDurationCodec::parse(const std::wstring_view value) noexcept
{
    try {
        if (value.size() > MaximumTextUtf16Units) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
                oversizedDuration());
        }
        auto utf8 = strictUtf16ToUtf8(value);
        if (!utf8) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
                std::move(utf8).error());
        }
        if (utf8.value().size() > Manager::ManagerStartupTaskPolicy::MaximumTextBytes) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
                oversizedDuration());
        }

        auto parsed = parseLexicalDuration(utf8.value());
        if (!parsed) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
                std::move(parsed).error());
        }
        if (parsed.value().requiresPreservation) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::success(
                Manager::ManagerStartupTaskDuration::preserved(
                    std::move(utf8).value()));
        }

        auto seconds = normalizedSeconds(parsed.value());
        if (!seconds) {
            return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
                std::move(seconds).error());
        }
        return Domain::Result<Manager::ManagerStartupTaskDuration>::success(
            Manager::ManagerStartupTaskDuration{seconds.value()});
    } catch (...) {
        return Domain::Result<Manager::ManagerStartupTaskDuration>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Task Scheduler duration could not allocate bounded state."));
    }
}

Domain::Result<std::wstring> TaskSchedulerDurationCodec::format(
    const Manager::ManagerStartupTaskDuration& value) noexcept
{
    try {
        if (const auto* fixed = value.fixedSeconds(); fixed != nullptr) {
            if (*fixed < std::chrono::seconds::zero()) {
                return Domain::Result<std::wstring>::failure(negativeDuration());
            }
            return Domain::Result<std::wstring>::success(
                L"PT" + std::to_wstring(fixed->count()) + L"S");
        }

        const auto* preserved = value.preservedText();
        if (preserved == nullptr) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The Task Scheduler duration has no representable value."));
        }
        if (preserved->size() > Manager::ManagerStartupTaskPolicy::MaximumTextBytes) {
            return Domain::Result<std::wstring>::failure(oversizedDuration());
        }
        auto native = strictUtf8ToUtf16(*preserved);
        if (!native) {
            return native;
        }
        if (native.value().size() > MaximumTextUtf16Units) {
            return Domain::Result<std::wstring>::failure(oversizedDuration());
        }
        auto parsed = parseLexicalDuration(*preserved);
        if (!parsed) {
            return Domain::Result<std::wstring>::failure(std::move(parsed).error());
        }
        if (!parsed.value().requiresPreservation) {
            auto bounded = normalizedSeconds(parsed.value());
            if (!bounded) {
                return Domain::Result<std::wstring>::failure(
                    std::move(bounded).error());
            }
        }
        return native;
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Task Scheduler duration could not be formatted."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
