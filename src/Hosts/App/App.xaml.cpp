#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "ManagerConnection.h"
#include <shellapi.h>
#include <memory>
#include <optional>
#include <string>

namespace {

struct LocalArguments final {
    wchar_t** values{};
    ~LocalArguments() { if (values) ::LocalFree(values); }
};

[[nodiscard]] std::optional<std::wstring> commandLineAlphaRoot() noexcept
{
    int count = 0;
    LocalArguments arguments{
        ::CommandLineToArgvW(::GetCommandLineW(), &count)};
    if (!arguments.values || count < 2) return std::nullopt;
    for (int index = 1; index < count; ++index) {
        if (std::wstring_view{arguments.values[index]} == L"--alpha-root") {
            return index + 1 < count
                ? std::optional<std::wstring>{arguments.values[index + 1]}
                : std::optional<std::wstring>{L""};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::wstring selectedAlphaRoot() noexcept
{
    if (auto requested = commandLineAlphaRoot()) {
        return std::move(*requested);
    }
    auto persistent = ::ForgeConductor::Infrastructure::Windows::
        WindowsAlphaManagerProfile::persistentDataRoot();
    return persistent ? std::move(persistent).value() : std::wstring{};
}

}
namespace winrt::ForgeConductorApp::implementation {
App::App() {}
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
    window_ = winrt::make<MainWindow>(
        std::make_shared<::ForgeConductor::Hosts::App::ManagerConnection>(
            selectedAlphaRoot()));
    window_.Activate();
}
}
