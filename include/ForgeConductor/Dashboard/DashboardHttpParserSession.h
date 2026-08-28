#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"
#include "ForgeConductor/Dashboard/DashboardHttpParser.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace ForgeConductor::Dashboard {

enum class DashboardHttpParserSessionState : std::uint8_t {
    ReceivingHeader,
    ReceivingBody,
    Complete,
    Rejected,
    Closed,
};

enum class DashboardHttpParserSessionError : std::uint8_t {
    None,
    HttpRejected,
    TrailingBytes,
    TerminalState,
    RequestNotReady,
    RequestAlreadyTaken,
    InternalFailure,
};

// Fixed-size operation result. HTTP-policy details remain available through
// DashboardHttpParserSession::rejection() when the state is Rejected.
class DashboardHttpParserSessionResult final {
public:
    [[nodiscard]] constexpr DashboardHttpParserSessionState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] constexpr DashboardHttpParserSessionError error() const noexcept
    {
        return error_;
    }

    [[nodiscard]] constexpr bool hasError() const noexcept
    {
        return error_ != DashboardHttpParserSessionError::None;
    }

    bool operator==(const DashboardHttpParserSessionResult&) const = default;

private:
    friend class DashboardHttpParserSession;

    constexpr DashboardHttpParserSessionResult(
        const DashboardHttpParserSessionState state,
        const DashboardHttpParserSessionError error) noexcept
        : state_{state}, error_{error}
    {
    }

    DashboardHttpParserSessionState state_{};
    DashboardHttpParserSessionError error_{};
};

// Move-only result ensures a successfully extracted request has one owner.
class DashboardHttpRequestTakeResult final {
public:
    DashboardHttpRequestTakeResult(const DashboardHttpRequestTakeResult&) = delete;
    DashboardHttpRequestTakeResult& operator=(
        const DashboardHttpRequestTakeResult&) = delete;
    DashboardHttpRequestTakeResult(DashboardHttpRequestTakeResult&&) noexcept =
        default;
    DashboardHttpRequestTakeResult& operator=(
        DashboardHttpRequestTakeResult&&) noexcept = default;

    [[nodiscard]] bool hasValue() const noexcept
    {
        return request_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] DashboardHttpParserSessionError error() const noexcept
    {
        return error_;
    }

    [[nodiscard]] const DashboardHttpRequest* request() const noexcept
    {
        return request_ ? &*request_ : nullptr;
    }

    [[nodiscard]] DashboardHttpRequest&& value() && noexcept
    {
        return std::move(*request_);
    }

private:
    friend class DashboardHttpParserSession;

    explicit DashboardHttpRequestTakeResult(
        DashboardHttpRequest request) noexcept
        : request_{std::move(request)}
    {
    }

    explicit constexpr DashboardHttpRequestTakeResult(
        const DashboardHttpParserSessionError error) noexcept
        : error_{error}
    {
    }

    std::optional<DashboardHttpRequest> request_;
    DashboardHttpParserSessionError error_{};
};

// One-shot incremental framing owner for exactly one strict HTTP request.
// Header bytes are scanned once as they arrive. The canonical parser is called
// only at the header boundary and at complete request framing, so appending
// fragments never reparses the accumulated prefix.
class DashboardHttpParserSession final {
public:
    static constexpr std::size_t HeaderDelimiterBytes = 4U;
    static constexpr std::size_t MaximumHeaderWireBytes =
        DashboardHttpParser::MaximumHeaderBytes + HeaderDelimiterBytes;
    static constexpr std::size_t MaximumOwnedRequestBytes =
        MaximumHeaderWireBytes + DashboardHttpParser::MaximumBodyBytes;
    static constexpr std::size_t MaximumOwnedStorageBytes =
        MaximumOwnedRequestBytes + 1U;

    DashboardHttpParserSession() noexcept = default;
    ~DashboardHttpParserSession() = default;

    DashboardHttpParserSession(const DashboardHttpParserSession&) = delete;
    DashboardHttpParserSession& operator=(
        const DashboardHttpParserSession&) = delete;
    DashboardHttpParserSession(DashboardHttpParserSession&&) = delete;
    DashboardHttpParserSession& operator=(DashboardHttpParserSession&&) = delete;

    [[nodiscard]] DashboardHttpParserSessionResult append(
        std::span<const std::byte> bytes) noexcept;

    // Signals that no more transport bytes can arrive. An incomplete request
    // becomes the same typed HTTP rejection produced by DashboardHttpParser.
    [[nodiscard]] DashboardHttpParserSessionResult finish() noexcept;

    // A successful take closes this one-shot session and releases its ingress
    // storage. The request can be moved out at most once.
    [[nodiscard]] DashboardHttpRequestTakeResult takeRequest() noexcept;

    [[nodiscard]] DashboardHttpParserSessionState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] std::size_t bufferedBytes() const noexcept
    {
        return bufferedBytes_;
    }

    [[nodiscard]] std::optional<std::size_t> expectedTotalBytes() const noexcept
    {
        return expectedTotalBytes_;
    }

    [[nodiscard]] std::optional<std::size_t> remainingBytes() const noexcept;

    [[nodiscard]] const DashboardHttpRejection* rejection() const noexcept
    {
        return rejection_ ? &*rejection_ : nullptr;
    }

private:
    [[nodiscard]] DashboardHttpParserSessionResult result(
        DashboardHttpParserSessionError error =
            DashboardHttpParserSessionError::None) const noexcept;
    [[nodiscard]] bool ensureCapacity(std::size_t capacity);
    [[nodiscard]] DashboardHttpParserSessionResult receiveHeaderByte(
        std::byte value);
    [[nodiscard]] DashboardHttpParserSessionResult validateHeader();
    [[nodiscard]] DashboardHttpParserSessionResult completeRequest();
    [[nodiscard]] DashboardHttpParserSessionResult rejectFrom(
        const DashboardHttpParseResult& parsed,
        DashboardHttpParserSessionError error);
    [[nodiscard]] DashboardHttpParserSessionResult rejectTrailingBytes(
        std::byte firstTrailingByte);
    void releaseIngressStorage() noexcept;
    void failInternal() noexcept;

    DashboardHttpParser parser_;
    std::unique_ptr<std::byte[]> storage_;
    std::size_t storageCapacity_{};
    std::size_t bufferedBytes_{};
    std::size_t delimiterProgress_{};
    std::optional<std::size_t> expectedTotalBytes_;
    std::optional<DashboardHttpRequest> request_;
    std::optional<DashboardHttpRejection> rejection_;
    DashboardHttpParserSessionState state_{
        DashboardHttpParserSessionState::ReceivingHeader};
    DashboardHttpParserSessionError terminalError_{
        DashboardHttpParserSessionError::None};
};

} // namespace ForgeConductor::Dashboard
