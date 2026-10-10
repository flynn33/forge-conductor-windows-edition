#include "IWindowsDashboardUriLaunchPlatform.h"
#include "IWindowsManagerBootstrapPlatform.h"
#include "CommandLineBuilder.h"
#include "UtfConversion.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAlphaManagerProfile.h"

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

#include <array>
#include <filesystem>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] Domain::Result<void> resolveDesktopShell(ComReference<IShellDispatch2>& shell)
{
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

    auto shellInterface = queryInterface(
        *applicationDispatch.get(), shell,
        "obtain the registered-URI Shell dispatcher");
    if (!shellInterface) {
        return shellInterface;
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
            ComReference<IShellDispatch2> shell;
            auto resolved = resolveDesktopShell(shell);
            if (!resolved) return resolved;

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

class WindowsManagerBootstrapPlatform final : public IWindowsManagerBootstrapPlatform {
public:
    [[nodiscard]] Domain::Result<void> start(
        const Domain::PathText& home, const bool isolatedProfile,
        const ManagerBootstrapLaunchMode mode) noexcept override
    {
        try {
            auto wideHome = strictUtf8ToUtf16(home.value());
            if (!wideHome) return Domain::Result<void>::failure(std::move(wideHome).error());
            const auto attributes = ::GetFileAttributesW(wideHome.value().c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest, "The Manager bootstrap requires an existing regular home directory."));
            auto profile = WindowsAlphaManagerProfile::create(home);
            auto persistent = WindowsAlphaManagerProfile::persistentDataRoot();
            if (!profile) return Domain::Result<void>::failure(std::move(profile).error());
            if (!persistent) return Domain::Result<void>::failure(std::move(persistent).error());
            const auto equalPath = [](const std::wstring_view left, const std::wstring_view right) {
                return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                    right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
            };
            if (!equalPath(wideHome.value(), profile.value().nativeDataRoot()) ||
                isolatedProfile == equalPath(profile.value().nativeDataRoot(), persistent.value()))
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict, "The Manager bootstrap profile mode or canonical home does not match."));
            std::array<wchar_t, 32'768U> module{};
            const auto length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            if (length == 0U || length >= module.size()) return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, "The Manager bootstrap could not resolve its executable."));
            const auto cliExecutable = std::filesystem::path{std::wstring{module.data(), length}};
            const auto executable = cliExecutable.parent_path() /
                L"ForgeConductor.Manager.exe";
            const HANDLE file = ::CreateFileW(executable.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, "The regular Manager sibling is unavailable."));
            struct FileCloser final { HANDLE value; ~FileCloser() { static_cast<void>(::CloseHandle(value)); } } close{file};
            FILE_ATTRIBUTE_TAG_INFO tag{};
            FILE_STANDARD_INFO standard{};
            if (!::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                !::GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
                standard.DeletePending || standard.NumberOfLinks != 1U)
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure, "The Manager sibling is not a regular single-link executable."));
            const std::vector<std::string> managerArguments{
                isolatedProfile ? "--alpha-root" : "--home", profile.value().dataRoot().value()};
            if (mode == ManagerBootstrapLaunchMode::DetachedManager) {
                auto command = CommandLineBuilder::buildCommandLine(executable.native(), managerArguments);
                if (!command) return Domain::Result<void>::failure(std::move(command).error());
                STARTUPINFOW startup{}; startup.cb = sizeof(startup);
                PROCESS_INFORMATION process{};
                if (!::CreateProcessW(executable.c_str(), command.value().data(), nullptr, nullptr,
                    FALSE, CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process))
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::ProcessLaunchFailed, "The detached CLI could not create its matching Manager.", true));
                static_cast<void>(::CloseHandle(process.hThread));
                static_cast<void>(::CloseHandle(process.hProcess));
                return Domain::Result<void>::success();
            }
            auto arguments = CommandLineBuilder::buildArgumentString(
                {"--internal-launch-manager", managerArguments[0], managerArguments[1]});
            if (!arguments) return Domain::Result<void>::failure(std::move(arguments).error());
            ComApartment apartment;
            auto initialized = apartment.initialize();
            if (!initialized) return initialized;
            ComReference<IShellDispatch2> shell;
            auto resolved = resolveDesktopShell(shell);
            if (!resolved) return resolved;
            const UniqueBstr image{cliExecutable.native()};
            const UniqueBstr params{arguments.value()};
            const UniqueBstr directory{executable.parent_path().native()};
            const UniqueBstr verb{L"open"};
            if (!image || !params || !directory || !verb) return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InternalFailure, "The Manager bootstrap could not allocate Shell arguments."));
            VARIANT args{}, cwd{}, operation{}, show{};
            args.vt = cwd.vt = operation.vt = VT_BSTR;
            args.bstrVal = params.get(); cwd.bstrVal = directory.get(); operation.bstrVal = verb.get();
            show.vt = VT_I4; show.lVal = SW_HIDE;
            // The registered CLI target dispatches through Explorer outside
            // the supervised helper's job, then creates its Manager sibling.
            const auto started = shell->ShellExecute(image.get(), args, cwd, operation, show);
            if (FAILED(started)) return Domain::Result<void>::failure(shellFailure("start its matching Manager launcher", started));
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure, "The Manager desktop bootstrap failed safely."));
        }
    }
};

} // namespace

std::unique_ptr<IWindowsDashboardUriLaunchPlatform>
createWindowsDashboardUriLaunchPlatform()
{
    return std::make_unique<WindowsDashboardUriLaunchPlatform>();
}

std::unique_ptr<IWindowsManagerBootstrapPlatform> createWindowsManagerBootstrapPlatform()
{
    return std::make_unique<WindowsManagerBootstrapPlatform>();
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
