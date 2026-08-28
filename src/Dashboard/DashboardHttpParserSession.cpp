#include "ForgeConductor/Dashboard/DashboardHttpParserSession.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::byte HeaderDelimiter[]{
    std::byte{static_cast<unsigned char>('\r')},
    std::byte{static_cast<unsigned char>('\n')},
    std::byte{static_cast<unsigned char>('\r')},
    std::byte{static_cast<unsigned char>('\n')},
};

[[nodiscard]] std::string_view byteView(
    const std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty()) {
        return {};
    }
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()};
}

[[nodiscard]] char asciiLower(const char value) noexcept
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool asciiEqualsIgnoreCase(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index{}; index < left.size(); ++index) {
        if (asciiLower(left[index]) != asciiLower(right[index])) {
            return false;
        }
    }
    return true;
}

// DashboardHttpParser has already validated the complete header before this
// helper runs. This scan only recovers its canonical Content-Length framing
// value; it does not make an independent HTTP-policy decision.
[[nodiscard]] bool extractValidatedContentLength(
    const std::span<const std::byte> headerWire,
    std::size_t& contentLength) noexcept
{
    constexpr std::string_view LineDelimiter{"\r\n"};
    constexpr std::string_view ContentLength{"content-length"};

    if (headerWire.size() < DashboardHttpParserSession::HeaderDelimiterBytes) {
        return false;
    }
    const auto headerText = byteView(headerWire.first(
        headerWire.size() - DashboardHttpParserSession::HeaderDelimiterBytes));
    const auto requestLineEnd = headerText.find(LineDelimiter);
    auto cursor = requestLineEnd == std::string_view::npos
        ? headerText.size()
        : requestLineEnd + LineDelimiter.size();

    contentLength = 0U;
    while (cursor < headerText.size()) {
        const auto lineEnd = headerText.find(LineDelimiter, cursor);
        const auto end = lineEnd == std::string_view::npos
            ? headerText.size()
            : lineEnd;
        const auto line = headerText.substr(cursor, end - cursor);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }

        if (asciiEqualsIgnoreCase(line.substr(0U, colon), ContentLength)) {
            auto value = line.substr(colon + 1U);
            while (!value.empty() &&
                   (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1U);
            }
            while (!value.empty() &&
                   (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1U);
            }
            if (value.empty()) {
                return false;
            }

            std::size_t parsed{};
            for (const char character : value) {
                if (character < '0' || character > '9') {
                    return false;
                }
                const auto digit =
                    static_cast<std::size_t>(character - '0');
                if (parsed >
                    ((std::numeric_limits<std::size_t>::max)() - digit) /
                        10U) {
                    return false;
                }
                parsed = parsed * 10U + digit;
            }
            contentLength = parsed;
            return true;
        }

        cursor = lineEnd == std::string_view::npos
            ? headerText.size()
            : lineEnd + LineDelimiter.size();
    }
    return true;
}

} // namespace

DashboardHttpParserSessionResult DashboardHttpParserSession::append(
    const std::span<const std::byte> bytes) noexcept
{
    try {
        if (state_ == DashboardHttpParserSessionState::Rejected ||
            state_ == DashboardHttpParserSessionState::Closed) {
            return result(DashboardHttpParserSessionError::TerminalState);
        }
        if (bytes.empty()) {
            return result();
        }
        if (state_ == DashboardHttpParserSessionState::Complete) {
            return rejectTrailingBytes(bytes.front());
        }

        std::size_t offset{};
        while (offset < bytes.size()) {
            if (state_ == DashboardHttpParserSessionState::ReceivingHeader) {
                const auto update = receiveHeaderByte(bytes[offset]);
                ++offset;
                if (update.hasError()) {
                    return update;
                }
                if (state_ == DashboardHttpParserSessionState::Complete) {
                    if (offset < bytes.size()) {
                        return rejectTrailingBytes(bytes[offset]);
                    }
                    return result();
                }
                continue;
            }

            if (state_ == DashboardHttpParserSessionState::ReceivingBody) {
                const auto remaining = *expectedTotalBytes_ - bufferedBytes_;
                const auto available = bytes.size() - offset;
                if (available > remaining) {
                    if (remaining != 0U) {
                        std::memcpy(
                            storage_.get() + bufferedBytes_,
                            bytes.data() + offset,
                            remaining);
                        bufferedBytes_ += remaining;
                    }
                    return rejectTrailingBytes(bytes[offset + remaining]);
                }
                if (available != 0U) {
                    std::memcpy(
                        storage_.get() + bufferedBytes_,
                        bytes.data() + offset,
                        available);
                    bufferedBytes_ += available;
                    offset += available;
                }
                if (bufferedBytes_ == *expectedTotalBytes_) {
                    return completeRequest();
                }
            }
        }
        return result();
    } catch (...) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
}

DashboardHttpParserSessionResult DashboardHttpParserSession::finish() noexcept
{
    try {
        if (state_ == DashboardHttpParserSessionState::Complete) {
            return result();
        }
        if (state_ == DashboardHttpParserSessionState::Rejected ||
            state_ == DashboardHttpParserSessionState::Closed) {
            return result(DashboardHttpParserSessionError::TerminalState);
        }

        const auto wire = storage_
            ? std::span<const std::byte>{storage_.get(), bufferedBytes_}
            : std::span<const std::byte>{};
        const auto parsed = parser_.parse(wire, true);
        if (parsed.kind() == DashboardHttpParseResult::Kind::Rejected) {
            return rejectFrom(
                parsed, DashboardHttpParserSessionError::HttpRejected);
        }
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    } catch (...) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
}

DashboardHttpRequestTakeResult DashboardHttpParserSession::takeRequest() noexcept
{
    if (state_ == DashboardHttpParserSessionState::Closed) {
        return DashboardHttpRequestTakeResult{
            DashboardHttpParserSessionError::RequestAlreadyTaken};
    }
    if (state_ == DashboardHttpParserSessionState::Rejected) {
        return DashboardHttpRequestTakeResult{terminalError_};
    }
    if (state_ != DashboardHttpParserSessionState::Complete || !request_) {
        return DashboardHttpRequestTakeResult{
            DashboardHttpParserSessionError::RequestNotReady};
    }

    auto request = std::move(*request_);
    request_.reset();
    rejection_.reset();
    storage_.reset();
    storageCapacity_ = 0U;
    bufferedBytes_ = 0U;
    delimiterProgress_ = 0U;
    expectedTotalBytes_.reset();
    state_ = DashboardHttpParserSessionState::Closed;
    terminalError_ = DashboardHttpParserSessionError::None;
    return DashboardHttpRequestTakeResult{std::move(request)};
}

std::optional<std::size_t> DashboardHttpParserSession::remainingBytes()
    const noexcept
{
    if (!expectedTotalBytes_) {
        return std::nullopt;
    }
    if (bufferedBytes_ >= *expectedTotalBytes_) {
        return 0U;
    }
    return *expectedTotalBytes_ - bufferedBytes_;
}

DashboardHttpParserSessionResult DashboardHttpParserSession::result(
    const DashboardHttpParserSessionError error) const noexcept
{
    return DashboardHttpParserSessionResult{state_, error};
}

bool DashboardHttpParserSession::ensureCapacity(const std::size_t capacity)
{
    if (capacity <= storageCapacity_) {
        return true;
    }
    if (capacity > MaximumOwnedStorageBytes) {
        return false;
    }

    auto replacement = std::make_unique<std::byte[]>(capacity);
    if (bufferedBytes_ != 0U) {
        std::memcpy(replacement.get(), storage_.get(), bufferedBytes_);
    }
    storage_ = std::move(replacement);
    storageCapacity_ = capacity;
    return true;
}

DashboardHttpParserSessionResult DashboardHttpParserSession::receiveHeaderByte(
    const std::byte value)
{
    if (!ensureCapacity(MaximumHeaderWireBytes)) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
    if (bufferedBytes_ >= MaximumHeaderWireBytes) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }

    storage_[bufferedBytes_] = value;
    ++bufferedBytes_;

    if (value == HeaderDelimiter[delimiterProgress_]) {
        ++delimiterProgress_;
    } else {
        delimiterProgress_ = value == HeaderDelimiter[0] ? 1U : 0U;
    }

    if (delimiterProgress_ == HeaderDelimiterBytes) {
        return validateHeader();
    }
    if (bufferedBytes_ == MaximumHeaderWireBytes) {
        const auto parsed = parser_.parse(
            std::span<const std::byte>{storage_.get(), bufferedBytes_}, false);
        if (parsed.kind() == DashboardHttpParseResult::Kind::Rejected) {
            return rejectFrom(
                parsed, DashboardHttpParserSessionError::HttpRejected);
        }
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
    return result();
}

DashboardHttpParserSessionResult DashboardHttpParserSession::validateHeader()
{
    const auto wire =
        std::span<const std::byte>{storage_.get(), bufferedBytes_};
    const auto parsed = parser_.parse(wire, false);
    if (parsed.kind() == DashboardHttpParseResult::Kind::Rejected) {
        return rejectFrom(parsed, DashboardHttpParserSessionError::HttpRejected);
    }

    if (parsed.kind() == DashboardHttpParseResult::Kind::Accepted) {
        if (parsed.request() == nullptr ||
            parsed.consumedBytes() != bufferedBytes_) {
            failInternal();
            return result(DashboardHttpParserSessionError::InternalFailure);
        }
        if (!ensureCapacity(bufferedBytes_ + 1U)) {
            failInternal();
            return result(DashboardHttpParserSessionError::InternalFailure);
        }
        expectedTotalBytes_ = bufferedBytes_;
        request_.emplace(*parsed.request());
        state_ = DashboardHttpParserSessionState::Complete;
        terminalError_ = DashboardHttpParserSessionError::None;
        return result();
    }

    std::size_t contentLength{};
    if (!extractValidatedContentLength(wire, contentLength) ||
        contentLength == 0U ||
        contentLength > DashboardHttpParser::MaximumBodyBytes ||
        bufferedBytes_ > MaximumOwnedRequestBytes - contentLength) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }

    expectedTotalBytes_ = bufferedBytes_ + contentLength;
    if (!ensureCapacity(*expectedTotalBytes_ + 1U)) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
    state_ = DashboardHttpParserSessionState::ReceivingBody;
    terminalError_ = DashboardHttpParserSessionError::None;
    return result();
}

DashboardHttpParserSessionResult DashboardHttpParserSession::completeRequest()
{
    const auto parsed = parser_.parse(
        std::span<const std::byte>{storage_.get(), bufferedBytes_}, false);
    if (parsed.kind() == DashboardHttpParseResult::Kind::Rejected) {
        return rejectFrom(parsed, DashboardHttpParserSessionError::HttpRejected);
    }
    if (parsed.kind() != DashboardHttpParseResult::Kind::Accepted ||
        parsed.request() == nullptr ||
        parsed.consumedBytes() != bufferedBytes_) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }

    request_.emplace(*parsed.request());
    state_ = DashboardHttpParserSessionState::Complete;
    terminalError_ = DashboardHttpParserSessionError::None;
    return result();
}

DashboardHttpParserSessionResult DashboardHttpParserSession::rejectFrom(
    const DashboardHttpParseResult& parsed,
    const DashboardHttpParserSessionError error)
{
    if (parsed.kind() != DashboardHttpParseResult::Kind::Rejected ||
        parsed.rejection() == nullptr) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }

    rejection_.emplace(*parsed.rejection());
    request_.reset();
    releaseIngressStorage();
    state_ = DashboardHttpParserSessionState::Rejected;
    terminalError_ = error;
    return result(error);
}

DashboardHttpParserSessionResult
DashboardHttpParserSession::rejectTrailingBytes(
    const std::byte firstTrailingByte)
{
    if (!storage_ || bufferedBytes_ >= storageCapacity_) {
        failInternal();
        return result(DashboardHttpParserSessionError::InternalFailure);
    }
    storage_[bufferedBytes_] = firstTrailingByte;
    const auto parsed = parser_.parse(
        std::span<const std::byte>{storage_.get(), bufferedBytes_ + 1U},
        false);
    return rejectFrom(
        parsed, DashboardHttpParserSessionError::TrailingBytes);
}

void DashboardHttpParserSession::releaseIngressStorage() noexcept
{
    storage_.reset();
    storageCapacity_ = 0U;
    bufferedBytes_ = 0U;
    delimiterProgress_ = 0U;
    expectedTotalBytes_.reset();
}

void DashboardHttpParserSession::failInternal() noexcept
{
    request_.reset();
    rejection_.reset();
    releaseIngressStorage();
    state_ = DashboardHttpParserSessionState::Rejected;
    terminalError_ = DashboardHttpParserSessionError::InternalFailure;
}

} // namespace ForgeConductor::Dashboard
