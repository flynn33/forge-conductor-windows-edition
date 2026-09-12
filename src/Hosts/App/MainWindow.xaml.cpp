#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <cmath>
#include <limits>
#include <winrt/Microsoft.UI.Dispatching.h>

namespace winrt::ForgeConductorApp::implementation {
namespace {
using Visibility = Microsoft::UI::Xaml::Visibility;

[[nodiscard]] std::uint32_t numberValue(
    const Microsoft::UI::Xaml::Controls::NumberBox& control,
    const std::string_view name)
{
    const auto value = control.Value();
    if (!std::isfinite(value) || value < 0.0 ||
        value > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) ||
        std::floor(value) != value) {
        throw std::invalid_argument{std::string{name} + " must be a whole number."};
    }
    return static_cast<std::uint32_t>(value);
}
}

MainWindow::MainWindow() {}

MainWindow::MainWindow(
    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection)
    : connection_{std::move(connection)}
{
    Closed([this](auto const&, auto const&) { cancellation_.request_stop(); });
}

void MainWindow::RefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::Refresh); }
void MainWindow::StartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::Start); }
void MainWindow::StopClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::Stop); }
void MainWindow::RestartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::Restart); }
void MainWindow::ProviderLoadClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderLoad); }
void MainWindow::ProviderSaveClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderSave); }
void MainWindow::ProviderTestClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderTest); }

void MainWindow::NavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
{
    const auto item = args.SelectedItem().try_as<
        Microsoft::UI::Xaml::Controls::NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"Rig");
    PageTitle().Text(tag);
    const bool provider = tag == L"Provider";
    const bool rig = tag == L"Rig" || tag == L"Manager" ||
        tag == L"Diagnostics" || tag == L"Runtimes";
    ProviderPanel().Visibility(provider ? Visibility::Visible : Visibility::Collapsed);
    RigPanel().Visibility(rig ? Visibility::Visible : Visibility::Collapsed);
    GenericPanel().Visibility(!provider && !rig ? Visibility::Visible : Visibility::Collapsed);
    if (provider) {
        PageDescription().Text(L"Configure and test the Manager-owned LM Studio Responses endpoint.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else {
        PageDescription().Text(L"Read the live Manager connection while this native surface is being completed.");
    }
}

std::optional<::ForgeConductor::Domain::ManagerSettings>
MainWindow::ReadProviderForm(std::string& error)
{
    try {
        auto settings = providerSettings_.value_or(
            ::ForgeConductor::Domain::ManagerSettings{});
        settings.localModelHost = winrt::to_string(ProviderHost().Text());
        const auto port = numberValue(ProviderPort(), "Port");
        if (port == 0U || port > 65'535U) {
            throw std::invalid_argument{"Port must be within 1 through 65535."};
        }
        settings.localModelPort = static_cast<std::uint16_t>(port);
        settings.localModelSecure = ProviderSecure().IsOn();
        settings.localModelName = winrt::to_string(ProviderModel().Text());
        settings.effectiveContextCapacity = numberValue(ContextCapacity(), "Context capacity");
        settings.nextResponseReserve = numberValue(ResponseReserve(), "Next response reserve");
        settings.handoffReserve = numberValue(HandoffReserve(), "Handoff reserve");
        settings.estimationSafetyMargin = numberValue(SafetyMargin(), "Safety margin");
        auto valid = ::ForgeConductor::Domain::validateManagerSettings(settings);
        if (!valid) throw std::invalid_argument{valid.error().message};
        return settings;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

void MainWindow::ApplyProviderForm(
    const ::ForgeConductor::Domain::ManagerSettings& settings)
{
    ProviderHost().Text(winrt::to_hstring(settings.localModelHost));
    ProviderPort().Value(settings.localModelPort);
    ProviderSecure().IsOn(settings.localModelSecure);
    ProviderModel().Text(winrt::to_hstring(settings.localModelName));
    ContextCapacity().Value(settings.effectiveContextCapacity);
    ResponseReserve().Value(settings.nextResponseReserve);
    HandoffReserve().Value(settings.handoffReserve);
    SafetyMargin().Value(settings.estimationSafetyMargin);
}

winrt::fire_and_forget MainWindow::RunAction(const Action action)
{
    auto lifetime = get_strong();
    if (busy_ || !connection_) co_return;

    std::optional<::ForgeConductor::Domain::ManagerSettings> submitted;
    if (action == Action::ProviderSave || action == Action::ProviderTest) {
        std::string error;
        submitted = ReadProviderForm(error);
        if (!submitted) {
            ProviderState().Text(winrt::to_hstring("Check provider settings: " + error));
            co_return;
        }
    }

    busy_ = true;
    winrt::apartment_context ui;
    if (action == Action::ProviderLoad || action == Action::ProviderSave ||
        action == Action::ProviderTest) {
        ProviderState().Text(L"Working…");
    } else {
        ManagerState().Text(L"Connecting…");
        GenericState().Text(L"Connecting…");
    }

    std::string message;
    ::ForgeConductor::Hosts::App::ProviderSettingsView loaded;
    bool failed{};
    try {
        co_await winrt::resume_background();
        switch (action) {
        case Action::Start:
            message = connection_->start(cancellation_.get_token());
            break;
        case Action::Refresh:
            message = connection_->refresh(cancellation_.get_token());
            break;
        case Action::Stop:
            message = connection_->control(
                ::ForgeConductor::Domain::ManagerControlAction::Stop,
                cancellation_.get_token());
            break;
        case Action::Restart:
            message = connection_->control(
                ::ForgeConductor::Domain::ManagerControlAction::Restart,
                cancellation_.get_token());
            break;
        case Action::ProviderLoad:
            loaded = connection_->providerSettings(cancellation_.get_token());
            message = loaded.message;
            break;
        case Action::ProviderSave: {
            ::ForgeConductor::Domain::ManagerSettingsPatch patch;
            patch.localModelHost = submitted->localModelHost;
            patch.localModelPort = submitted->localModelPort;
            patch.localModelSecure = submitted->localModelSecure;
            patch.localModelName = submitted->localModelName;
            patch.effectiveContextCapacity = submitted->effectiveContextCapacity;
            patch.nextResponseReserve = submitted->nextResponseReserve;
            patch.handoffReserve = submitted->handoffReserve;
            patch.estimationSafetyMargin = submitted->estimationSafetyMargin;
            message = connection_->saveProviderSettings(
                patch, cancellation_.get_token());
            break;
        }
        case Action::ProviderTest:
            message = connection_->testProvider(
                *submitted, cancellation_.get_token());
            break;
        }
    } catch (const std::exception& exception) {
        message = exception.what();
        failed = true;
    } catch (...) {
        message = "The operation failed before completion.";
        failed = true;
    }

    try {
        co_await ui;
    } catch (...) {
        co_return;
    }
    if (!cancellation_.stop_requested()) {
        if (action == Action::ProviderLoad || action == Action::ProviderSave ||
            action == Action::ProviderTest) {
            if (action == Action::ProviderLoad && loaded.loaded) {
                providerSettings_ = loaded.settings;
                ApplyProviderForm(*providerSettings_);
            } else if (action == Action::ProviderSave && submitted && !failed) {
                providerSettings_ = submitted;
            }
            ProviderState().Text(winrt::to_hstring(message));
        } else {
            ManagerState().Text(winrt::to_hstring(message));
            GenericState().Text(winrt::to_hstring(message));
        }
    }
    busy_ = false;
}
}
