#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace {

constexpr std::wstring_view HomeArgument = L"--home";
constexpr std::wstring_view SentinelFileName =
    L"manager-startup-lifecycle.sentinel";
constexpr std::string_view SentinelPayload =
    "forge-conductor-manager-startup-lifecycle-v1\r\n";
constexpr std::size_t MaximumPathCharacters = 32'767U;

class UniqueHandle final {
public:
    explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_{value}
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    void reset() noexcept
    {
        if (valid()) {
            static_cast<void>(::CloseHandle(value_));
        }
        value_ = INVALID_HANDLE_VALUE;
    }

    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] bool isBoundedAbsoluteLocalPath(
    const std::wstring_view value) noexcept
{
    const bool hasDrive = value.size() >= 3U &&
        ((value[0U] >= L'A' && value[0U] <= L'Z') ||
         (value[0U] >= L'a' && value[0U] <= L'z')) &&
        value[1U] == L':' && value[2U] == L'\\';
    return hasDrive && value.size() <= MaximumPathCharacters &&
        value.back() != L'\\' && value.find(L'/') == std::wstring_view::npos;
}

[[nodiscard]] bool writeAll(
    const HANDLE destination,
    const std::string_view bytes) noexcept
{
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto requested = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written{};
        if (!::WriteFile(
                destination,
                bytes.data() + offset,
                requested,
                &written,
                nullptr) ||
            written == 0U) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

int wmain(const int argumentCount, wchar_t** const arguments)
{
    if (argumentCount != 3 || arguments == nullptr ||
        arguments[1] == nullptr || arguments[2] == nullptr ||
        std::wstring_view{arguments[1]} != HomeArgument) {
        return 2;
    }

    const std::wstring_view home{arguments[2]};
    if (!isBoundedAbsoluteLocalPath(home) ||
        home.size() + 1U + SentinelFileName.size() >
            MaximumPathCharacters) {
        return 3;
    }

    std::wstring sentinelPath{home};
    sentinelPath.push_back(L'\\');
    sentinelPath.append(SentinelFileName);

    UniqueHandle sentinel{::CreateFileW(
        sentinelPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr)};
    if (!sentinel.valid()) {
        return 4;
    }
    if (!writeAll(sentinel.get(), SentinelPayload)) {
        return 5;
    }
    return ::FlushFileBuffers(sentinel.get()) != FALSE ? 0 : 6;
}
