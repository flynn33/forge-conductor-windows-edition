#pragma once

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class SecureBuffer final {
public:
    explicit SecureBuffer(const std::size_t size)
        : data_{size == 0 ? nullptr : std::make_unique<std::byte[]>(size)},
          size_{size}
    {
    }

    ~SecureBuffer() noexcept { clear(); }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept
        : data_{std::move(other.data_)}, size_{std::exchange(other.size_, 0)}
    {
    }

    SecureBuffer& operator=(SecureBuffer&& other) noexcept
    {
        if (this != &other) {
            clear();
            data_ = std::move(other.data_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    [[nodiscard]] std::byte* data() noexcept { return data_.get(); }
    [[nodiscard]] const std::byte* data() const noexcept { return data_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::span<std::byte> bytes() noexcept
    {
        return {data_.get(), size_};
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return {data_.get(), size_};
    }

private:
    void clear() noexcept
    {
        if (data_ != nullptr && size_ != 0) {
            static_cast<void>(::SecureZeroMemory(data_.get(), size_));
        }
        data_.reset();
        size_ = 0;
    }

    std::unique_ptr<std::byte[]> data_;
    std::size_t size_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
