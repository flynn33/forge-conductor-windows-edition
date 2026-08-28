#pragma once

#include "DashboardLoopbackEndpoint.h"
#include "DashboardWinsockRuntime.h"

#include "ForgeConductor/Domain/Result.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow configuration seam for a socket that has already been created and
// remains owned by UniqueDashboardSocket. Production uses the system adapter;
// tests inject a deterministic implementation without binding a real port.
class IDashboardListeningSocketApi {
public:
    virtual ~IDashboardListeningSocketApi() = default;

    [[nodiscard]] virtual int setSocketOption(
        SOCKET socket,
        int level,
        int optionName,
        const char* optionValue,
        int optionLength) noexcept = 0;
    [[nodiscard]] virtual int bindSocket(
        SOCKET socket,
        const sockaddr* address,
        int addressLength) noexcept = 0;
    [[nodiscard]] virtual int getSocketName(
        SOCKET socket,
        sockaddr* address,
        int& addressLength) noexcept = 0;
    [[nodiscard]] virtual int listenSocket(
        SOCKET socket,
        int backlog) noexcept = 0;
    [[nodiscard]] virtual int lastError() noexcept = 0;
};

class DashboardListeningSocketSystemApi final
    : public IDashboardListeningSocketApi {
public:
    [[nodiscard]] int setSocketOption(
        SOCKET socket,
        int level,
        int optionName,
        const char* optionValue,
        int optionLength) noexcept override;
    [[nodiscard]] int bindSocket(
        SOCKET socket,
        const sockaddr* address,
        int addressLength) noexcept override;
    [[nodiscard]] int getSocketName(
        SOCKET socket,
        sockaddr* address,
        int& addressLength) noexcept override;
    [[nodiscard]] int listenSocket(
        SOCKET socket,
        int backlog) noexcept override;
    [[nodiscard]] int lastError() noexcept override;
};

// Fully configured loopback listener. The native handle never leaves typed
// ownership; callers receive only a borrowed value for IOCP association and
// AcceptEx submission. The endpoint has no mutation surface after creation.
class DashboardListeningSocket final {
public:
    static constexpr int ListenBacklog = 40;

    [[nodiscard]] static Domain::Result<DashboardListeningSocket> create(
        DashboardWinsockRuntime& runtime,
        const DashboardLoopbackEndpoint& endpoint) noexcept;

    [[nodiscard]] static Domain::Result<DashboardListeningSocket> create(
        DashboardWinsockRuntime& runtime,
        const DashboardLoopbackEndpoint& endpoint,
        std::shared_ptr<IDashboardListeningSocketApi> api) noexcept;

    DashboardListeningSocket(const DashboardListeningSocket&) = delete;
    DashboardListeningSocket& operator=(const DashboardListeningSocket&) =
        delete;
    DashboardListeningSocket(DashboardListeningSocket&& other) noexcept;
    DashboardListeningSocket& operator=(DashboardListeningSocket&&) = delete;
    ~DashboardListeningSocket() noexcept = default;

    [[nodiscard]] const DashboardLoopbackEndpoint& endpoint() const noexcept
    {
        return endpoint_;
    }

    [[nodiscard]] SOCKET borrowedNativeSocket() const noexcept
    {
        return socket_.get();
    }

private:
    DashboardListeningSocket(
        UniqueDashboardSocket socket,
        const DashboardLoopbackEndpoint& endpoint) noexcept;

    UniqueDashboardSocket socket_;
    const DashboardLoopbackEndpoint endpoint_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
