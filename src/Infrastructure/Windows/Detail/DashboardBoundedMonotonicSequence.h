#pragma once

#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Small allocation-free sequence primitive shared by dashboard registration,
// completion-key, and deadline-arm ownership. Copies are independent staging
// values: a caller that needs two identities atomically can advance copies and
// commit them together only after every candidate succeeds.
template <std::unsigned_integral Value>
class DashboardBoundedMonotonicSequence final {
public:
    explicit constexpr DashboardBoundedMonotonicSequence(
        const Value firstValue = Value{1}) noexcept
        : nextValue_{firstValue}
    {
    }

    template <typename ReservedPredicate>
    [[nodiscard]] std::optional<Value> tryTake(
        ReservedPredicate&& reserved) noexcept
    {
        static_assert(std::is_nothrow_invocable_r_v<
                      bool, ReservedPredicate&, Value>);
        if (exhausted_) {
            return std::nullopt;
        }

        auto candidate = nextValue_;
        for (;;) {
            if (!reserved(candidate)) {
                break;
            }
            if (candidate == (std::numeric_limits<Value>::max)()) {
                exhausted_ = true;
                return std::nullopt;
            }
            ++candidate;
        }

        if (candidate == (std::numeric_limits<Value>::max)()) {
            exhausted_ = true;
        } else {
            nextValue_ = static_cast<Value>(candidate + Value{1});
        }
        return candidate;
    }

    [[nodiscard]] constexpr bool exhausted() const noexcept
    {
        return exhausted_;
    }

private:
    Value nextValue_{};
    bool exhausted_{};
};

static_assert(std::is_trivially_copyable_v<
              DashboardBoundedMonotonicSequence<unsigned int>>);

} // namespace ForgeConductor::Infrastructure::Windows::Detail
