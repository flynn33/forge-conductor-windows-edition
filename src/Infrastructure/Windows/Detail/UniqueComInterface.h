#pragma once

#include <unknwn.h>

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

template <typename Interface>
class UniqueComInterface final {
public:
    UniqueComInterface() noexcept = default;
    explicit UniqueComInterface(Interface* value) noexcept : value_{value} {}

    ~UniqueComInterface() noexcept { reset(); }

    UniqueComInterface(const UniqueComInterface&) = delete;
    UniqueComInterface& operator=(const UniqueComInterface&) = delete;

    UniqueComInterface(UniqueComInterface&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)}
    {
    }

    UniqueComInterface& operator=(UniqueComInterface&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] Interface* get() const noexcept { return value_; }
    [[nodiscard]] Interface* operator->() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

    [[nodiscard]] Interface** put() noexcept
    {
        reset();
        return &value_;
    }

    [[nodiscard]] Interface* detach() noexcept
    {
        return std::exchange(value_, nullptr);
    }

    void reset(Interface* replacement = nullptr) noexcept
    {
        Interface* previous = std::exchange(value_, replacement);
        if (previous != nullptr) {
            previous->Release();
        }
    }

private:
    Interface* value_{};
};

template <typename To, typename From>
[[nodiscard]] HRESULT queryComInterface(
    From* source,
    UniqueComInterface<To>& destination) noexcept
{
    if (source == nullptr) {
        return E_POINTER;
    }
    UniqueComInterface<To> queried;
    const HRESULT result = source->QueryInterface(
        __uuidof(To),
        reinterpret_cast<void**>(queried.put()));
    if (result == S_OK) {
        destination = std::move(queried);
    }
    return result;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
