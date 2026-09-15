#pragma once
#include "MainWindow.g.h"
#include "AppActionScheduler.h"
#include "ManagerConnection.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace winrt::ForgeConductorApp::implementation {
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    explicit MainWindow(std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection);
    void WindowClosed(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::WindowEventArgs const&);
    void WindowContentLoaded(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void StartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void StopClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RestartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void NavigationChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void TelemetryChartSizeChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void TelemetryGaugeSizeChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void ProviderLoadClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderSaveClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderTestClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsLoadClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsSaveClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsRevertClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsTestClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void SettingsRestartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void MaintenanceResetClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenProviderClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenManagerClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenAutonomyClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenFeedClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenSettingsClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStatusClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunPauseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunResumeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunCancelClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectRegisterClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectSearchClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectRememberClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectSelectionChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void LmStudioInspectClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void LmStudioRepairClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void LmStudioActivateClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToolsRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToolInvokeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ToolFilterChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void ToolPackFilterChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void ToolSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalPruneClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalCloseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalCardSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

private:
    enum class Action {
        Refresh, Start, Stop, Restart, ProviderLoad, ProviderSave, ProviderTest,
        SettingsLoad, SettingsSave, SettingsTest, SettingsRestart, MaintenanceReset,
        RunStart, RunStatus, RunPause, RunResume, RunCancel,
        ProjectList, ProjectRegister, ProjectLoad, ProjectRemember,
        LmStudioInspect, LmStudioRepair, LmStudioActivate, ToolsList, ToolInvoke,
        OperationalInspect, OperationalPrune, OperationalClose
    };
    winrt::fire_and_forget RunAction(Action action);
    [[nodiscard]] std::optional<::ForgeConductor::Domain::ManagerSettings>
        ReadProviderForm(std::string& error);
    [[nodiscard]] std::optional<::ForgeConductor::Domain::ManagerSettings>
        ReadSettingsForm(std::string& error);
    void ApplyProviderForm(const ::ForgeConductor::Domain::ManagerSettings& settings);
    void ApplySettingsForm(const ::ForgeConductor::Domain::ManagerSettings& settings);
    void ApplyTelemetryPresentation(
        const ::ForgeConductor::Domain::ManagerTelemetrySnapshot& snapshot);
    void ApplyDisconnectedTelemetry(std::string_view reason);
    void ApplyProjectList(
        const ::ForgeConductor::Manager::ManagerProjectsSnapshot& snapshot);
    void ApplyProjectWorkspace(
        const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot);
    void ApplyLmStudio(const ::ForgeConductor::Manager::ManagerLmStudioSnapshot& snapshot);
    void ApplyTools(const ::ForgeConductor::Manager::ManagerToolsSnapshot& snapshot);
    void FilterTools();
    void ApplyOperational(const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot);
    void SelectOperationalRecord(std::size_t index);
    void ClearSelectedProject();
    void SelectPage(const winrt::hstring& tag);

    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection_;
    std::optional<::ForgeConductor::Domain::ManagerSettings> providerSettings_;
    std::optional<::ForgeConductor::Domain::ManagerTelemetrySnapshot>
        telemetrySnapshot_;
    std::vector<::ForgeConductor::Domain::ProjectMemoryDescriptor> projects_;
    std::vector<::ForgeConductor::Manager::ManagerToolDescriptor> tools_;
    std::vector<std::size_t> visibleTools_;
    std::vector<std::string> operationalLines_;
    bool rebuildingTools_{};
    std::string selectedProjectId_;
    std::wstring selectedPageValueName_{L"SelectedPage"};
    std::wstring selectedProjectValueName_{L"SelectedProjectId"};
    std::stop_source cancellation_;
    Microsoft::UI::Xaml::DispatcherTimer telemetryTimer_{nullptr};
    bool telemetryUiInitialized_{};
    bool rebuildingProjects_{};
    ::ForgeConductor::Manager::ManagerOperationalArea operationalArea_{
        ::ForgeConductor::Manager::ManagerOperationalArea::Agents};
    ::ForgeConductor::Hosts::App::AppActionScheduler actionScheduler_;
};
}

namespace winrt::ForgeConductorApp::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
