#pragma once

#include "ForgeConductor/Domain/Result.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>

#include <memory>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Narrow native seam retained by the initialized runtime state. Production
// composition supplies DashboardWinsockSystemApi; tests may inject a bounded
// deterministic implementation without changing process-global Winsock state.
class IDashboardWinsockApi {
public:
    virtual ~IDashboardWinsockApi() = default;

    [[nodiscard]] virtual int startup(
        WORD requestedVersion,
        WSADATA& data) noexcept = 0;
    [[nodiscard]] virtual int cleanup() noexcept = 0;
    [[nodiscard]] virtual SOCKET createSocket(
        int addressFamily,
        int socketType,
        int protocol,
        DWORD flags) noexcept = 0;
    [[nodiscard]] virtual int closeSocket(SOCKET socket) noexcept = 0;
    [[nodiscard]] virtual int lastError() noexcept = 0;
};

class DashboardWinsockSystemApi final : public IDashboardWinsockApi {
public:
    [[nodiscard]] int startup(
        WORD requestedVersion,
        WSADATA& data) noexcept override;
    [[nodiscard]] int cleanup() noexcept override;
    [[nodiscard]] SOCKET createSocket(
        int addressFamily,
        int socketType,
        int protocol,
        DWORD flags) noexcept override;
    [[nodiscard]] int closeSocket(SOCKET socket) noexcept override;
    [[nodiscard]] int lastError() noexcept override;
};

class DashboardWinsockRuntimeState;

// Owns one socket created inside an initialized dashboard Winsock session.
// Each wrapper is confined to one serialized listener/connection owner. The
// retained session state guarantees that closesocket precedes WSACleanup even
// if the outer runtime facade is released first.
class UniqueDashboardSocket final {
public:
    ~UniqueDashboardSocket() noexcept;

    UniqueDashboardSocket(const UniqueDashboardSocket&) = delete;
    UniqueDashboardSocket& operator=(const UniqueDashboardSocket&) = delete;
    UniqueDashboardSocket(UniqueDashboardSocket&& other) noexcept;
    UniqueDashboardSocket& operator=(UniqueDashboardSocket&& other) noexcept;

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return socket_ != INVALID_SOCKET;
    }

    void reset() noexcept;

private:
    friend class DashboardWinsockRuntime;

    UniqueDashboardSocket(
        std::shared_ptr<DashboardWinsockRuntimeState> state,
        SOCKET socket) noexcept;

    std::shared_ptr<DashboardWinsockRuntimeState> state_;
    SOCKET socket_{INVALID_SOCKET};
};

// Process-composition owner for one balanced WSAStartup/WSACleanup reference.
// It is deliberately noncopyable and nonmovable. Its initialized state is
// retained by owned sockets so cleanup cannot race ahead of socket closure.
class DashboardWinsockRuntime final {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>
    create() noexcept;

    [[nodiscard]] static Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>
    create(std::shared_ptr<IDashboardWinsockApi> api) noexcept;

    ~DashboardWinsockRuntime() noexcept = default;

    DashboardWinsockRuntime(const DashboardWinsockRuntime&) = delete;
    DashboardWinsockRuntime& operator=(const DashboardWinsockRuntime&) = delete;
    DashboardWinsockRuntime(DashboardWinsockRuntime&&) = delete;
    DashboardWinsockRuntime& operator=(DashboardWinsockRuntime&&) = delete;

    [[nodiscard]] Domain::Result<UniqueDashboardSocket>
    createOverlappedTcpSocket(int addressFamily) noexcept;

    static constexpr WORD RequiredVersion = MAKEWORD(2, 2);
    static constexpr DWORD RequiredSocketFlags =
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT;

private:
    explicit DashboardWinsockRuntime(
        std::shared_ptr<DashboardWinsockRuntimeState> state) noexcept;

    std::shared_ptr<DashboardWinsockRuntimeState> state_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
