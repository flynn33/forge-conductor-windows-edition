#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"
#include "ForgeConductor/Domain/Result.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Dashboard {

enum class DashboardPostDeliveryAction : std::uint8_t {
    None,
    RequestManagerShutdown,
};

class IDashboardSseReadySink {
public:
    virtual ~IDashboardSseReadySink() noexcept = default;

    // Signals that a previously empty subscription may now have one frame.
    // The notification carries no work item and therefore cannot form a
    // callback backlog.
    virtual void signal() noexcept = 0;
};

// One producer frame with both wire representations. The pair and its byte
// buffers are shared as immutable values across all live stream mailboxes.
class DashboardSseFramePair final {
public:
    using ImmutableBytes = std::shared_ptr<const std::vector<std::byte>>;
    using ImmutableFrame = std::shared_ptr<const DashboardSseFramePair>;

    static constexpr std::size_t MaximumFrameBytes =
        DashboardHttpResponseEncoder::MaximumEncodedResponseBytes;

    [[nodiscard]] static Domain::Result<ImmutableFrame> create(
        std::uint64_t sourceSequence,
        const std::vector<std::byte>& compactBytes,
        const std::vector<std::byte>& fullBytes) noexcept
    {
        try {
            if (compactBytes.empty() || fullBytes.empty()) {
                return Domain::Result<ImmutableFrame>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The telemetry producer supplied a missing or empty "
                        "dashboard SSE frame."));
            }
            if (compactBytes.size() > MaximumFrameBytes ||
                fullBytes.size() > MaximumFrameBytes) {
                return Domain::Result<ImmutableFrame>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "A dashboard SSE frame exceeds the response byte "
                        "limit."));
            }

            ImmutableFrame frame{new DashboardSseFramePair{
                sourceSequence,
                std::make_shared<const std::vector<std::byte>>(compactBytes),
                std::make_shared<const std::vector<std::byte>>(fullBytes)}};
            return Domain::Result<ImmutableFrame>::success(std::move(frame));
        } catch (...) {
            return Domain::Result<ImmutableFrame>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard SSE frame pair could not be created."));
        }
    }

    [[nodiscard]] std::uint64_t sourceSequence() const noexcept
    {
        return sourceSequence_;
    }

    [[nodiscard]] const ImmutableBytes& compactBytes() const noexcept
    {
        return compactBytes_;
    }

    [[nodiscard]] const ImmutableBytes& fullBytes() const noexcept
    {
        return fullBytes_;
    }

private:
    DashboardSseFramePair(
        const std::uint64_t sourceSequence,
        ImmutableBytes compactBytes,
        ImmutableBytes fullBytes) noexcept
        : sourceSequence_{sourceSequence},
          compactBytes_{std::move(compactBytes)},
          fullBytes_{std::move(fullBytes)}
    {
    }

    const std::uint64_t sourceSequence_{};
    const ImmutableBytes compactBytes_;
    const ImmutableBytes fullBytes_;
};

// Per-connection latest-value mailbox. Implementations have capacity exactly
// one: publishing a newer frame replaces the unsent frame, takeLatest clears
// it atomically, and pendingCount is always zero or one. close discards the
// pending frame, detaches the sink, and makes subsequent takes empty. close is
// idempotent and must remain safe when an owner closes before destruction.
class IDashboardSseSubscription {
public:
    static constexpr double MinimumDeliveryHz = 1.0;
    static constexpr double MaximumDeliveryHz = 2.0;

    virtual ~IDashboardSseSubscription() noexcept = default;

    // The validated per-connection delivery rate survives application-to-
    // transport type erasure. Implementations return a finite value in the
    // closed Windows profile range [1, 2] Hz.
    [[nodiscard]] virtual double deliveryHz() const noexcept = 0;

    virtual void attachReadySink(
        std::weak_ptr<IDashboardSseReadySink> sink) noexcept = 0;

    [[nodiscard]] virtual DashboardSseFramePair::ImmutableFrame takeLatest()
        noexcept = 0;

    [[nodiscard]] virtual std::size_t pendingCount() const noexcept = 0;

    virtual void close() noexcept = 0;
};

class DashboardCompleteExchange final {
public:
    DashboardCompleteExchange(const DashboardCompleteExchange&) = delete;
    DashboardCompleteExchange& operator=(const DashboardCompleteExchange&) =
        delete;
    DashboardCompleteExchange(DashboardCompleteExchange&& other) noexcept
        : encodedResponse_{std::move(other.encodedResponse_)},
          postDeliveryAction_{std::exchange(
              other.postDeliveryAction_,
              DashboardPostDeliveryAction::None)}
    {
    }

    DashboardCompleteExchange& operator=(
        DashboardCompleteExchange&& other) noexcept
    {
        if (this != &other) {
            encodedResponse_ = std::move(other.encodedResponse_);
            postDeliveryAction_ = std::exchange(
                other.postDeliveryAction_,
                DashboardPostDeliveryAction::None);
        }
        return *this;
    }
    ~DashboardCompleteExchange() = default;

    [[nodiscard]] static Domain::Result<DashboardCompleteExchange> create(
        DashboardHttpEncodingResult encodedResponse,
        const DashboardPostDeliveryAction postDeliveryAction) noexcept
    {
        try {
            const auto kind = encodedResponse.kind();
            const bool validKind =
                kind == DashboardHttpEncodingResult::Kind::CompleteResponse ||
                kind == DashboardHttpEncodingResult::Kind::HeadResponseHead;
            const bool definedAction =
                postDeliveryAction == DashboardPostDeliveryAction::None ||
                postDeliveryAction ==
                    DashboardPostDeliveryAction::RequestManagerShutdown;
            if (!encodedResponse.hasValue() || !validKind ||
                encodedResponse.bytes().empty() || !definedAction ||
                (kind ==
                     DashboardHttpEncodingResult::Kind::HeadResponseHead &&
                 postDeliveryAction != DashboardPostDeliveryAction::None)) {
                return Domain::Result<DashboardCompleteExchange>::failure(
                    invalidExchangeError(
                        "A complete dashboard exchange requires a successful "
                        "complete or HEAD encoding and a compatible defined "
                        "action."));
            }
            return Domain::Result<DashboardCompleteExchange>::success(
                DashboardCompleteExchange{
                    std::move(encodedResponse), postDeliveryAction});
        } catch (...) {
            return Domain::Result<DashboardCompleteExchange>::failure(
                internalExchangeError());
        }
    }

    [[nodiscard]] const DashboardHttpEncodingResult& encodedResponse()
        const noexcept
    {
        return encodedResponse_;
    }

    [[nodiscard]] DashboardPostDeliveryAction takePostDeliveryAction() noexcept
    {
        return std::exchange(
            postDeliveryAction_, DashboardPostDeliveryAction::None);
    }

private:
    friend class DashboardPreparedExchange;

    DashboardCompleteExchange(
        DashboardHttpEncodingResult encodedResponse,
        const DashboardPostDeliveryAction postDeliveryAction) noexcept
        : encodedResponse_{std::move(encodedResponse)},
          postDeliveryAction_{postDeliveryAction}
    {
    }

    [[nodiscard]] static Domain::Error invalidExchangeError(
        std::string_view message)
    {
        return Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure, std::string{message});
    }

    [[nodiscard]] static Domain::Error internalExchangeError()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The complete dashboard exchange could not be created.");
    }

    DashboardHttpEncodingResult encodedResponse_;
    DashboardPostDeliveryAction postDeliveryAction_{};
};

class DashboardSseExchange final {
public:
    static constexpr std::string_view ConnectedCommentText =
        ": connected realtime\n\n";

    DashboardSseExchange(const DashboardSseExchange&) = delete;
    DashboardSseExchange& operator=(const DashboardSseExchange&) = delete;

    DashboardSseExchange(DashboardSseExchange&&) noexcept = default;

    DashboardSseExchange& operator=(DashboardSseExchange&& other) noexcept
    {
        if (this != &other) {
            close();
            encodedHead_ = std::move(other.encodedHead_);
            connectedCommentBytes_ =
                std::move(other.connectedCommentBytes_);
            subscription_ = std::move(other.subscription_);
        }
        return *this;
    }

    ~DashboardSseExchange() noexcept { close(); }

    [[nodiscard]] static Domain::Result<DashboardSseExchange> create(
        DashboardHttpEncodingResult encodedHead,
        std::unique_ptr<IDashboardSseSubscription> subscription) noexcept
    {
        try {
            const auto deliveryHz = subscription == nullptr
                ? 0.0
                : subscription->deliveryHz();
            if (!encodedHead.hasValue() ||
                encodedHead.kind() !=
                    DashboardHttpEncodingResult::Kind::SseBootstrapHead ||
                encodedHead.bytes().empty() || subscription == nullptr ||
                !std::isfinite(deliveryHz) ||
                deliveryHz < IDashboardSseSubscription::MinimumDeliveryHz ||
                deliveryHz > IDashboardSseSubscription::MaximumDeliveryHz) {
                closeSubscription(subscription);
                return Domain::Result<DashboardSseExchange>::failure(
                    invalidExchangeError());
            }

            auto connectedComment = makeConnectedCommentBytes();
            return Domain::Result<DashboardSseExchange>::success(
                DashboardSseExchange{
                    std::move(encodedHead),
                    std::move(connectedComment),
                    std::move(subscription)});
        } catch (...) {
            closeSubscription(subscription);
            return Domain::Result<DashboardSseExchange>::failure(
                internalExchangeError());
        }
    }

    [[nodiscard]] const DashboardHttpEncodingResult& encodedHead()
        const noexcept
    {
        return encodedHead_;
    }

    [[nodiscard]] const std::vector<std::byte>& connectedCommentBytes()
        const noexcept
    {
        return connectedCommentBytes_;
    }

    [[nodiscard]] IDashboardSseSubscription* subscription() noexcept
    {
        return subscription_.get();
    }

    [[nodiscard]] const IDashboardSseSubscription* subscription()
        const noexcept
    {
        return subscription_.get();
    }

    void close() noexcept
    {
        closeSubscription(subscription_);
    }

private:
    friend class DashboardPreparedExchange;

    DashboardSseExchange(
        DashboardHttpEncodingResult encodedHead,
        std::vector<std::byte> connectedCommentBytes,
        std::unique_ptr<IDashboardSseSubscription> subscription) noexcept
        : encodedHead_{std::move(encodedHead)},
          connectedCommentBytes_{std::move(connectedCommentBytes)},
          subscription_{std::move(subscription)}
    {
    }

    [[nodiscard]] static std::vector<std::byte> makeConnectedCommentBytes()
    {
        std::vector<std::byte> result;
        result.reserve(ConnectedCommentText.size());
        for (const unsigned char character : ConnectedCommentText) {
            result.push_back(static_cast<std::byte>(character));
        }
        return result;
    }

    [[nodiscard]] static Domain::Error invalidExchangeError()
    {
        return Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "An SSE dashboard exchange requires a successful SSE bootstrap "
            "encoding and a unique subscription with a valid delivery rate.");
    }

    [[nodiscard]] static Domain::Error internalExchangeError()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The SSE dashboard exchange could not be created.");
    }

    static void closeSubscription(
        std::unique_ptr<IDashboardSseSubscription>& subscription) noexcept
    {
        auto owned = std::move(subscription);
        if (owned != nullptr) {
            owned->close();
        }
    }

    DashboardHttpEncodingResult encodedHead_;
    std::vector<std::byte> connectedCommentBytes_;
    std::unique_ptr<IDashboardSseSubscription> subscription_;
};

// The application-to-transport boundary is closed to exactly one complete
// response or one live SSE response. It is move-only so the response bytes,
// post-delivery action, and stream subscription have one lifetime owner.
class DashboardPreparedExchange final {
public:
    enum class Kind : std::uint8_t {
        Empty,
        Complete,
        ServerSentEvents,
    };

    DashboardPreparedExchange(const DashboardPreparedExchange&) = delete;
    DashboardPreparedExchange& operator=(const DashboardPreparedExchange&) =
        delete;
    DashboardPreparedExchange(DashboardPreparedExchange&& other) noexcept
        : value_{std::move(other.value_)}
    {
        other.value_.template emplace<std::monostate>();
    }

    DashboardPreparedExchange& operator=(
        DashboardPreparedExchange&& other) noexcept
    {
        if (this != &other) {
            value_ = std::move(other.value_);
            other.value_.template emplace<std::monostate>();
        }
        return *this;
    }
    ~DashboardPreparedExchange() = default;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    createComplete(
        DashboardHttpEncodingResult encodedResponse,
        const DashboardPostDeliveryAction postDeliveryAction) noexcept
    {
        auto complete = DashboardCompleteExchange::create(
            std::move(encodedResponse), postDeliveryAction);
        if (!complete) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                std::move(complete).error());
        }
        return Domain::Result<DashboardPreparedExchange>::success(
            DashboardPreparedExchange{std::move(complete).value()});
    }

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange> createSse(
        DashboardHttpEncodingResult encodedHead,
        std::unique_ptr<IDashboardSseSubscription> subscription) noexcept
    {
        auto sse = DashboardSseExchange::create(
            std::move(encodedHead), std::move(subscription));
        if (!sse) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                std::move(sse).error());
        }
        return Domain::Result<DashboardPreparedExchange>::success(
            DashboardPreparedExchange{std::move(sse).value()});
    }

    [[nodiscard]] Kind kind() const noexcept
    {
        if (std::holds_alternative<DashboardCompleteExchange>(value_)) {
            return Kind::Complete;
        }
        if (std::holds_alternative<DashboardSseExchange>(value_)) {
            return Kind::ServerSentEvents;
        }
        return Kind::Empty;
    }

    [[nodiscard]] DashboardCompleteExchange* completeExchange() noexcept
    {
        return std::get_if<DashboardCompleteExchange>(&value_);
    }

    [[nodiscard]] const DashboardCompleteExchange* completeExchange()
        const noexcept
    {
        return std::get_if<DashboardCompleteExchange>(&value_);
    }

    [[nodiscard]] DashboardSseExchange* sseExchange() noexcept
    {
        return std::get_if<DashboardSseExchange>(&value_);
    }

    [[nodiscard]] const DashboardSseExchange* sseExchange() const noexcept
    {
        return std::get_if<DashboardSseExchange>(&value_);
    }

private:
    explicit DashboardPreparedExchange(
        DashboardCompleteExchange exchange) noexcept
        : value_{std::move(exchange)}
    {
    }

    explicit DashboardPreparedExchange(DashboardSseExchange exchange) noexcept
        : value_{std::move(exchange)}
    {
    }

    std::variant<
        std::monostate,
        DashboardCompleteExchange,
        DashboardSseExchange>
        value_;
};

} // namespace ForgeConductor::Dashboard
