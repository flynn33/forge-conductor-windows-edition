#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Dashboard {

// Immutable, transport-neutral input for one complete HTTP/1.1 response.
// The encoder owns all framing and policy headers; callers may supply only
// explicitly vetted supplemental headers.
class DashboardHttpResponse final {
public:
    DashboardHttpResponse(
        const std::uint16_t status,
        std::string contentType,
        std::vector<std::byte> body,
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept
        : status_{status},
          contentType_{std::move(contentType)},
          body_{std::move(body)},
          extraHeaders_{std::move(extraHeaders)}
    {
    }

    [[nodiscard]] std::uint16_t status() const noexcept { return status_; }
    [[nodiscard]] const std::string& contentType() const noexcept
    {
        return contentType_;
    }
    [[nodiscard]] const std::vector<std::byte>& body() const noexcept
    {
        return body_;
    }
    [[nodiscard]] const std::vector<DashboardHttpHeader>& extraHeaders()
        const noexcept
    {
        return extraHeaders_;
    }

private:
    std::uint16_t status_{};
    std::string contentType_;
    std::vector<std::byte> body_;
    std::vector<DashboardHttpHeader> extraHeaders_;
};

// A HEAD response advertises the representation length but never owns or emits
// its bytes. Keeping this distinct prevents an error response to an unsupported
// HEAD request from accidentally writing a body.
class DashboardHttpHeadResponse final {
public:
    DashboardHttpHeadResponse(
        const std::uint16_t status,
        std::string contentType,
        const std::size_t representationLength,
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept
        : status_{status},
          contentType_{std::move(contentType)},
          representationLength_{representationLength},
          extraHeaders_{std::move(extraHeaders)}
    {
    }

    [[nodiscard]] std::uint16_t status() const noexcept { return status_; }
    [[nodiscard]] const std::string& contentType() const noexcept
    {
        return contentType_;
    }
    [[nodiscard]] std::size_t representationLength() const noexcept
    {
        return representationLength_;
    }
    [[nodiscard]] const std::vector<DashboardHttpHeader>& extraHeaders()
        const noexcept
    {
        return extraHeaders_;
    }

private:
    std::uint16_t status_{};
    std::string contentType_;
    std::size_t representationLength_{};
    std::vector<DashboardHttpHeader> extraHeaders_;
};

// SSE framing has no Content-Length and remains open after this head is sent.
// Keeping it as a distinct model prevents accidental use of complete-response
// connection semantics for a live stream.
class DashboardSseBootstrap final {
public:
    explicit DashboardSseBootstrap(
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept
        : extraHeaders_{std::move(extraHeaders)}
    {
    }

    [[nodiscard]] const std::vector<DashboardHttpHeader>& extraHeaders()
        const noexcept
    {
        return extraHeaders_;
    }

private:
    std::vector<DashboardHttpHeader> extraHeaders_;
};

enum class DashboardHttpEncodingError : std::uint8_t {
    None,
    UnsupportedStatus,
    BodyNotAllowed,
    InvalidContentType,
    TooManyExtraHeaders,
    InvalidHeaderName,
    InvalidHeaderValue,
    UnsupportedExtraHeader,
    DuplicateHeader,
    HeaderTooLarge,
    ResponseTooLarge,
    InternalFailure,
};

class DashboardHttpEncodingResult final {
public:
    enum class Kind : std::uint8_t {
        Failure,
        CompleteResponse,
        HeadResponseHead,
        SseBootstrapHead,
    };

    [[nodiscard]] bool hasValue() const noexcept
    {
        return kind_ != Kind::Failure;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] DashboardHttpEncodingError error() const noexcept
    {
        return error_;
    }
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
    {
        return bytes_;
    }

private:
    friend class DashboardHttpResponseEncoder;

    [[nodiscard]] static DashboardHttpEncodingResult success(
        Kind kind,
        std::vector<std::byte> bytes) noexcept
    {
        return DashboardHttpEncodingResult{kind, std::move(bytes)};
    }

    [[nodiscard]] static DashboardHttpEncodingResult failure(
        const DashboardHttpEncodingError error) noexcept
    {
        return DashboardHttpEncodingResult{error};
    }

    DashboardHttpEncodingResult(
        const Kind kind,
        std::vector<std::byte> bytes) noexcept
        : bytes_{std::move(bytes)}, kind_{kind}
    {
    }

    explicit DashboardHttpEncodingResult(
        const DashboardHttpEncodingError error) noexcept
        : error_{error}
    {
    }

    std::vector<std::byte> bytes_;
    Kind kind_{Kind::Failure};
    DashboardHttpEncodingError error_{DashboardHttpEncodingError::None};
};

class DashboardHttpResponseEncoder final {
public:
    static constexpr std::size_t MaximumEncodedResponseBytes =
        2U * 1024U * 1024U;
    static constexpr std::size_t MaximumEncodedHeaderBytes = 16U * 1024U;
    static constexpr std::size_t MaximumExtraHeaderCount = 8U;
    static constexpr std::size_t MaximumExtraHeaderNameBytes = 64U;
    static constexpr std::size_t MaximumExtraHeaderValueBytes = 2U * 1024U;
    static constexpr std::size_t MaximumContentTypeBytes = 256U;

    [[nodiscard]] static DashboardHttpEncodingResult encode(
        const DashboardHttpResponse& response) noexcept;

    [[nodiscard]] static DashboardHttpEncodingResult encodeHead(
        const DashboardHttpHeadResponse& response) noexcept;

    [[nodiscard]] static DashboardHttpEncodingResult encodeSseBootstrap(
        const DashboardSseBootstrap& bootstrap = DashboardSseBootstrap{})
        noexcept;

private:
    [[nodiscard]] static DashboardHttpEncodingResult encodeFixedResponse(
        std::uint16_t status,
        std::string_view contentType,
        std::span<const std::byte> body,
        std::size_t representationLength,
        std::span<const DashboardHttpHeader> extraHeaders,
        DashboardHttpEncodingResult::Kind kind) noexcept;
};

} // namespace ForgeConductor::Dashboard
