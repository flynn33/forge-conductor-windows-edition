#pragma once

#include "ForgeConductor/Domain/Error.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ForgeConductor::Domain {

template <typename T>
class [[nodiscard]] Result final {
    static_assert(!std::is_void_v<T>, "Result<void> uses the explicit specialization.");
    static_assert(!std::is_reference_v<T>, "Result cannot own a reference payload.");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>,
                  "Result<Error> would make success and failure ambiguous.");

public:
    [[nodiscard]] static Result success(T value)
    {
        return Result{std::in_place_index<0>, std::move(value)};
    }

    [[nodiscard]] static Result failure(Error error)
    {
        return Result{std::in_place_index<1>, std::move(error)};
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return valueOrError_.index() == 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] T& value() & { return std::get<0>(valueOrError_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(valueOrError_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(valueOrError_)); }

    [[nodiscard]] Error& error() & { return std::get<1>(valueOrError_); }
    [[nodiscard]] const Error& error() const& { return std::get<1>(valueOrError_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(valueOrError_)); }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value&& value)
        : valueOrError_{index, std::forward<Value>(value)}
    {
    }

    std::variant<T, Error> valueOrError_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    [[nodiscard]] static Result success() noexcept { return Result{}; }

    [[nodiscard]] static Result failure(Error error)
    {
        return Result{std::move(error)};
    }

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    void value() const
    {
        if (error_.has_value()) {
            throw std::bad_variant_access{};
        }
    }

    [[nodiscard]] Error& error() & { return error_.value(); }
    [[nodiscard]] const Error& error() const& { return error_.value(); }
    [[nodiscard]] Error&& error() && { return std::move(error_.value()); }

private:
    Result() = default;
    explicit Result(Error error) : error_{std::move(error)} {}

    std::optional<Error> error_;
};

} // namespace ForgeConductor::Domain
