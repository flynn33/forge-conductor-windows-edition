#pragma once

#include <oleauto.h>

#include <climits>
#include <cstddef>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class UniqueBstr final {
public:
    UniqueBstr() noexcept = default;
    explicit UniqueBstr(BSTR value) noexcept : value_{value} {}

    ~UniqueBstr() noexcept { reset(); }

    UniqueBstr(const UniqueBstr&) = delete;
    UniqueBstr& operator=(const UniqueBstr&) = delete;

    UniqueBstr(UniqueBstr&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)}
    {
    }

    UniqueBstr& operator=(UniqueBstr&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] static UniqueBstr copy(const std::wstring_view value) noexcept
    {
        if (value.size() > static_cast<std::size_t>(UINT_MAX)) {
            return {};
        }
        return UniqueBstr{::SysAllocStringLen(
            value.data(), static_cast<UINT>(value.size()))};
    }

    [[nodiscard]] BSTR get() const noexcept { return value_; }
    [[nodiscard]] BSTR* put() noexcept
    {
        reset();
        return &value_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

    [[nodiscard]] std::wstring_view view() const noexcept
    {
        return value_ == nullptr
            ? std::wstring_view{}
            : std::wstring_view{value_, ::SysStringLen(value_)};
    }

    [[nodiscard]] BSTR detach() noexcept
    {
        return std::exchange(value_, nullptr);
    }

    void reset(BSTR replacement = nullptr) noexcept
    {
        BSTR previous = std::exchange(value_, replacement);
        if (previous != nullptr) {
            ::SysFreeString(previous);
        }
    }

private:
    BSTR value_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
