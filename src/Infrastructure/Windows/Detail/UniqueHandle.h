#pragma once

#include <Windows.h>

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_{handle} {}

    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(handle_); }

    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ == handle) {
            return;
        }
        const HANDLE previous = std::exchange(handle_, handle);
        if (valid(previous)) {
            static_cast<void>(::CloseHandle(previous));
        }
    }

private:
    [[nodiscard]] static bool valid(HANDLE handle) noexcept
    {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    HANDLE handle_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
