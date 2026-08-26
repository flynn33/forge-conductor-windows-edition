#pragma once

#include <bcrypt.h>

#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class UniqueBCryptAlgorithmHandle final {
public:
    UniqueBCryptAlgorithmHandle() noexcept = default;
    explicit UniqueBCryptAlgorithmHandle(BCRYPT_ALG_HANDLE handle) noexcept
        : handle_{handle}
    {
    }

    ~UniqueBCryptAlgorithmHandle() noexcept { reset(); }

    UniqueBCryptAlgorithmHandle(const UniqueBCryptAlgorithmHandle&) = delete;
    UniqueBCryptAlgorithmHandle& operator=(const UniqueBCryptAlgorithmHandle&) = delete;

    UniqueBCryptAlgorithmHandle(UniqueBCryptAlgorithmHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    UniqueBCryptAlgorithmHandle& operator=(
        UniqueBCryptAlgorithmHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] BCRYPT_ALG_HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(BCRYPT_ALG_HANDLE handle = nullptr) noexcept
    {
        if (handle_ == handle) {
            return;
        }
        BCRYPT_ALG_HANDLE const previous = std::exchange(handle_, handle);
        if (previous != nullptr) {
            static_cast<void>(::BCryptCloseAlgorithmProvider(previous, 0));
        }
    }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class UniqueBCryptHashHandle final {
public:
    UniqueBCryptHashHandle() noexcept = default;
    explicit UniqueBCryptHashHandle(BCRYPT_HASH_HANDLE handle) noexcept
        : handle_{handle}
    {
    }

    ~UniqueBCryptHashHandle() noexcept { reset(); }

    UniqueBCryptHashHandle(const UniqueBCryptHashHandle&) = delete;
    UniqueBCryptHashHandle& operator=(const UniqueBCryptHashHandle&) = delete;

    UniqueBCryptHashHandle(UniqueBCryptHashHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    UniqueBCryptHashHandle& operator=(UniqueBCryptHashHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] BCRYPT_HASH_HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(BCRYPT_HASH_HANDLE handle = nullptr) noexcept
    {
        if (handle_ == handle) {
            return;
        }
        BCRYPT_HASH_HANDLE const previous = std::exchange(handle_, handle);
        if (previous != nullptr) {
            static_cast<void>(::BCryptDestroyHash(previous));
        }
    }

private:
    BCRYPT_HASH_HANDLE handle_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
