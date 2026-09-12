#pragma once
#include "MainWindow.g.h"
#include "ManagerConnection.h"

#include <memory>
#include <optional>

namespace winrt::ForgeConductorApp::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    explicit MainWindow(std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection);
    void RefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void StartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void StopClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RestartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void NavigationChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void ProviderLoadClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderSaveClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderTestClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStatusClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunPauseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunResumeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunCancelClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    enum class Action {
        Refresh, Start, Stop, Restart, ProviderLoad, ProviderSave, ProviderTest,
        RunStart, RunStatus, RunPause, RunResume, RunCancel
    };
    winrt::fire_and_forget RunAction(Action action);
    [[nodiscard]] std::optional<::ForgeConductor::Domain::ManagerSettings>
        ReadProviderForm(std::string& error);
    void ApplyProviderForm(const ::ForgeConductor::Domain::ManagerSettings& settings);

    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection_;
    std::optional<::ForgeConductor::Domain::ManagerSettings> providerSettings_;
    std::optional<::ForgeConductor::Domain::ManagerTelemetrySnapshot>
        telemetrySnapshot_;
    std::stop_source cancellation_;
    bool busy_{};
};
}

namespace winrt::ForgeConductorApp::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
