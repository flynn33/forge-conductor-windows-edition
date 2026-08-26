#pragma once

#include <objbase.h>

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

template <typename T>
class UniqueCoTaskMemAllocation final {
public:
    UniqueCoTaskMemAllocation() noexcept = default;
    explicit UniqueCoTaskMemAllocation(T* allocation) noexcept : allocation_{allocation} {}

    ~UniqueCoTaskMemAllocation() noexcept { reset(); }

    UniqueCoTaskMemAllocation(const UniqueCoTaskMemAllocation&) = delete;
    UniqueCoTaskMemAllocation& operator=(const UniqueCoTaskMemAllocation&) = delete;

    UniqueCoTaskMemAllocation(UniqueCoTaskMemAllocation&& other) noexcept
        : allocation_{std::exchange(other.allocation_, nullptr)}
    {
    }

    UniqueCoTaskMemAllocation& operator=(UniqueCoTaskMemAllocation&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.allocation_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return allocation_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return allocation_ != nullptr;
    }

    [[nodiscard]] T* release() noexcept
    {
        return std::exchange(allocation_, nullptr);
    }

    void reset(T* allocation = nullptr) noexcept
    {
        if (allocation_ == allocation) {
            return;
        }
        T* const previous = std::exchange(allocation_, allocation);
        if (previous != nullptr) {
            ::CoTaskMemFree(previous);
        }
    }

private:
    T* allocation_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
