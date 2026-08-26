#pragma once

#include <optional>
#include <string>
#include <utility>

namespace forge
{
    struct Error final
    {
        std::string code;
        std::string message;
        bool retryable{};
        std::optional<std::string> evidenceId;
    };

    template <typename T>
    class Result final
    {
    public:
        static Result success(T value) { return Result{std::move(value), std::nullopt}; }
        static Result failure(Error error) { return Result{std::nullopt, std::move(error)}; }

        [[nodiscard]] bool hasValue() const noexcept { return m_value.has_value(); }
        [[nodiscard]] T const& value() const& { return m_value.value(); }
        [[nodiscard]] T&& value() && { return std::move(m_value.value()); }
        [[nodiscard]] Error const& error() const { return m_error.value(); }

    private:
        Result(std::optional<T> value, std::optional<Error> error)
            : m_value{std::move(value)}, m_error{std::move(error)} {}

        std::optional<T> m_value;
        std::optional<Error> m_error;
    };
}
