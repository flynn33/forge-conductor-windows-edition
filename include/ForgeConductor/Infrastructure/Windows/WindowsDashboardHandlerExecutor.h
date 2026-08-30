#pragma once

#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace ForgeConductor::Infrastructure::Windows {

enum class DashboardHandlerCompletionKind : std::uint8_t {
    PreparedExchange,
    PostDelivery,
};

// Closed move-only result passed from the blocking handler pool back to the
// socket runtime. The two alternatives correspond to the only application
// operations permitted by the dashboard transport boundary.
class DashboardHandlerCompletion final {
public:
    using PreparedResult =
        Domain::Result<Dashboard::DashboardPreparedExchange>;
    using PostDeliveryResult = Domain::Result<void>;

    DashboardHandlerCompletion(const DashboardHandlerCompletion&) = delete;
    DashboardHandlerCompletion& operator=(
        const DashboardHandlerCompletion&) = delete;
    DashboardHandlerCompletion(DashboardHandlerCompletion&&) noexcept =
        default;
    DashboardHandlerCompletion& operator=(
        DashboardHandlerCompletion&&) noexcept = default;
    ~DashboardHandlerCompletion() = default;

    [[nodiscard]] static DashboardHandlerCompletion prepared(
        PreparedResult result) noexcept;

    [[nodiscard]] static DashboardHandlerCompletion postDelivery(
        PostDeliveryResult result) noexcept;

    [[nodiscard]] DashboardHandlerCompletionKind kind() const noexcept;

    [[nodiscard]] PreparedResult* preparedResult() noexcept;
    [[nodiscard]] const PreparedResult* preparedResult() const noexcept;

    [[nodiscard]] PostDeliveryResult* postDeliveryResult() noexcept;
    [[nodiscard]] const PostDeliveryResult* postDeliveryResult() const noexcept;

private:
    explicit DashboardHandlerCompletion(PreparedResult result) noexcept;
    explicit DashboardHandlerCompletion(PostDeliveryResult result) noexcept;

    std::variant<PreparedResult, PostDeliveryResult> value_;
};

// A transport-owned operation is moved into the executor. execute deliberately
// is not noexcept: the executor converts every escaped exception to a typed
// InternalFailure completion of completionKind(). Implementations must not
// retain references to the supplied context and must observe its cancellation
// and deadline while blocking.
class IDashboardHandlerOperation {
public:
    virtual ~IDashboardHandlerOperation() noexcept = default;

    [[nodiscard]] virtual DashboardHandlerCompletionKind completionKind()
        const noexcept = 0;

    [[nodiscard]] virtual DashboardHandlerCompletion execute(
        const Domain::OperationContext& context) = 0;
};

// The executor weakly retains this sink and invokes it at most once per
// accepted task. tryPost must perform one nonblocking admission attempt into a
// bounded transport-owned completion mechanism (normally an IOCP operation
// registration plus PostQueuedCompletionStatus). It consumes completion even
// when returning false and may not call back into a destroyed executor.
class IDashboardHandlerCompletionSink {
public:
    virtual ~IDashboardHandlerCompletionSink() noexcept = default;

    [[nodiscard]] virtual bool tryPost(
        DashboardHandlerCompletion completion) noexcept = 0;
};

// Optional process-owned one-shot edge emitted only when shutdown has stopped
// admission, every accepted task has settled, every worker thread handle has
// been joined, and every reservation has returned its capacity. Standalone
// executors need not bind it. Process composition must bind before shutdown
// and retain the observer through the exact drain edge. The executor retains
// it weakly to keep process ownership acyclic. Implementations must remain
// nonblocking and only latch the edge.
class IDashboardHandlerExecutorDrainObserver {
public:
    virtual ~IDashboardHandlerExecutorDrainObserver() noexcept = default;

    virtual void handlerExecutorMayHaveDrained() noexcept = 0;
};

// Fixed blocking-work boundary for the loopback dashboard. The executor owns
// exactly four persistent workers. Pending move-only tasks and live
// post-delivery reservations share one capacity of eight. It never creates a
// thread for a connection. Submission and completion posting are nonblocking.
class WindowsDashboardHandlerExecutor final {
private:
    class Impl;

public:
    static constexpr std::size_t WorkerCount = 4U;
    static constexpr std::size_t QueueCapacity = 8U;
    static constexpr auto ShutdownDrainTimeout = std::chrono::seconds{5};

    // Move-only ownership of one future PostDelivery queue position. A live
    // reservation shares the executor's fixed queue capacity with pending
    // tasks. Validation failures preserve ownership; only a successful FIFO
    // enqueue consumes it. Destruction or release returns unused capacity.
    class Reservation final {
    public:
        Reservation() noexcept = default;
        ~Reservation() noexcept;

        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept;

        [[nodiscard]] Domain::Result<void> trySubmit(
            std::unique_ptr<IDashboardHandlerOperation> operation,
            Domain::OperationContext context,
            std::weak_ptr<IDashboardHandlerCompletionSink> completionSink)
            && noexcept;

        void release() noexcept;

    private:
        friend class Impl;

        std::shared_ptr<Impl> implementation_;
        bool ownsCapacity_{};
    };

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<WindowsDashboardHandlerExecutor>>
    create() noexcept;

    ~WindowsDashboardHandlerExecutor() noexcept;

    WindowsDashboardHandlerExecutor(
        const WindowsDashboardHandlerExecutor&) = delete;
    WindowsDashboardHandlerExecutor& operator=(
        const WindowsDashboardHandlerExecutor&) = delete;
    WindowsDashboardHandlerExecutor(
        WindowsDashboardHandlerExecutor&&) = delete;
    WindowsDashboardHandlerExecutor& operator=(
        WindowsDashboardHandlerExecutor&&) = delete;

    [[nodiscard]] Domain::Result<void> trySubmit(
        std::unique_ptr<IDashboardHandlerOperation> operation,
        Domain::OperationContext context,
        std::weak_ptr<IDashboardHandlerCompletionSink> completionSink)
        noexcept;

    // Reserves one of the eight pending positions before transport delivery
    // begins. The resulting token can convert that position into exactly one
    // PostDelivery task without another capacity race.
    [[nodiscard]] Domain::Result<Reservation> tryReservePostDelivery()
        noexcept;

    [[nodiscard]] std::size_t pendingCount() const noexcept;
    [[nodiscard]] std::size_t reservationCount() const noexcept;
    [[nodiscard]] std::size_t activeCount() const noexcept;
    [[nodiscard]] bool isShuttingDown() const noexcept;
    [[nodiscard]] bool fullyDrained() const noexcept;

    // Binding is one-shot for the executor lifetime and must complete before
    // shutdown begins. Process composition must bind; standalone use may omit
    // observation. Process composition must retain a successfully bound
    // observer strongly through its exact notification; losing it is a
    // structural lifetime violation and fails closed at the ready edge.
    [[nodiscard]] Domain::Result<void> bindShutdownDrainObserver(
        std::weak_ptr<IDashboardHandlerExecutorDrainObserver> observer)
        noexcept;

    // beginShutdown is nonblocking and safe from a worker completion sink. It
    // rejects new tasks and reservations, invalidates live reservations for
    // submission, and requests cancellation for queued and active work. Live
    // tokens remain safe capacity owners until they release or destruct.
    void beginShutdown() noexcept;

    // shutdown cancellation-drains accepted tasks and joins every worker when
    // called externally. With process observation bound, a worker-context call
    // only begins shutdown and returns; the external process finalizer retains
    // exact authority to join all four handles and emit the drain edge. In
    // standalone use, a sink may synchronously release the outer executor; in
    // that reentrant case shutdown joins the other workers and detaches only
    // the current thread handle to avoid self-join. That worker retains shared
    // implementation ownership until it exits. A non-cooperative operation
    // that outlives the five-second hard contract fails fast.
    void shutdown() noexcept;

private:
    explicit WindowsDashboardHandlerExecutor(
        std::shared_ptr<Impl> implementation) noexcept;

    std::shared_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
