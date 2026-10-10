#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include <Windows.h>
#include <charconv>
#include <memory>
#include <string>
#include <string_view>

namespace ForgeConductor::NativeTools::Windows::Detail {

// Both managed Comfy paths use this lease for the whole provider operation.
// A Windows mutex permits nested backend operations on the owning worker.
class ProviderOperationLease final {
public:
    ~ProviderOperationLease() { if(handle_) { ::ReleaseMutex(handle_); ::CloseHandle(handle_); } }
    ProviderOperationLease(const ProviderOperationLease&) = delete;
    ProviderOperationLease& operator=(const ProviderOperationLease&) = delete;

    [[nodiscard]] static Domain::Result<std::unique_ptr<ProviderOperationLease>> acquire(
        std::string_view endpoint, const Domain::OperationContext& context) {
        using Lease = std::unique_ptr<ProviderOperationLease>;
        const auto separator = endpoint.rfind(':');
        unsigned port{};
        const auto encoded = separator == std::string_view::npos ? std::string_view{} : endpoint.substr(separator + 1U);
        if(encoded.empty()) return Domain::Result<Lease>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,"Provider lease requires an explicit loopback port."));
        const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), port);
        if(parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size() || !port || port > 65535U)
            return Domain::Result<Lease>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest,"Provider lease requires an explicit loopback port."));
        const auto name = L"Local\\ForgeConductor.ComfyProvider." + std::to_wstring(port);
        const auto handle = ::CreateMutexW(nullptr,FALSE,name.c_str());
        if(!handle) return Domain::Result<Lease>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"Cannot open the managed ComfyUI provider lease."));
        for(;;) {
            const auto valid = Infrastructure::Windows::Detail::validateOperationContext(context,
                std::chrono::steady_clock::now(),"waiting for the managed ComfyUI provider");
            if(!valid) { ::CloseHandle(handle); return Domain::Result<Lease>::failure(valid.error()); }
            const auto state = ::WaitForSingleObject(handle,50U);
            if(state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) {
                const auto acquired = Infrastructure::Windows::Detail::validateOperationContext(context,
                    std::chrono::steady_clock::now(),"acquiring the managed ComfyUI provider");
                if(!acquired) { ::ReleaseMutex(handle); ::CloseHandle(handle); return Domain::Result<Lease>::failure(acquired.error()); }
                return Domain::Result<Lease>::success(Lease{new ProviderOperationLease{handle}});
            }
            if(state != WAIT_TIMEOUT) { ::CloseHandle(handle); return Domain::Result<Lease>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure,"Cannot acquire the managed ComfyUI provider lease.")); }
        }
    }
private:
    explicit ProviderOperationLease(HANDLE handle):handle_(handle) {}
    HANDLE handle_{};
};
}
