#pragma once

#include <Windows.h>

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

template <typename T>
class UniqueLocalAllocation final {
public:
    UniqueLocalAllocation() noexcept = default;
    explicit UniqueLocalAllocation(T* allocation) noexcept : allocation_{allocation} {}

    ~UniqueLocalAllocation() noexcept { reset(); }

    UniqueLocalAllocation(const UniqueLocalAllocation&) = delete;
    UniqueLocalAllocation& operator=(const UniqueLocalAllocation&) = delete;

    UniqueLocalAllocation(UniqueLocalAllocation&& other) noexcept
        : allocation_{std::exchange(other.allocation_, nullptr)}
    {
    }

    UniqueLocalAllocation& operator=(UniqueLocalAllocation&& other) noexcept
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
            static_cast<void>(::LocalFree(reinterpret_cast<HLOCAL>(previous)));
        }
    }

private:
    T* allocation_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
