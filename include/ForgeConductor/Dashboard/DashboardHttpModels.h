#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Dashboard {

struct DashboardHttpHeader final {
    std::string name;
    std::string value;

    bool operator==(const DashboardHttpHeader&) const = default;
};

class DashboardHttpRequest final {
public:
    DashboardHttpRequest(
        std::string method,
        std::string target,
        std::vector<DashboardHttpHeader> headers,
        std::vector<std::byte> body)
        : method_{std::move(method)},
          target_{std::move(target)},
          headers_{std::move(headers)},
          body_{std::move(body)}
    {
    }

    [[nodiscard]] const std::string& method() const noexcept { return method_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
    [[nodiscard]] const std::vector<DashboardHttpHeader>& headers() const noexcept
    {
        return headers_;
    }
    [[nodiscard]] const std::vector<std::byte>& body() const noexcept
    {
        return body_;
    }

    [[nodiscard]] std::optional<std::string_view> header(
        std::string_view lowercaseName) const noexcept
    {
        for (const auto& entry : headers_) {
            if (entry.name == lowercaseName) {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string_view path() const noexcept
    {
        const auto query = target_.find('?');
        return std::string_view{target_}.substr(0U, query);
    }

    [[nodiscard]] bool isMutation() const noexcept
    {
        return method_ == "POST" || method_ == "PUT" || method_ == "PATCH" ||
            method_ == "DELETE";
    }

    bool operator==(const DashboardHttpRequest&) const = default;

private:
    std::string method_;
    std::string target_;
    std::vector<DashboardHttpHeader> headers_;
    std::vector<std::byte> body_;
};

struct DashboardHttpRejection final {
    std::uint16_t status{};
    std::string code;
    std::string message;
    std::vector<DashboardHttpHeader> responseHeaders;

    bool operator==(const DashboardHttpRejection&) const = default;
};

class DashboardHttpParseResult final {
public:
    enum class Kind { Incomplete, Accepted, Rejected };

    [[nodiscard]] static DashboardHttpParseResult needMoreData() noexcept
    {
        return DashboardHttpParseResult{};
    }

    [[nodiscard]] static DashboardHttpParseResult accepted(
        DashboardHttpRequest request,
        const std::size_t consumedBytes)
    {
        return DashboardHttpParseResult{
            std::move(request), consumedBytes};
    }

    [[nodiscard]] static DashboardHttpParseResult rejected(
        DashboardHttpRejection rejection)
    {
        return DashboardHttpParseResult{std::move(rejection)};
    }

    [[nodiscard]] Kind kind() const noexcept
    {
        if (std::holds_alternative<DashboardHttpRequest>(value_)) {
            return Kind::Accepted;
        }
        if (std::holds_alternative<DashboardHttpRejection>(value_)) {
            return Kind::Rejected;
        }
        return Kind::Incomplete;
    }

    [[nodiscard]] const DashboardHttpRequest* request() const noexcept
    {
        return std::get_if<DashboardHttpRequest>(&value_);
    }

    [[nodiscard]] const DashboardHttpRejection* rejection() const noexcept
    {
        return std::get_if<DashboardHttpRejection>(&value_);
    }

    [[nodiscard]] std::size_t consumedBytes() const noexcept
    {
        return consumedBytes_;
    }

private:
    DashboardHttpParseResult() = default;

    DashboardHttpParseResult(
        DashboardHttpRequest request,
        const std::size_t consumedBytes)
        : value_{std::move(request)}, consumedBytes_{consumedBytes}
    {
    }

    explicit DashboardHttpParseResult(DashboardHttpRejection rejection)
        : value_{std::move(rejection)}
    {
    }

    std::variant<std::monostate, DashboardHttpRequest, DashboardHttpRejection> value_;
    std::size_t consumedBytes_{};
};

} // namespace ForgeConductor::Dashboard
