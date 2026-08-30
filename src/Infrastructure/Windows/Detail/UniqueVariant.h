#pragma once

#include <oleauto.h>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class UniqueVariant final {
public:
    UniqueVariant() noexcept { ::VariantInit(&value_); }
    ~UniqueVariant() noexcept { static_cast<void>(::VariantClear(&value_)); }

    UniqueVariant(const UniqueVariant&) = delete;
    UniqueVariant& operator=(const UniqueVariant&) = delete;
    UniqueVariant(UniqueVariant&&) = delete;
    UniqueVariant& operator=(UniqueVariant&&) = delete;

    [[nodiscard]] VARIANT* put() noexcept
    {
        if (::VariantClear(&value_) != S_OK) {
            return nullptr;
        }
        ::VariantInit(&value_);
        return &value_;
    }

    [[nodiscard]] VARIANT& get() noexcept { return value_; }
    [[nodiscard]] const VARIANT& get() const noexcept { return value_; }

private:
    VARIANT value_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
