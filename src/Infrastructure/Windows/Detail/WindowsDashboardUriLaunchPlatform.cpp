#include "IWindowsDashboardUriLaunchPlatform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <servprov.h>
#include <ExDisp.h>
#include <ShlDisp.h>
#include <ShlObj.h>

#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error shellFailure(
    const std::string_view action,
    const HRESULT result,
    const bool retryable = true)
{
    return Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "The Windows Shell could not " + std::string{action} +
            " (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ").",
        retryable);
}

class ComApartment final {
public:
    ComApartment() noexcept = default;
    ~ComApartment() noexcept
    {
        if (initialized_) {
            ::CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] Domain::Result<void> initialize()
    {
        const HRESULT result = ::CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(result)) {
            return Domain::Result<void>::failure(
                shellFailure("initialize the dashboard activation STA",
                             result));
        }
        initialized_ = true;
        return Domain::Result<void>::success();
    }

private:
    bool initialized_{};
};

template <typename Interface>
class ComReference final {
public:
    ComReference() noexcept = default;
    ~ComReference() noexcept { reset(); }

    ComReference(const ComReference&) = delete;
    ComReference& operator=(const ComReference&) = delete;

    [[nodiscard]] Interface* get() const noexcept { return value_; }
    [[nodiscard]] Interface** put() noexcept
    {
        reset();
        return &value_;
    }
    [[nodiscard]] Interface* operator->() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

private:
    void reset() noexcept
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

    Interface* value_{};
};

class UniqueBstr final {
public:
    explicit UniqueBstr(const std::wstring_view value) noexcept
        : value_{value.size() <=
                         static_cast<std::size_t>(
                             (std::numeric_limits<UINT>::max)())
                     ? ::SysAllocStringLen(
                           value.data(), static_cast<UINT>(value.size()))
                     : nullptr}
    {
    }

    ~UniqueBstr() noexcept { ::SysFreeString(value_); }

    UniqueBstr(const UniqueBstr&) = delete;
    UniqueBstr& operator=(const UniqueBstr&) = delete;

    [[nodiscard]] BSTR get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return value_ != nullptr;
    }

private:
    BSTR value_{};
};

template <typename Target, typename Source>
[[nodiscard]] Domain::Result<void> queryInterface(
    Source& source,
    ComReference<Target>& target,
    const std::string_view action)
{
    const HRESULT queried = source.QueryInterface(
        __uuidof(Target), reinterpret_cast<void**>(target.put()));
    if (FAILED(queried) || !target) {
        return Domain::Result<void>::failure(shellFailure(action, queried));
    }
    return Domain::Result<void>::success();
}

class WindowsDashboardUriLaunchPlatform final
    : public IWindowsDashboardUriLaunchPlatform {
public:
    [[nodiscard]] Domain::Result<void> open(
        const std::wstring_view uri) noexcept override
    {
        try {
            ComApartment apartment;
            auto initialized = apartment.initialize();
            if (!initialized) {
                return Domain::Result<void>::failure(
                    std::move(initialized).error());
            }

            // CLSID_Shell itself is an in-process shell32 class. Using it (or
            // ShellExecuteExW) inside the helper could place a newly created
            // browser in the helper's kill-on-close job. CLSID_ShellWindows is
            // explicitly requested as a local COM server; the desktop Shell
            // therefore owns the ShellExecute call and any associated browser.
            ComReference<IShellWindows> shellWindows;
            const HRESULT created = ::CoCreateInstance(
                __uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER,
                __uuidof(IShellWindows),
                reinterpret_cast<void**>(shellWindows.put()));
            if (FAILED(created) || !shellWindows) {
                return Domain::Result<void>::failure(shellFailure(
                    "connect to its local desktop server", created));
            }

            VARIANT location{};
            VARIANT locationRoot{};
            ::VariantInit(&location);
            ::VariantInit(&locationRoot);
            location.vt = VT_I4;
            location.lVal = CSIDL_DESKTOP;
            long desktopWindow{};
            ComReference<IDispatch> desktopDispatch;
            const HRESULT found = shellWindows->FindWindowSW(
                &location, &locationRoot, SWC_DESKTOP, &desktopWindow,
                SWFO_NEEDDISPATCH, desktopDispatch.put());
            if (found != S_OK || !desktopDispatch) {
                return Domain::Result<void>::failure(shellFailure(
                    "resolve the interactive desktop", found));
            }

            ComReference<IServiceProvider> desktopServices;
            auto serviceInterface = queryInterface(
                *desktopDispatch.get(), desktopServices,
                "obtain the desktop Shell service provider");
            if (!serviceInterface) {
                return serviceInterface;
            }

            ComReference<IShellBrowser> desktopBrowser;
            const HRESULT browserService = desktopServices->QueryService(
                SID_STopLevelBrowser, __uuidof(IShellBrowser),
                reinterpret_cast<void**>(desktopBrowser.put()));
            if (FAILED(browserService) || !desktopBrowser) {
                return Domain::Result<void>::failure(shellFailure(
                    "obtain the top-level desktop browser service",
                    browserService));
            }

            ComReference<IShellView> shellView;
            const HRESULT activeView =
                desktopBrowser->QueryActiveShellView(shellView.put());
            if (FAILED(activeView) || !shellView) {
                return Domain::Result<void>::failure(shellFailure(
                    "obtain the active desktop Shell view", activeView));
            }

            ComReference<IDispatch> backgroundDispatch;
            const HRESULT background = shellView->GetItemObject(
                SVGIO_BACKGROUND, __uuidof(IDispatch),
                reinterpret_cast<void**>(backgroundDispatch.put()));
            if (FAILED(background) || !backgroundDispatch) {
                return Domain::Result<void>::failure(shellFailure(
                    "obtain the desktop Shell background", background));
            }

            ComReference<IShellFolderViewDual> desktopView;
            auto viewInterface = queryInterface(
                *backgroundDispatch.get(), desktopView,
                "obtain the desktop Shell view");
            if (!viewInterface) {
                return viewInterface;
            }

            ComReference<IDispatch> applicationDispatch;
            const HRESULT application =
                desktopView->get_Application(applicationDispatch.put());
            if (FAILED(application) || !applicationDispatch) {
                return Domain::Result<void>::failure(shellFailure(
                    "obtain the desktop Shell application", application));
            }

            ComReference<IShellDispatch2> shell;
            auto shellInterface = queryInterface(
                *applicationDispatch.get(), shell,
                "obtain the registered-URI Shell dispatcher");
            if (!shellInterface) {
                return shellInterface;
            }

            const UniqueBstr file{uri};
            const UniqueBstr verb{L"open"};
            if (!file || !verb) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard activation helper could not allocate its bounded Shell arguments."));
            }
            VARIANT emptyArguments{};
            VARIANT emptyDirectory{};
            VARIANT operation{};
            VARIANT show{};
            ::VariantInit(&emptyArguments);
            ::VariantInit(&emptyDirectory);
            ::VariantInit(&operation);
            ::VariantInit(&show);
            operation.vt = VT_BSTR;
            operation.bstrVal = verb.get();
            show.vt = VT_I4;
            show.lVal = SW_SHOWNORMAL;

            // IShellDispatch2 has verb/show parameters instead of the
            // in-process ShellExecuteEx fMask. The call executes through the
            // local-server proxy and is bounded externally by the helper job.
            const HRESULT opened = shell->ShellExecute(
                file.get(), emptyArguments, emptyDirectory, operation, show);
            if (FAILED(opened)) {
                return Domain::Result<void>::failure(shellFailure(
                    "open the registered dashboard URI", opened));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The registered browser activation failed at the Windows boundary."));
        }
    }
};

} // namespace

std::unique_ptr<IWindowsDashboardUriLaunchPlatform>
createWindowsDashboardUriLaunchPlatform()
{
    return std::make_unique<WindowsDashboardUriLaunchPlatform>();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
