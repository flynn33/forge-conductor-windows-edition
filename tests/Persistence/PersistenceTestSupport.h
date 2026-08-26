#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/Domain.h"
#include "Infrastructure/TestSupport.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace ForgeConductor::Tests::PersistenceSupport {

class FixedClock final : public Contracts::IClock {
public:
    FixedClock(
        Domain::UtcTimePoint utc,
        Domain::MonotonicTimePoint monotonic) noexcept
        : utc_{utc}, monotonic_{monotonic}
    {
    }

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override { return utc_; }
    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic_;
    }

private:
    Domain::UtcTimePoint utc_;
    Domain::MonotonicTimePoint monotonic_;
};

class ScopedTestDirectory final {
public:
    explicit ScopedTestDirectory(std::wstring_view label)
    {
        std::wstring temporary(32U * 1024U, L'\0');
        const DWORD length =
            ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0U && length < temporary.size(), "GetTempPathW failed");
        temporary.resize(length);

        LARGE_INTEGER counter{};
        require(::QueryPerformanceCounter(&counter) != FALSE,
                "QueryPerformanceCounter failed");
        path_ = std::filesystem::path{temporary} /
                (L"ForgeConductor-P07-" + std::wstring{label} + L"-" +
                 std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(static_cast<unsigned long long>(counter.QuadPart)));
        require(std::filesystem::create_directories(path_),
                "could not create a unique P07 test directory");
    }

    ~ScopedTestDirectory() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory(ScopedTestDirectory&&) = delete;
    ScopedTestDirectory& operator=(ScopedTestDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline Domain::OperationContext activeContext(
    std::string_view correlation = "p07-persistence-test")
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("11111111-1111-4111-8111-111111111111"),
        std::chrono::steady_clock::now() + std::chrono::minutes{2},
        {},
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] inline Domain::PathText pathText(const std::filesystem::path& value)
{
    return take(Domain::PathText::create(
        take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(value.native()))));
}

[[nodiscard]] inline std::string readFixture(
    const std::filesystem::path& path,
    const std::size_t maximumBytes = 8U * 1024U * 1024U)
{
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(path, sizeError);
    require(!sizeError && bytes <= maximumBytes,
            "fixture is missing or exceeds its bounded size");
    require(bytes <= static_cast<std::uintmax_t>(
                         (std::numeric_limits<std::streamsize>::max)()),
            "fixture size cannot be represented by the test stream");

    std::ifstream input{path, std::ios::binary};
    require(input.good(), "could not open persistence fixture");
    std::string value(static_cast<std::size_t>(bytes), '\0');
    if (!value.empty()) {
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
        require(input.gcount() == static_cast<std::streamsize>(value.size()),
                "could not read the complete persistence fixture");
    }
    require(input.peek() == std::char_traits<char>::eof(),
            "persistence fixture changed while it was read");
    return value;
}

} // namespace ForgeConductor::Tests::PersistenceSupport
