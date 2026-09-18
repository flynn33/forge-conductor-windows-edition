#pragma once
#include "MainWindow.g.h"
#include "AppActionScheduler.h"
#include "ManagerConnection.h"

#include <cstdint>
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
    void ConsoleSizeChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::SizeChangedEventArgs const&);
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
    void ProviderDiscoverClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderContractClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderContractCancelClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProviderModelSelectionChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
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
    void OpenEvidenceClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenSettingsClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenToolsClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenProjectsClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void GuidedModeToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void GuidedModePrimaryClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void GuidedModeSecondaryClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void GuidedModeBackClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void GuidedModeCloseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStartClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunStatusClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunHistoryRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunHistoryAttachClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunPauseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunResumeClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunCancelClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RunIdTextChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void ProjectRegisterClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectBrowseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectSearchClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectRememberClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void InstructionPackageBrowseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void InstructionPackagePathChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void InstructionPackagePreviewClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void InstructionPackageActivateClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectUpdateClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectForgetClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectEditCloseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectArchiveExportClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectArchiveBrowseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectArchivePathChanged(Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void ProjectArchivePreviewClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void ProjectArchiveImportClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
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
    void OperationalExportClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalPruneClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void RuntimeJobInspectClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalCloseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OperationalSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalCardSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalAgentSearchChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void OperationalFeedFilterChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void OperationalFeedSeverityChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalFeedCategoryChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalFeedProjectChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OperationalFeedPauseClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EvidenceRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EvidenceVerifyClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EvidenceRunInspectClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void EvidenceExportClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    enum class GuidedProjectStep : std::uint32_t {
        Welcome = 1U,
        ChooseFolder = 2U,
        RegisterProject = 3U,
        UnderstandProject = 4U,
        Ready = 5U
    };
    enum class Action {
        Refresh, Start, Stop, Restart, ProviderLoad, ProviderSave, ProviderTest,
        ProviderModels, ProviderContract,
        SettingsLoad, SettingsSave, SettingsTest, SettingsRestart, MaintenanceReset,
        RunStart, RunStatus, RunPause, RunResume, RunCancel,
        ProjectList, ProjectRegister, ProjectLoad, ProjectRemember, ProjectUpdate, ProjectForget,
        ProjectArchiveExport, ProjectArchivePreview, ProjectArchiveImport,
        InstructionPackagePreview, InstructionPackageActivate,
        LmStudioInspect, LmStudioRepair, LmStudioActivate, ToolsList, ToolInvoke,
        OperationalInspect, OperationalPrune, OperationalClose, RunHistory, EvidenceLoad, EvidenceVerify
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
    void ApplyRunReadback(const ::ForgeConductor::Domain::ManagedRunSnapshot& snapshot);
    void ApplyDisconnectedTelemetry(std::string_view reason);
    void ApplyProjectList(
        const ::ForgeConductor::Manager::ManagerProjectsSnapshot& snapshot);
    void ApplyProjectWorkspace(
        const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot);
    void SelectMemoryRecord(
        const ::ForgeConductor::Manager::ManagerProjectMemoryRecord& record);
    void ApplyLmStudio(const ::ForgeConductor::Manager::ManagerLmStudioSnapshot& snapshot);
    void ApplyLmStudioIdentities();
    void ApplyTools(const ::ForgeConductor::Manager::ManagerToolsSnapshot& snapshot);
    void FilterTools();
    void BuildToolForm(const ::ForgeConductor::Manager::ManagerToolDescriptor& tool);
    std::optional<std::string> ToolCanonicalArguments();
    void RenderToolOutcome(
        const ::ForgeConductor::Manager::ManagerToolOutcomeSnapshot& snapshot,
        std::string_view message);
    void ApplyOperational(const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot);
    void ApplyRunHistory(const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot);
    void ApplyEvidence(const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot);
    void SelectEvidenceRun(std::string_view runId);
    void SelectOperationalRecord(std::size_t index);
    void UpdateRunProjectLabel();
    void ClearSelectedRun();
    void ClearSelectedProject();
    void ClearArchivePreview();
    void ClearInstructionPackagePreview();
    void SelectPage(const winrt::hstring& tag);
    void SetGuidedMode(bool enabled, bool restart);
    void SetGuidedStep(GuidedProjectStep step);
    void RenderGuidedMode();
    void ShowProjectRegistration();
    [[nodiscard]] bool BrowseForProjectFolder();

    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection_;
    std::optional<::ForgeConductor::Domain::ManagerSettings> providerSettings_;
    std::optional<::ForgeConductor::Domain::ManagerTelemetrySnapshot>
        telemetrySnapshot_;
    std::optional<::ForgeConductor::Manager::ManagerLmStudioSnapshot>
        lmStudioSnapshot_;
    std::string activityTimelineKey_;
    std::vector<::ForgeConductor::Domain::ProjectMemoryDescriptor> projects_;
    std::optional<::ForgeConductor::Manager::ManagerProjectMemoryRecord> selectedMemoryRecord_;
    std::string selectedMemoryProjectId_;
    std::vector<::ForgeConductor::Manager::ManagerToolDescriptor> tools_;
    struct ToolField final {
        std::string name;
        std::string type;
        bool required{};
        Microsoft::UI::Xaml::Controls::TextBox text{nullptr};
        Microsoft::UI::Xaml::Controls::ComboBox choice{nullptr};
        Microsoft::UI::Xaml::Controls::ToggleSwitch toggle{nullptr};
    };
    std::vector<ToolField> toolFields_;
    bool toolFormSupported_{};
    std::vector<std::string> loadedModels_;
    bool providerDiscoveryAttempted_{};
    bool providerDiscoverySucceeded_{};
    bool rebuildingProviderModels_{};
    std::string providerDiscoveredEndpoint_;
    std::vector<std::size_t> visibleTools_;
    std::vector<std::string> operationalLines_;
    std::vector<std::size_t> visibleOperationalIndices_;
    std::optional<::ForgeConductor::Manager::ManagerOperationalSnapshot> operationalSnapshot_;
    std::optional<::ForgeConductor::Manager::ManagerOperationalSnapshot> evidenceSnapshot_;
    std::string selectedEvidenceRunId_;
    std::string selectedEvidenceProjectId_;
    std::optional<::ForgeConductor::Manager::ManagerOperationalSnapshot> pendingFeedSnapshot_;
    bool feedDisplayPaused_{};
    bool rebuildingTools_{};
    std::string selectedProjectId_;
    std::string archivePreviewProjectId_;
    std::string archivePreviewPath_;
    std::string archivePreviewChecksum_;
    std::string instructionPreviewProjectId_;
    std::string instructionPreviewPath_;
    std::string instructionPreviewRevision_;
    std::string verifiedRunId_;
    std::string verifiedRunProjectId_;
    std::wstring selectedPageValueName_{L"SelectedPage"};
    std::wstring selectedProjectValueName_{L"SelectedProjectId"};
    std::wstring selectedRunValueName_{L"SelectedRunId"};
    std::wstring selectedRunProjectValueName_{L"SelectedRunProjectId"};
    std::wstring selectedEvidenceRunValueName_{L"SelectedEvidenceRunId"};
    std::wstring selectedEvidenceProjectValueName_{L"SelectedEvidenceProjectId"};
    std::wstring guidedModeValueName_{L"GuidedModeEnabled"};
    std::wstring guidedStepValueName_{L"GuidedModeStep"};
    std::stop_source cancellation_;
    std::stop_source providerContractCancellation_;
    Microsoft::UI::Xaml::DispatcherTimer telemetryTimer_{nullptr};
    bool telemetryUiInitialized_{};
    bool rebuildingProjects_{};
    bool guidedModeEnabled_{true};
    bool updatingGuidedModeControls_{};
    bool guidedRegistrationPending_{};
    GuidedProjectStep guidedStep_{GuidedProjectStep::Welcome};
    ::ForgeConductor::Manager::ManagerOperationalArea operationalArea_{
        ::ForgeConductor::Manager::ManagerOperationalArea::Agents};
    ::ForgeConductor::Hosts::App::AppActionScheduler actionScheduler_;
};
}

namespace winrt::ForgeConductorApp::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
