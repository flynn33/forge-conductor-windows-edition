#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>
#include <windows.h>

namespace winrt::ForgeConductorApp::implementation {
namespace {
using Visibility = Microsoft::UI::Xaml::Visibility;
constexpr wchar_t ViewSettingsKey[] =
    L"Software\\Forge Conductor\\Windows";

struct RegistryKey final {
    HKEY value{};
    ~RegistryKey() { if (value) ::RegCloseKey(value); }
};

[[nodiscard]] std::optional<hstring> loadSavedText(
    const wchar_t* const valueName) noexcept
{
    try {
        DWORD bytes{};
        if (::RegGetValueW(HKEY_CURRENT_USER, ViewSettingsKey,
                valueName, RRF_RT_REG_SZ, nullptr, nullptr,
                &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
            return std::nullopt;
        }
        std::vector<wchar_t> value(bytes / sizeof(wchar_t));
        if (::RegGetValueW(HKEY_CURRENT_USER, ViewSettingsKey,
                valueName, RRF_RT_REG_SZ, nullptr, value.data(),
                &bytes) != ERROR_SUCCESS || value.empty()) {
            return std::nullopt;
        }
        return hstring{value.data()};
    } catch (...) {
        return std::nullopt;
    }
}

void storeSavedText(
    const wchar_t* const valueName,
    const hstring& value) noexcept
{
    RegistryKey key;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, ViewSettingsKey, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key.value,
            nullptr) != ERROR_SUCCESS) {
        return;
    }
    const auto bytes = static_cast<DWORD>(
        (value.size() + 1U) * sizeof(wchar_t));
    static_cast<void>(::RegSetValueExW(key.value, valueName, 0,
        REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes));
}

void clearSavedText(const wchar_t* const valueName) noexcept
{
    RegistryKey key;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, ViewSettingsKey, 0,
            KEY_SET_VALUE, &key.value) != ERROR_SUCCESS) {
        return;
    }
    static_cast<void>(::RegDeleteValueW(key.value, valueName));
}

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

[[nodiscard]] std::vector<std::string> commaSeparatedTags(std::string value)
{
    std::vector<std::string> tags;
    std::size_t begin{};
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        auto tag = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto first = tag.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const auto last = tag.find_last_not_of(" \t\r\n");
            tags.push_back(tag.substr(first, last - first + 1U));
        }
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return tags;
}

void applyMetric(
    const Microsoft::UI::Xaml::Controls::TextBlock& value,
    const Microsoft::UI::Xaml::Controls::TextBlock& state,
    const Microsoft::UI::Xaml::Controls::ProgressBar& gauge,
    const ::ForgeConductor::Hosts::App::MetricPresentation& presentation)
{
    value.Text(winrt::to_hstring(presentation.value));
    state.Text(winrt::to_hstring(presentation.state));
    gauge.IsIndeterminate(!presentation.gaugePercent.has_value());
    if (presentation.gaugePercent) gauge.Value(*presentation.gaugePercent);
}

[[nodiscard]] Microsoft::UI::Xaml::Media::PointCollection chartPoints(
    const std::vector<double>& values,
    const double width,
    const double height)
{
    Microsoft::UI::Xaml::Media::PointCollection points;
    if (values.empty()) return points;
    const auto denominator = values.size() > 1U
        ? static_cast<double>(values.size() - 1U)
        : 1.0;
    for (std::size_t index{}; index < values.size(); ++index) {
        const auto x = values.size() == 1U
            ? width
            : width * static_cast<double>(index) / denominator;
        const auto y = height - height * std::clamp(values[index], 0.0, 100.0) /
            100.0;
        points.Append(Windows::Foundation::Point{
            static_cast<float>(x), static_cast<float>(y)});
    }
    return points;
}

[[nodiscard]] hstring currentLocalTime()
{
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t text[32]{};
    static_cast<void>(swprintf_s(text, L"Today %02u:%02u:%02u",
        now.wHour, now.wMinute, now.wSecond));
    return hstring{text};
}

[[nodiscard]] hstring currentMachineName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (::GetComputerNameW(name, &size) && size != 0U) return hstring{name, size};
    return L"Windows workstation";
}
}

MainWindow::MainWindow()
{
    SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
}

MainWindow::MainWindow(
    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection)
    : connection_{std::move(connection)}
{
    SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
    const auto scope = connection_ ? connection_->viewStateScope() : std::nullopt;
    selectedPageValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedPage", scope);
    selectedProjectValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedProjectId", scope);
}

void MainWindow::WindowClosed(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::WindowEventArgs const&)
{
    if (telemetryTimer_) telemetryTimer_.Stop();
    actionScheduler_.cancel();
    cancellation_.request_stop();
}

void MainWindow::WindowContentLoaded(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (telemetryUiInitialized_) return;
    telemetryUiInitialized_ = true;
    try {
        const auto titleBar = AppWindow().TitleBar();
        titleBar.BackgroundColor(Windows::UI::Color{255, 7, 17, 30});
        titleBar.ForegroundColor(Windows::UI::Color{255, 244, 248, 252});
        titleBar.InactiveBackgroundColor(Windows::UI::Color{255, 7, 17, 30});
        titleBar.InactiveForegroundColor(Windows::UI::Color{255, 132, 149, 168});
        titleBar.ButtonBackgroundColor(Windows::UI::Color{255, 7, 17, 30});
        titleBar.ButtonForegroundColor(Windows::UI::Color{255, 244, 248, 252});
        titleBar.ButtonHoverBackgroundColor(Windows::UI::Color{255, 24, 40, 59});
        titleBar.ButtonHoverForegroundColor(Windows::UI::Color{255, 255, 255, 255});
        titleBar.ButtonPressedBackgroundColor(Windows::UI::Color{255, 43, 168, 255});
        titleBar.ButtonPressedForegroundColor(Windows::UI::Color{255, 255, 255, 255});
    } catch (...) {
        // Title-bar theming is presentation-only; never block the runtime surface.
    }
    FooterMachineName().Text(currentMachineName());
    ProfileState().Text(winrt::to_hstring(connection_
        ? connection_->profileSummary()
        : std::string{"Deployment profile unavailable"}));
    providerSettings_.emplace();
    ApplyProviderForm(*providerSettings_);
    ApplySettingsForm(*providerSettings_);
    ProviderState().Text(L"Validated product defaults are available while the Manager connects.");
    SettingsState().Text(L"Validated product defaults are available while effective settings load.");
    if (const auto savedProject = loadSavedText(
            selectedProjectValueName_.c_str())) {
        selectedProjectId_ = winrt::to_string(*savedProject);
        RunProjectId().Text(*savedProject);
    }
    if (const auto saved = loadSavedText(selectedPageValueName_.c_str())) {
        const auto items = RootNavigation().MenuItems();
        for (std::uint32_t index{}; index < items.Size(); ++index) {
            const auto item = items.GetAt(index).try_as<
                Microsoft::UI::Xaml::Controls::NavigationViewItem>();
            if (item && unbox_value_or<hstring>(item.Tag(), L"") == *saved) {
                RootNavigation().SelectedItem(item);
                break;
            }
        }
    }
    const auto weak = get_weak();
    telemetryTimer_ = Microsoft::UI::Xaml::DispatcherTimer{};
    telemetryTimer_.Interval(std::chrono::milliseconds{500});
    telemetryTimer_.Tick([weak](auto const&, auto const&) {
        if (const auto self = weak.get()) self->RunAction(Action::Refresh);
    });
    RunAction(Action::Start);
    RunAction(Action::SettingsLoad);
    RunAction(Action::ProjectList);
    RunAction(Action::Refresh);
    telemetryTimer_.Start();
}

void MainWindow::RefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::Refresh); }
void MainWindow::StartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::Start);
}
void MainWindow::StopClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::Stop);
}
void MainWindow::RestartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::Restart);
}
void MainWindow::ProviderLoadClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderLoad); }
void MainWindow::ProviderSaveClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderSave); }
void MainWindow::ProviderTestClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderTest); }
void MainWindow::SettingsLoadClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::SettingsLoad); }
void MainWindow::SettingsSaveClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::SettingsSave); }
void MainWindow::SettingsRevertClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (providerSettings_) {
        ApplySettingsForm(*providerSettings_);
        SettingsState().Text(L"Pending edits were reverted to the last effective readback.");
    } else {
        RunAction(Action::SettingsLoad);
    }
}
void MainWindow::SettingsTestClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::SettingsTest); }
void MainWindow::SettingsRestartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::SettingsRestart); }
void MainWindow::MaintenanceResetClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::MaintenanceReset); }
void MainWindow::OpenProviderClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Provider"); }
void MainWindow::OpenManagerClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Manager"); }

void MainWindow::SelectPage(const winrt::hstring& tag)
{
    for (const auto& value : RootNavigation().MenuItems()) {
        const auto item = value.try_as<
            Microsoft::UI::Xaml::Controls::NavigationViewItem>();
        if (item && unbox_value_or<hstring>(item.Tag(), L"") == tag) {
            RootNavigation().SelectedItem(item);
            return;
        }
    }
}
void MainWindow::RunStartClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunStart); }
void MainWindow::RunStatusClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunStatus); }
void MainWindow::RunPauseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunPause); }
void MainWindow::RunResumeClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunResume); }
void MainWindow::RunCancelClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunCancel); }
void MainWindow::ProjectRegisterClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectRegister); }
void MainWindow::ProjectRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectList); }
void MainWindow::ProjectSearchClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectLoad); }
void MainWindow::ProjectRememberClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectRemember); }
void MainWindow::LmStudioInspectClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioInspect); }
void MainWindow::LmStudioRepairClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioRepair); }
void MainWindow::LmStudioActivateClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioActivate); }
void MainWindow::ToolsRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ToolsList); }
void MainWindow::ToolInvokeClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ToolInvoke); }
void MainWindow::ToolFilterChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&) { FilterTools(); }
void MainWindow::ToolPackFilterChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) { FilterTools(); }
void MainWindow::ToolSelectionChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = ToolList().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= visibleTools_.size()) return;
    const auto& tool = tools_[visibleTools_[static_cast<std::size_t>(index)]];
    ToolName().Text(winrt::to_hstring(tool.name));
    ToolDetailName().Text(winrt::to_hstring(tool.name));
    ToolDetailPack().Text(winrt::to_hstring(tool.pack + " · Manager-owned capability"));
    ToolDetailDescription().Text(winrt::to_hstring(tool.description));
    ToolDetailPolicy().Text(winrt::to_hstring(
        std::string{tool.requiresProject ? "Selected project authority required" : "No project binding required"} +
        (tool.requiresShell ? " · shell policy applies" : "") +
        ". Canonical arguments are validated by the Manager."));
}
void MainWindow::OperationalRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::OperationalInspect); }
void MainWindow::OperationalPruneClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::OperationalPrune); }
void MainWindow::OperationalCloseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::OperationalClose); }
void MainWindow::OperationalSelectionChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = OperationalList().SelectedIndex();
    if (index >= 0) SelectOperationalRecord(static_cast<std::size_t>(index));
}
void MainWindow::OperationalCardSelectionChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = OperationalCards().SelectedIndex();
    if (index >= 0) SelectOperationalRecord(static_cast<std::size_t>(index));
}
void MainWindow::SelectOperationalRecord(const std::size_t index)
{
    if (index >= operationalLines_.size()) return;
    const auto& line = operationalLines_[index];
    const auto separator = line.find('\n');
    OperationalDetailTitle().Text(winrt::to_hstring(line.substr(0, separator)));
    OperationalDetailBody().Text(winrt::to_hstring(
        separator == std::string::npos ? line : line.substr(separator + 1)));
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Agents) {
        const auto idEnd = line.find(" · ");
        if (idEnd != std::string::npos && line.find("Tools: ") == std::string::npos) {
            OperationalSessionId().Text(winrt::to_hstring(line.substr(0, idEnd)));
        }
    }
}

void MainWindow::ProjectSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    if (rebuildingProjects_) return;
    const auto index = ProjectSelector().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= projects_.size()) return;
    selectedProjectId_ = projects_[static_cast<std::size_t>(index)].id.value();
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);
    RunProjectId().Text(selected);
    ToolProjectId().Text(selected);
    RunAction(Action::ProjectLoad);
}

void MainWindow::NavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
{
    const auto item = args.SelectedItem().try_as<
        Microsoft::UI::Xaml::Controls::NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"Rig");
    PageTitle().Text(tag);
    if (telemetryUiInitialized_) {
        storeSavedText(selectedPageValueName_.c_str(), tag);
    }
    const bool provider = tag == L"Provider";
    const bool settings = tag == L"Settings";
    const bool autonomy = tag == L"Autonomy" || tag == L"Continuity";
    const bool continuityPage = tag == L"Continuity";
    const bool rig = tag == L"Rig";
    const bool projects = tag == L"Projects";
    const bool lmStudioMcp = tag == L"LM Studio MCP";
    const bool tools = tag == L"Tools";
    const bool operational = tag == L"Agents" || tag == L"Feed" ||
        tag == L"Events & Evidence" || tag == L"Runtimes" ||
        tag == L"Diagnostics" || tag == L"Manager";
    if (tag == L"Agents") operationalArea_ = ::ForgeConductor::Manager::ManagerOperationalArea::Agents;
    else if (tag == L"Feed" || tag == L"Events & Evidence") operationalArea_ = ::ForgeConductor::Manager::ManagerOperationalArea::Feed;
    else if (tag == L"Runtimes") operationalArea_ = ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes;
    else if (tag == L"Diagnostics") operationalArea_ = ::ForgeConductor::Manager::ManagerOperationalArea::Diagnostics;
    else if (tag == L"Manager") operationalArea_ = ::ForgeConductor::Manager::ManagerOperationalArea::Manager;
    if (operational) {
        const bool agents = tag == L"Agents";
        const bool feed = tag == L"Feed";
        const bool evidence = tag == L"Events & Evidence";
        const bool runtimes = tag == L"Runtimes";
        const bool diagnostics = tag == L"Diagnostics";
        OperationalHeading().Text(agents ? L"Agent playbooks & sessions" :
            feed ? L"Recent tool activity" :
            evidence ? L"Activity & evidence boundary" :
            runtimes ? L"Native runtime inventory" :
            diagnostics ? L"Health & diagnostics" : L"Manager ownership");
        OperationalSubtitle().Text(agents ? L"Inspect available specialists and exact session identities." :
            feed ? L"Manager-owned chronological audit outcomes." :
            evidence ? L"Audit activity is visible; durable evidence requires a separate verified projection." :
            runtimes ? L"Owned operations, threads, processes, repositories and databases." :
            diagnostics ? L"Doctor checks and bounded diagnostic output." :
            L"Current service identity and owned runtime resources.");
        OperationalListTitle().Text(agents ? L"Available specialists & sessions" :
            feed ? L"Recent outcomes" :
            evidence ? L"Audited outcomes" :
            runtimes ? L"Resource inventory" :
            diagnostics ? L"Doctor checks" : L"Manager state");
        OperationalHeroIcon().Glyph(agents ? L"\uE716" :
            feed || evidence ? L"\uE8D4" :
            runtimes ? L"\uE7F4" :
            diagnostics ? L"\uE713" : L"\uE77B");
        OperationalPruneButton().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalSessionCard().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalCards().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalList().Visibility(agents ? Visibility::Collapsed : Visibility::Visible);
    }
    ProviderPanel().Visibility(provider ? Visibility::Visible : Visibility::Collapsed);
    AutonomyPanel().Visibility(autonomy ? Visibility::Visible : Visibility::Collapsed);
    AutonomyOverviewCard().Visibility(continuityPage ? Visibility::Collapsed : Visibility::Visible);
    ContinuityOverviewCard().Visibility(continuityPage ? Visibility::Visible : Visibility::Collapsed);
    RunControlHeading().Text(continuityPage ? L"Exact run handoff controls" : L"Run control");
    RunStateHeading().Text(continuityPage ? L"Latest continuity readback" : L"Run and continuity state");
    RigPanel().Visibility(rig ? Visibility::Visible : Visibility::Collapsed);
    ProjectsPanel().Visibility(projects ? Visibility::Visible : Visibility::Collapsed);
    LmStudioMcpPanel().Visibility(lmStudioMcp ? Visibility::Visible : Visibility::Collapsed);
    ToolsPanel().Visibility(tools ? Visibility::Visible : Visibility::Collapsed);
    OperationalPanel().Visibility(operational ? Visibility::Visible : Visibility::Collapsed);
    SettingsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
    GenericPanel().Visibility(
        !provider && !settings && !rig && !autonomy && !projects && !lmStudioMcp && !tools && !operational
            ? Visibility::Visible : Visibility::Collapsed);
    if (provider) {
        PageDescription().Text(L"Configure and test the Manager-owned LM Studio Responses endpoint.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else if (autonomy) {
        PageDescription().Text(continuityPage
            ? L"Inspect retained context and control the exact Manager-owned run."
            : L"Start, attach, pause, resume, and stop Manager-owned work.");
    } else if (tag == L"Projects") {
        PageDescription().Text(L"Register authorized folders, select exact project identities, and read or write persistent project memory.");
        RunAction(Action::ProjectList);
    } else if (lmStudioMcp) {
        PageDescription().Text(L"Inspect, repair, activate, and verify the LM Studio MCP registration.");
        RunAction(Action::LmStudioInspect);
    } else if (tools) {
        PageDescription().Text(L"Inspect and run the Manager-owned native tool catalog.");
        if (!selectedProjectId_.empty()) {
            ToolProjectId().Text(winrt::to_hstring(selectedProjectId_));
        }
        RunAction(Action::ToolsList);
    } else if (settings) {
        PageDescription().Text(L"Edit effective Manager, dashboard, provider, context, logging, and runtime preferences without editing configuration files.");
        const auto project = selectedProjectId_.empty()
            ? std::string{"<select a project first>"} : selectedProjectId_;
        MaintenanceState().Text(winrt::to_hstring(
            "Exact confirmations for the current selection:\n"
            "Memory: RESET PROJECT MEMORY " + project +
            "\nContinuity: RESET PROJECT CONTINUITY " + project +
            "\nCombined: RESET PROJECT DATA " + project +
            "\nAll registered project data: RESET ALL PROJECT DATA"));
        RunAction(Action::SettingsLoad);
    } else if (operational) {
        PageDescription().Text(tag == L"Agents" ? L"Browse native specialists and their live sessions." :
            tag == L"Feed" ? L"Read recent Manager-owned tool outcomes." :
            tag == L"Events & Evidence" ? L"Distinguish audited activity from durable trusted evidence." :
            tag == L"Runtimes" ? L"Inspect native resource ownership and service state." :
            tag == L"Diagnostics" ? L"Run through health checks and diagnostic readback." :
            L"Inspect Manager ownership and runtime state.");
        RunAction(Action::OperationalInspect);
    } else {
        PageDescription().Text(L"Inspect the current typed Manager operational snapshot.");
    }
    if (telemetrySnapshot_) ApplyTelemetryPresentation(*telemetrySnapshot_);
}

void MainWindow::TelemetryChartSizeChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::SizeChangedEventArgs const&)
{
    if (telemetrySnapshot_) ApplyTelemetryPresentation(*telemetrySnapshot_);
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

std::optional<::ForgeConductor::Domain::ManagerSettings>
MainWindow::ReadSettingsForm(std::string& error)
{
    try {
        auto settings = providerSettings_.value_or(
            ::ForgeConductor::Domain::ManagerSettings{});
        settings.dashboardHost = winrt::to_string(SettingsDashboardHost().Text());
        const auto dashboardPort = numberValue(SettingsDashboardPort(), "Dashboard port");
        const auto providerPort = numberValue(SettingsProviderPort(), "Provider port");
        if (dashboardPort == 0U || dashboardPort > 65'535U ||
            providerPort == 0U || providerPort > 65'535U) {
            throw std::invalid_argument{"Ports must be within 1 through 65535."};
        }
        settings.dashboardPort = static_cast<std::uint16_t>(dashboardPort);
        settings.dashboardRefreshInterval = std::chrono::seconds{
            numberValue(SettingsRefreshSeconds(), "Refresh interval")};
        settings.watchdogInterval = std::chrono::seconds{
            numberValue(SettingsWatchdogSeconds(), "Watchdog interval")};
        settings.autoRestart = SettingsAutoRestart().IsOn();
        settings.openBrowserOnStart = SettingsOpenBrowser().IsOn();
        settings.sessionIdleTtl = std::chrono::seconds{
            numberValue(SettingsSessionTtl(), "Session retention")};
        settings.shellTimeout = std::chrono::seconds{
            numberValue(SettingsShellTimeout(), "Shell timeout")};
        settings.shellEnabled = SettingsShellEnabled().IsOn();
        const auto logIndex = SettingsLogLevel().SelectedIndex();
        if (logIndex < 0 || logIndex > 5) {
            throw std::invalid_argument{"Select a log detail level."};
        }
        settings.logLevel = static_cast<::ForgeConductor::Domain::LogLevel>(logIndex);
        settings.localModelHost = winrt::to_string(SettingsProviderHost().Text());
        settings.localModelPort = static_cast<std::uint16_t>(providerPort);
        settings.localModelSecure = SettingsProviderSecure().IsOn();
        settings.localModelName = winrt::to_string(SettingsProviderModel().Text());
        settings.effectiveContextCapacity = numberValue(
            SettingsContextCapacity(), "Effective context capacity");
        settings.nextResponseReserve = numberValue(
            SettingsResponseReserve(), "Next response reserve");
        settings.handoffReserve = numberValue(
            SettingsHandoffReserve(), "Handoff reserve");
        settings.estimationSafetyMargin = numberValue(
            SettingsSafetyMargin(), "Estimation safety margin");
        auto valid = ::ForgeConductor::Domain::validateManagerSettings(settings);
        if (!valid) throw std::invalid_argument{valid.error().message};
        return settings;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

void MainWindow::ApplySettingsForm(
    const ::ForgeConductor::Domain::ManagerSettings& settings)
{
    SettingsDashboardHost().Text(winrt::to_hstring(settings.dashboardHost));
    SettingsDashboardPort().Value(settings.dashboardPort);
    SettingsRefreshSeconds().Value(static_cast<double>(
        settings.dashboardRefreshInterval.count()));
    SettingsWatchdogSeconds().Value(static_cast<double>(
        settings.watchdogInterval.count()));
    SettingsAutoRestart().IsOn(settings.autoRestart);
    SettingsOpenBrowser().IsOn(settings.openBrowserOnStart);
    SettingsSessionTtl().Value(static_cast<double>(settings.sessionIdleTtl.count()));
    SettingsShellTimeout().Value(static_cast<double>(settings.shellTimeout.count()));
    SettingsShellEnabled().IsOn(settings.shellEnabled);
    SettingsLogLevel().SelectedIndex(static_cast<std::int32_t>(settings.logLevel));
    SettingsProviderHost().Text(winrt::to_hstring(settings.localModelHost));
    SettingsProviderPort().Value(settings.localModelPort);
    SettingsProviderSecure().IsOn(settings.localModelSecure);
    SettingsProviderModel().Text(winrt::to_hstring(settings.localModelName));
    SettingsContextCapacity().Value(settings.effectiveContextCapacity);
    SettingsResponseReserve().Value(settings.nextResponseReserve);
    SettingsHandoffReserve().Value(settings.handoffReserve);
    SettingsSafetyMargin().Value(settings.estimationSafetyMargin);
}

void MainWindow::ApplyTelemetryPresentation(
    const ::ForgeConductor::Domain::ManagerTelemetrySnapshot& snapshot)
{
    if (!selectedProjectId_.empty() &&
        !::ForgeConductor::Hosts::App::containsProjectId(
            snapshot.projects, selectedProjectId_)) {
        rebuildingProjects_ = true;
        ProjectSelector().SelectedIndex(-1);
        rebuildingProjects_ = false;
        ClearSelectedProject();
    }
    const auto presentation =
        ::ForgeConductor::Hosts::App::makeTelemetryPresentation(snapshot);
    applyMetric(CpuValue(), CpuState(), CpuGauge(), presentation.cpu);
    applyMetric(RamValue(), RamState(), RamGauge(), presentation.ram);
    applyMetric(GpuValue(), GpuState(), GpuGauge(), presentation.gpu);
    applyMetric(
        ContextValue(), ContextState(), ContextGauge(), presentation.context);
    ContinuityContextState().Text(winrt::to_hstring(
        presentation.context.value + " · " + presentation.context.state));
    ManagerHealth().Text(winrt::to_hstring(presentation.managerStatus));
    ProviderHealth().Text(winrt::to_hstring(presentation.providerStatus));
    StoreHealth().Text(winrt::to_hstring(presentation.storeStatus));
    ContinuityHealth().Text(winrt::to_hstring(presentation.continuityStatus));
    SystemStrip().Text(winrt::to_hstring(presentation.systemStatus));
    SamplingStrip().Text(winrt::to_hstring(presentation.samplingStatus));
    DiskState().Text(winrt::to_hstring(presentation.diskStatus));
    WorkflowStatus().Text(winrt::to_hstring(presentation.workflowStatus));
    NavigationManagerState().Text(L"System online");
    LastUpdatedText().Text(currentLocalTime());
    const auto online = Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 61, 220, 151}};
    NavigationStatusDot().Fill(online);
    HeroManagerDot().Fill(online);
    WorkflowDot().Fill(online);
    ProviderDot().Fill(online);
    ContinuityDot().Fill(online);
    StoreDot().Fill(online);

    const auto applyRows = [](const Microsoft::UI::Xaml::Controls::StackPanel& panel,
                              const std::vector<std::string>& rows,
                              const wchar_t* emptyText) {
        panel.Children().Clear();
        if (rows.empty()) {
            Microsoft::UI::Xaml::Controls::TextBlock row;
            row.Text(emptyText);
            row.Opacity(0.72);
            panel.Children().Append(row);
            return;
        }
        for (const auto& text : rows) {
            Microsoft::UI::Xaml::Controls::TextBlock row;
            row.Text(winrt::to_hstring(text));
            row.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            row.IsTextSelectionEnabled(true);
            panel.Children().Append(row);
        }
    };
    CpuCoreRows().Children().Clear();
    if (presentation.cpuLogicalRows.empty()) {
        Microsoft::UI::Xaml::Controls::TextBlock row;
        row.Text(L"Logical processor counters are unavailable.");
        row.Opacity(0.72);
        CpuCoreRows().Children().Append(row);
    } else {
        for (std::size_t index{}; index < presentation.cpuLogicalRows.size(); ++index) {
            Microsoft::UI::Xaml::Controls::StackPanel row;
            row.Spacing(2.0);
            Microsoft::UI::Xaml::Controls::TextBlock label;
            label.Text(winrt::to_hstring(presentation.cpuLogicalRows[index]));
            label.IsTextSelectionEnabled(true);
            Microsoft::UI::Xaml::Controls::ProgressBar gauge;
            gauge.Minimum(0.0);
            gauge.Maximum(100.0);
            gauge.Value(index < presentation.cpuLogicalValues.size()
                ? presentation.cpuLogicalValues[index] : 0.0);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
                gauge, winrt::to_hstring(presentation.cpuLogicalRows[index]));
            row.Children().Append(label);
            row.Children().Append(gauge);
            CpuCoreRows().Children().Append(row);
        }
    }
    applyRows(GpuAdapterRows(), presentation.gpuRows,
              L"No hardware GPU adapter was reported.");
    applyRows(VolumeRows(), presentation.volumeRows,
              L"No mounted fixed or removable volume was reported.");
    applyRows(ProcessRows(), presentation.processRows,
              L"No Forge or model-server process is currently visible.");

    const auto width = std::max(320.0, HistoryCanvas().ActualWidth());
    const auto height = std::max(1.0, HistoryCanvas().ActualHeight());
    CpuHistoryLine().Points(chartPoints(presentation.cpuHistory, width, height));
    RamHistoryLine().Points(chartPoints(presentation.ramHistory, width, height));
    GpuHistoryLine().Points(chartPoints(presentation.gpuHistory, width, height));
    MiniCpuLine().Points(chartPoints(presentation.cpuHistory,
        std::max(1.0, MiniCpuCanvas().ActualWidth()),
        std::max(1.0, MiniCpuCanvas().ActualHeight())));
    MiniRamLine().Points(chartPoints(presentation.ramHistory,
        std::max(1.0, MiniRamCanvas().ActualWidth()),
        std::max(1.0, MiniRamCanvas().ActualHeight())));
    MiniGpuLine().Points(chartPoints(presentation.gpuHistory,
        std::max(1.0, MiniGpuCanvas().ActualWidth()),
        std::max(1.0, MiniGpuCanvas().ActualHeight())));
    if (presentation.cpuHistory.empty()) {
        HistoryEquivalentText().Text(L"No measured CPU/RAM history samples.");
    } else {
        HistoryEquivalentText().Text(winrt::to_hstring(
            std::to_string(presentation.cpuHistory.size()) +
            " measured samples · latest CPU " + presentation.cpu.value +
            " · latest RAM " + presentation.ram.value));
    }

    if (presentation.diskHistoryBytesPerSecond.empty()) {
        DiskHistoryLine().Points(Microsoft::UI::Xaml::Media::PointCollection{});
        DiskHistoryEquivalentText().Text(L"No measured disk throughput samples.");
    } else {
        const auto diskMaximum = *std::max_element(
            presentation.diskHistoryBytesPerSecond.begin(),
            presentation.diskHistoryBytesPerSecond.end());
        std::vector<double> normalized;
        normalized.reserve(presentation.diskHistoryBytesPerSecond.size());
        for (const auto value : presentation.diskHistoryBytesPerSecond) {
            normalized.push_back(diskMaximum > 0.0 ? value * 100.0 / diskMaximum : 0.0);
        }
        DiskHistoryLine().Points(chartPoints(
            normalized, std::max(320.0, DiskHistoryCanvas().ActualWidth()), 72.0));
        DiskHistoryEquivalentText().Text(winrt::to_hstring(
            std::to_string(normalized.size()) + " samples · latest " +
            ::ForgeConductor::Hosts::App::bytesText(static_cast<std::uint64_t>(
                presentation.diskHistoryBytesPerSecond.back())) + "/s · peak " +
            ::ForgeConductor::Hosts::App::bytesText(static_cast<std::uint64_t>(
                diskMaximum)) + "/s"));
    }

    if (presentation.latencyHistoryMilliseconds.empty()) {
        LatencyHistoryLine().Points(
            Microsoft::UI::Xaml::Media::PointCollection{});
        LatencyEquivalentText().Text(L"No measured activity latency observations.");
    } else {
        const auto maximum = *std::max_element(
            presentation.latencyHistoryMilliseconds.begin(),
            presentation.latencyHistoryMilliseconds.end());
        std::vector<double> normalized;
        normalized.reserve(presentation.latencyHistoryMilliseconds.size());
        for (const auto value : presentation.latencyHistoryMilliseconds) {
            normalized.push_back(maximum > 0.0 ? value * 100.0 / maximum : 0.0);
        }
        const auto latencyWidth = std::max(320.0, LatencyCanvas().ActualWidth());
        LatencyHistoryLine().Points(chartPoints(normalized, latencyWidth, 72.0));
        LatencyEquivalentText().Text(winrt::to_hstring(
            std::to_string(normalized.size()) + " observations · latest " +
            std::to_string(static_cast<std::uint64_t>(
                presentation.latencyHistoryMilliseconds.back())) +
            " ms · chart maximum " +
            std::to_string(static_cast<std::uint64_t>(maximum)) + " ms"));
    }

    ActivityTimeline().Children().Clear();
    for (const auto& item : presentation.timeline) {
        Microsoft::UI::Xaml::Controls::TextBlock row;
        row.Text(winrt::to_hstring(item));
        row.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        row.IsTextSelectionEnabled(true);
        ActivityTimeline().Children().Append(row);
    }

    const auto page = winrt::to_string(PageTitle().Text());
    const auto detail = ::ForgeConductor::Hosts::App::telemetryDetailText(
        snapshot, page);
    if (page == "Provider") {
        ProviderState().Text(winrt::to_hstring(detail));
    } else if (page != "Rig" && page != "Autonomy" && page != "Continuity") {
        GenericState().Text(winrt::to_hstring(detail));
    }
}

void MainWindow::ApplyProjectList(
    const ::ForgeConductor::Manager::ManagerProjectsSnapshot& snapshot)
{
    rebuildingProjects_ = true;
    projects_ = snapshot.projects;
    ProjectSelector().Items().Clear();
    std::int32_t selectedIndex{-1};
    for (std::size_t index{}; index < projects_.size(); ++index) {
        const auto& project = projects_[index];
        ProjectSelector().Items().Append(box_value(winrt::to_hstring(
            project.displayName + " · " + project.id.value())));
        if (project.id.value() == selectedProjectId_) {
            selectedIndex = static_cast<std::int32_t>(index);
        }
    }
    ProjectSelector().SelectedIndex(selectedIndex);
    rebuildingProjects_ = false;

    if (selectedIndex >= 0) {
        selectedProjectId_ = projects_[static_cast<std::size_t>(selectedIndex)].id.value();
        const auto selected = winrt::to_hstring(selectedProjectId_);
        storeSavedText(selectedProjectValueName_.c_str(), selected);
        RunProjectId().Text(selected);
        ToolProjectId().Text(selected);
    } else {
        ClearSelectedProject();
        ProjectIdentity().Text(L"No registered project is selected.");
        ProjectFolders().Text(L"Register an authorized folder to begin.");
        ProjectPersistence().Text(L"No project memory store is active.");
        ProjectMemoryRecords().Children().Clear();
    }
}

void MainWindow::ClearSelectedProject()
{
    selectedProjectId_.clear();
    clearSavedText(selectedProjectValueName_.c_str());
    RunProjectId().Text(L"");
    ToolProjectId().Text(L"");
    MaintenanceState().Text(
        L"Select the exact project on the Projects page first.\n"
        L"All registered project data: RESET ALL PROJECT DATA");
}

void MainWindow::ApplyProjectWorkspace(
    const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot)
{
    selectedProjectId_ = snapshot.project.id.value();
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);
    RunProjectId().Text(selected);
    ToolProjectId().Text(selected);

    std::string identity = "Active project: " + snapshot.project.displayName +
        "\nExact ID: " + selectedProjectId_;
    if (snapshot.project.repositoryIdentity) {
        identity += "\nRepository identity: " +
            *snapshot.project.repositoryIdentity;
    }
    ProjectIdentity().Text(winrt::to_hstring(identity));

    std::string folders = "Authorized folders";
    for (const auto& alias : snapshot.project.aliases) {
        folders += "\n• " + alias.value();
    }
    if (snapshot.project.aliases.empty()) folders += "\nNone";
    ProjectFolders().Text(winrt::to_hstring(folders));

    const auto searchMode = snapshot.fullTextSearchAvailable
        ? "full-text and lexical search"
        : "lexical search";
    ProjectPersistence().Text(winrt::to_hstring(
        std::string{snapshot.integrityOk ? "Integrity verified" : "Integrity check failed"} +
        " · " + std::to_string(snapshot.recordCount) + " active records · " +
        std::to_string(snapshot.tombstoneCount) + " tombstones · " +
        std::to_string(snapshot.databaseBytes) + " database bytes · " + searchMode));

    ProjectMemoryRecords().Children().Clear();
    if (snapshot.records.empty()) {
        Microsoft::UI::Xaml::Controls::TextBlock empty;
        empty.Text(L"No matching project memory records.");
        ProjectMemoryRecords().Children().Append(empty);
    }
    for (const auto& record : snapshot.records) {
        std::string text = record.title + "\n" + record.summary;
        if (record.body && !record.body->empty()) text += "\n\n" + *record.body;
        text += "\n\n" + record.kind + " · v" +
            std::to_string(record.version) + " · " + record.id.value();
        if (!record.tags.empty()) {
            text += "\nTags: ";
            for (std::size_t index{}; index < record.tags.size(); ++index) {
                if (index != 0U) text += ", ";
                text += record.tags[index];
            }
        }
        Microsoft::UI::Xaml::Controls::TextBlock row;
        row.Text(winrt::to_hstring(text));
        row.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        row.IsTextSelectionEnabled(true);
        ProjectMemoryRecords().Children().Append(row);
    }
}

void MainWindow::ApplyDisconnectedTelemetry(const std::string_view reason)
{
    const auto explanation = reason.empty()
        ? std::string{"Manager telemetry is unavailable."}
        : std::string{reason};
    const auto unavailable = ::ForgeConductor::Hosts::App::MetricPresentation{
        "Unavailable", explanation, std::nullopt};
    applyMetric(CpuValue(), CpuState(), CpuGauge(), unavailable);
    applyMetric(RamValue(), RamState(), RamGauge(), unavailable);
    applyMetric(GpuValue(), GpuState(), GpuGauge(), unavailable);
    applyMetric(ContextValue(), ContextState(), ContextGauge(), unavailable);
    ManagerHealth().Text(winrt::to_hstring("Disconnected · " + explanation));
    ProviderHealth().Text(L"Unavailable while Manager is disconnected");
    StoreHealth().Text(L"Unavailable while Manager is disconnected");
    ContinuityHealth().Text(L"Unavailable while Manager is disconnected");
    SystemStrip().Text(L"System telemetry disconnected");
    SamplingStrip().Text(winrt::to_hstring(explanation));
    DiskState().Text(winrt::to_hstring(explanation));
    WorkflowStatus().Text(winrt::to_hstring(explanation));
    NavigationManagerState().Text(L"Manager unavailable");
    LastUpdatedText().Text(L"Connection unavailable");
    const auto unavailableBrush = Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 255, 200, 87}};
    NavigationStatusDot().Fill(unavailableBrush);
    HeroManagerDot().Fill(unavailableBrush);
    WorkflowDot().Fill(unavailableBrush);
    ProviderDot().Fill(unavailableBrush);
    ContinuityDot().Fill(unavailableBrush);
    StoreDot().Fill(unavailableBrush);
    if (telemetrySnapshot_) {
        HistoryEquivalentText().Text(
            L"Last measured CPU/RAM history is stale because the Manager is disconnected.");
        LatencyEquivalentText().Text(
            L"Last measured latency history is stale because the Manager is disconnected.");
        DiskHistoryEquivalentText().Text(
            L"Last measured disk history is stale because the Manager is disconnected.");
    }
}

void MainWindow::ApplyLmStudio(
    const ::ForgeConductor::Manager::ManagerLmStudioSnapshot& snapshot)
{
    const auto installed = snapshot.primaryPluginInstalled &&
        snapshot.fallbackPluginInstalled && snapshot.continuityPluginInstalled &&
        snapshot.mcpConfigurationRegistered &&
        snapshot.binaryExecutable;
    LmStudioBadge().Text(installed ? L"REGISTERED" : L"ATTENTION");
    LmStudioOverview().Text(winrt::to_hstring(
        std::string{installed ? "Three native roles registered" : "Registration requires attention"} +
        " · " + (snapshot.connectedClientObserved ? "client observed" : "no connected client observed")));
    LmStudioPrimaryRole().Text(winrt::to_hstring(
        std::string{snapshot.primaryPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.primaryConnectorReady ? "connector ready" : "connector not ready")
            : "not yet verified")));
    LmStudioFallbackRole().Text(winrt::to_hstring(
        std::string{snapshot.fallbackPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.fallbackConnectorReady ? "connector ready" : "connector not ready")
            : "not yet verified")));
    LmStudioCluRole().Text(winrt::to_hstring(
        std::string{snapshot.continuityPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.continuityConnectorReady ? "connector ready" : "connector not ready")
            : "not yet verified")));
    LmStudioRegistrationState().Text(winrt::to_hstring(
        std::string{"Installed registration: "} + (installed ? "complete" : "incomplete") +
        "\nPrimary: " + (snapshot.primaryPluginInstalled ? "installed" : "missing") +
        " · Fallback: " + (snapshot.fallbackPluginInstalled ? "installed" : "missing") +
        " · CLU: " + (snapshot.continuityPluginInstalled ? "installed" : "missing") +
        " · MCP config: " + (snapshot.mcpConfigurationRegistered ? "registered" : "missing") +
        "\n" + snapshot.detail + "\n" + snapshot.actionDetail));
    LmStudioConnectionState().Text(winrt::to_hstring(
        snapshot.connectionCheckPerformed
            ? std::string{"Connector verification: primary "} +
                (snapshot.primaryConnectorReady ? "ready" : "not ready") +
                ", fallback " + (snapshot.fallbackConnectorReady ? "ready" : "not ready") +
                ", CLU " + (snapshot.continuityConnectorReady ? "ready" : "not ready") +
                ". Connected LM Studio client observed: " +
                (snapshot.connectedClientObserved ? "yes" : "no") + "."
            : "Connector verification: not run. Connected LM Studio client observed: no."));
    LmStudioContinuityState().Text(winrt::to_hstring(
        "Manager continuity projects active: " +
        std::to_string(snapshot.managedContinuityProjects)));
    LmStudioPaths().Text(winrt::to_hstring(
        "Binary: " + snapshot.binaryPath + "\nPrimary: " + snapshot.primaryPluginPath +
        "\nFallback: " + snapshot.fallbackPluginPath +
        "\nCLU: " + snapshot.continuityPluginPath +
        "\nConfiguration: " + snapshot.mcpConfigurationPath));
}

void MainWindow::ApplyTools(
    const ::ForgeConductor::Manager::ManagerToolsSnapshot& snapshot)
{
    tools_ = snapshot.tools;
    std::string text = "Shell preference: ";
    text += snapshot.shellEnabled ? "enabled" : "disabled";
    for (const auto& tool : snapshot.tools) {
        text += "\n\n" + tool.name + " [" + tool.pack + "]";
        if (tool.requiresShell) text += " · shell required";
        text += "\n" + tool.description + "\nSchema: " + tool.inputSchema;
    }
    ToolsCatalog().Text(winrt::to_hstring(text));
    rebuildingTools_ = true;
    ToolPackFilter().Items().Clear();
    Microsoft::UI::Xaml::Controls::ComboBoxItem all;
    all.Content(box_value(L"All packs"));
    ToolPackFilter().Items().Append(all);
    std::vector<std::string> packs;
    for (const auto& tool : tools_) {
        if (std::find(packs.begin(), packs.end(), tool.pack) == packs.end())
            packs.push_back(tool.pack);
    }
    std::sort(packs.begin(), packs.end());
    for (const auto& pack : packs) {
        Microsoft::UI::Xaml::Controls::ComboBoxItem item;
        item.Content(box_value(winrt::to_hstring(pack)));
        ToolPackFilter().Items().Append(item);
    }
    ToolPackFilter().SelectedIndex(0);
    rebuildingTools_ = false;
    FilterTools();
}

void MainWindow::FilterTools()
{
    if (rebuildingTools_ || !ToolList()) return;
    const auto search = winrt::to_string(ToolFilter().Text());
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    std::string pack;
    if (const auto item = ToolPackFilter().SelectedItem().try_as<
            Microsoft::UI::Xaml::Controls::ComboBoxItem>()) {
        pack = winrt::to_string(unbox_value_or<hstring>(item.Content(), L""));
    }
    ToolList().Items().Clear();
    visibleTools_.clear();
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const auto& tool = tools_[i];
        if (!pack.empty() && pack != "All packs" && pack != tool.pack) continue;
        auto haystack = tool.name + " " + tool.pack + " " + tool.description;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
            [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!needle.empty() && haystack.find(needle) == std::string::npos) continue;
        Microsoft::UI::Xaml::Controls::StackPanel row;
        row.Spacing(4);
        row.Padding(Microsoft::UI::Xaml::Thickness{12, 10, 12, 10});
        Microsoft::UI::Xaml::Controls::TextBlock name;
        name.Text(winrt::to_hstring(tool.name));
        name.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Cascadia Mono"});
        name.FontSize(13);
        name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        row.Children().Append(name);
        Microsoft::UI::Xaml::Controls::TextBlock description;
        description.Text(winrt::to_hstring(tool.description));
        description.FontSize(12);
        description.MaxLines(2);
        description.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
        description.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 168, 179, 199}});
        row.Children().Append(description);
        Microsoft::UI::Xaml::Controls::TextBlock metadata;
        metadata.Text(winrt::to_hstring(tool.pack +
            (tool.requiresProject ? " · project scope" : "") +
            (tool.requiresShell ? " · shell policy" : "")));
        metadata.FontSize(11);
        metadata.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 87, 166, 255}});
        row.Children().Append(metadata);
        ToolList().Items().Append(row);
        visibleTools_.push_back(i);
    }
    ToolsState().Text(winrt::to_hstring(std::to_string(visibleTools_.size()) +
        " of " + std::to_string(tools_.size()) + " Manager-owned tools · select a row for details"));
    if (!visibleTools_.empty()) ToolList().SelectedIndex(0);
}

void MainWindow::ApplyOperational(
    const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot)
{
    operationalLines_.clear();
    OperationalList().Items().Clear();
    OperationalCards().Items().Clear();
    OperationalState().Text(winrt::to_hstring(snapshot.title));
    std::string summary;
    for (const auto& line : snapshot.lines) {
        auto text = winrt::to_string(OperationalState().Text());
        OperationalState().Text(winrt::to_hstring(text + "\n\n" + line));
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents &&
            (line.starts_with("Agent definitions:") ||
             line.starts_with("Open sessions:") ||
             line.starts_with("Recent sessions:"))) {
            if (!summary.empty()) summary += "  ·  ";
            summary += line;
            continue;
        }
        operationalLines_.push_back(line);
        const auto separator = line.find('\n');
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents) {
            Microsoft::UI::Xaml::Controls::StackPanel cardContent;
            cardContent.Spacing(10);
            Microsoft::UI::Xaml::Controls::TextBlock name;
            name.Text(winrt::to_hstring(line.substr(0, separator)));
            name.FontSize(16);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            cardContent.Children().Append(name);
            Microsoft::UI::Xaml::Controls::TextBlock description;
            description.Text(winrt::to_hstring(separator == std::string::npos
                ? "Manager-owned session or inventory entry" : line.substr(separator + 1)));
            description.FontSize(12);
            description.MaxLines(4);
            description.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            description.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 168, 179, 199}});
            cardContent.Children().Append(description);
            Microsoft::UI::Xaml::Controls::Border card;
            card.Width(280);
            card.MinHeight(145);
            card.Padding(Microsoft::UI::Xaml::Thickness{16, 16, 16, 16});
            card.CornerRadius(Microsoft::UI::Xaml::CornerRadius{12});
            card.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 18, 23, 34}});
            card.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 52, 67, 94}});
            card.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
            card.Child(cardContent);
            OperationalCards().Items().Append(card);
            continue;
        }
        Microsoft::UI::Xaml::Controls::StackPanel row;
        row.Spacing(5);
        row.Padding(Microsoft::UI::Xaml::Thickness{12, 11, 12, 11});
        Microsoft::UI::Xaml::Controls::TextBlock heading;
        heading.Text(winrt::to_hstring(line.substr(0, separator)));
        heading.FontSize(13);
        heading.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        heading.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        row.Children().Append(heading);
        if (separator != std::string::npos) {
            Microsoft::UI::Xaml::Controls::TextBlock detail;
            detail.Text(winrt::to_hstring(line.substr(separator + 1)));
            detail.FontSize(12);
            detail.MaxLines(2);
            detail.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            detail.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 168, 179, 199}});
            row.Children().Append(detail);
        }
        OperationalList().Items().Append(row);
    }
    OperationalListSummary().Text(winrt::to_hstring(summary.empty()
        ? std::to_string(operationalLines_.size()) + " records from the live Manager projection"
        : summary));
    OperationalCount().Text(winrt::to_hstring(
        std::to_string(operationalLines_.size()) + " LIVE"));
    if (!operationalLines_.empty()) {
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents)
            OperationalCards().SelectedIndex(0);
        else OperationalList().SelectedIndex(0);
    }
    else {
        OperationalDetailTitle().Text(L"No records");
        OperationalDetailBody().Text(L"The Manager returned no entries for this view.");
    }
}

winrt::fire_and_forget MainWindow::RunAction(const Action action)
{
    auto lifetime = get_strong();
    if (!connection_ || cancellation_.stop_requested()) co_return;

    std::optional<::ForgeConductor::Domain::ManagerSettings> submitted;
    const bool runAction = action == Action::RunStart ||
        action == Action::RunStatus || action == Action::RunPause ||
        action == Action::RunResume || action == Action::RunCancel;
    const bool projectAction = action == Action::ProjectList ||
        action == Action::ProjectRegister || action == Action::ProjectLoad ||
        action == Action::ProjectRemember;
    const bool lmStudioAction = action == Action::LmStudioInspect ||
        action == Action::LmStudioRepair || action == Action::LmStudioActivate;
    const bool toolsAction = action == Action::ToolsList || action == Action::ToolInvoke;
    const bool operationalAction = action == Action::OperationalInspect ||
        action == Action::OperationalPrune || action == Action::OperationalClose;
    const bool settingsAction = action == Action::SettingsLoad ||
        action == Action::SettingsSave || action == Action::SettingsTest ||
        action == Action::SettingsRestart;
    const bool maintenanceAction = action == Action::MaintenanceReset;
    std::string runProject;
    std::string runClient;
    std::string runTask;
    std::string runId;
    std::uint64_t runGeneration{};
    std::string projectPath;
    std::string projectDisplayName;
    std::string projectQuery;
    std::string memoryTitle;
    std::string memorySummary;
    std::string memoryBody;
    std::vector<std::string> memoryTags;
    std::string toolProject;
    std::string toolName;
    std::string toolArguments;
    std::string operationalSessionId;
    std::string operationalSummary;
    ::ForgeConductor::Manager::ManagerMaintenanceScope maintenanceScope{
        ::ForgeConductor::Manager::ManagerMaintenanceScope::ProjectMemory};
    std::optional<std::string> maintenanceProject;
    std::string maintenanceToken;
    if (action == Action::Refresh) {
        runId = winrt::to_string(RunId().Text());
    }
    if (action == Action::ProviderSave || action == Action::ProviderTest) {
        std::string error;
        submitted = ReadProviderForm(error);
        if (!submitted) {
            ProviderState().Text(winrt::to_hstring("Check provider settings: " + error));
            co_return;
        }
    }
    if (runAction) {
        runId = winrt::to_string(RunId().Text());
        if (action == Action::RunStart) {
            runProject = selectedProjectId_;
            runClient = "forge-conductor-manager";
            runTask = winrt::to_string(RunTask().Text());
            runGeneration = 0U;
            if (runProject.empty()) {
                RunState().Text(L"Select a named project before starting work.");
                co_return;
            }
        } else if (runId.empty()) {
            RunState().Text(L"Enter a run ID to attach or control a Manager-owned run.");
            co_return;
        }
    }
    if (action == Action::SettingsSave || action == Action::SettingsTest) {
        std::string error;
        submitted = ReadSettingsForm(error);
        if (!submitted) {
            SettingsState().Text(winrt::to_hstring("Check pending settings: " + error));
            co_return;
        }
    }
    if (projectAction) {
        if (action == Action::ProjectRegister) {
            projectPath = winrt::to_string(ProjectPath().Text());
            projectDisplayName = winrt::to_string(ProjectDisplayName().Text());
            if (projectPath.empty()) {
                ProjectState().Text(L"Enter the project folder to authorize.");
                co_return;
            }
        } else if (action == Action::ProjectLoad ||
            action == Action::ProjectRemember) {
            if (selectedProjectId_.empty()) {
                ProjectState().Text(L"Select or register a project first.");
                co_return;
            }
            projectQuery = winrt::to_string(ProjectMemoryQuery().Text());
            if (action == Action::ProjectRemember) {
                memoryTitle = winrt::to_string(ProjectMemoryTitle().Text());
                memorySummary = winrt::to_string(ProjectMemorySummary().Text());
                memoryBody = winrt::to_string(ProjectMemoryBody().Text());
                memoryTags = commaSeparatedTags(
                    winrt::to_string(ProjectMemoryTags().Text()));
                if (memoryTitle.empty() || memorySummary.empty()) {
                    ProjectState().Text(L"Memory title and summary are required.");
                    co_return;
                }
            }
        }
    }
    if (action == Action::ToolInvoke) {
        toolProject = winrt::to_string(ToolProjectId().Text());
        toolName = winrt::to_string(ToolName().Text());
        toolArguments = winrt::to_string(ToolArguments().Text());
        if (toolProject.empty() || toolName.empty() || toolArguments.empty()) {
            ToolsState().Text(L"Project ID, tool name, and JSON arguments are required.");
            co_return;
        }
    }
    if (action == Action::OperationalClose) {
        operationalSessionId = winrt::to_string(OperationalSessionId().Text());
        operationalSummary = winrt::to_string(OperationalSummary().Text());
        if (operationalSessionId.empty()) {
            OperationalState().Text(L"Enter the exact session ID to close.");
            co_return;
        }
    }
    if (maintenanceAction) {
        const auto index = MaintenanceScope().SelectedIndex();
        if (index < 0 || index > 3) {
            MaintenanceState().Text(L"Select a reset scope.");
            co_return;
        }
        maintenanceScope = static_cast<
            ::ForgeConductor::Manager::ManagerMaintenanceScope>(index);
        if (maintenanceScope != ::ForgeConductor::Manager::
                ManagerMaintenanceScope::AllProjectsAllData) {
            if (selectedProjectId_.empty()) {
                MaintenanceState().Text(L"Select the exact project on the Projects page first.");
                co_return;
            }
            maintenanceProject = selectedProjectId_;
        }
        maintenanceToken = winrt::to_string(MaintenanceConfirmation().Text());
        if (maintenanceToken.empty()) {
            MaintenanceState().Text(L"Type the exact confirmation before running a reset.");
            co_return;
        }
    }

    const auto lane = action == Action::Refresh
        ? ::ForgeConductor::Hosts::App::AppActionLane::Observation
        : ::ForgeConductor::Hosts::App::AppActionLane::Command;
    const auto admission = actionScheduler_.admit(
        lane, static_cast<std::size_t>(action));
    if (admission != ::ForgeConductor::Hosts::App::AppActionAdmission::Started) {
        if (admission == ::ForgeConductor::Hosts::App::AppActionAdmission::Queued) {
            const auto queued = L"Queued behind the current Manager command.";
            if (runAction) RunState().Text(queued);
            else if (projectAction) ProjectState().Text(queued);
            else if (lmStudioAction) LmStudioRegistrationState().Text(queued);
            else if (toolsAction) ToolsState().Text(queued);
            else if (operationalAction) OperationalState().Text(queued);
            else if (settingsAction) SettingsState().Text(queued);
            else if (maintenanceAction) MaintenanceState().Text(queued);
            else if (action == Action::ProviderLoad ||
                     action == Action::ProviderSave ||
                     action == Action::ProviderTest) ProviderState().Text(queued);
            else ManagerState().Text(queued);
        } else if (admission ==
                ::ForgeConductor::Hosts::App::AppActionAdmission::Rejected) {
            GenericState().Text(
                L"The bounded Manager command queue is full; this action was not accepted. Retry after the current command completes.");
        }
        co_return;
    }
    winrt::apartment_context ui;
    if (runAction) {
        RunState().Text(L"Contacting the Manager…");
    } else if (projectAction) {
        ProjectState().Text(L"Contacting the Manager…");
    } else if (lmStudioAction) {
        LmStudioRegistrationState().Text(L"Contacting the Manager…");
    } else if (toolsAction) {
        ToolsState().Text(L"Contacting the Manager…");
    } else if (operationalAction) {
        OperationalState().Text(L"Contacting the Manager…");
    } else if (settingsAction) {
        SettingsState().Text(L"Contacting the Manager…");
    } else if (maintenanceAction) {
        MaintenanceState().Text(L"The Manager is fencing the selected data scope…");
    } else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
        action == Action::ProviderTest) {
        ProviderState().Text(L"Working…");
    } else {
        ManagerState().Text(L"Connecting…");
        GenericState().Text(L"Connecting…");
    }

    std::string message;
    ::ForgeConductor::Hosts::App::ProviderSettingsView loaded;
    ::ForgeConductor::Hosts::App::ManagedRunView runView;
    ::ForgeConductor::Hosts::App::TelemetryView telemetryView;
    ::ForgeConductor::Hosts::App::ProjectsView projectsView;
    ::ForgeConductor::Hosts::App::ProjectWorkspaceView projectView;
    ::ForgeConductor::Hosts::App::LmStudioView lmStudioView;
    ::ForgeConductor::Hosts::App::ToolsView toolsView;
    ::ForgeConductor::Hosts::App::ToolOutcomeView toolOutcomeView;
    ::ForgeConductor::Hosts::App::OperationalView operationalView;
    ::ForgeConductor::Hosts::App::MaintenanceView maintenanceView;
    bool failed{};
    try {
        co_await winrt::resume_background();
        switch (action) {
        case Action::Start:
            message = connection_->start(cancellation_.get_token());
            break;
        case Action::Refresh:
            telemetryView = connection_->telemetry(
                std::move(runId), cancellation_.get_token());
            message = telemetryView.message;
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
        case Action::SettingsLoad:
            loaded = connection_->providerSettings(cancellation_.get_token());
            message = loaded.message;
            break;
        case Action::SettingsSave: {
            ::ForgeConductor::Domain::ManagerSettingsPatch patch;
            patch.dashboardHost = submitted->dashboardHost;
            patch.dashboardPort = submitted->dashboardPort;
            patch.dashboardRefreshInterval = submitted->dashboardRefreshInterval;
            patch.autoRestart = submitted->autoRestart;
            patch.watchdogInterval = submitted->watchdogInterval;
            patch.openBrowserOnStart = submitted->openBrowserOnStart;
            patch.sessionIdleTtl = submitted->sessionIdleTtl;
            patch.shellTimeout = submitted->shellTimeout;
            patch.shellEnabled = submitted->shellEnabled;
            patch.logLevel = submitted->logLevel;
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
        case Action::SettingsTest:
            message = connection_->testProvider(
                *submitted, cancellation_.get_token());
            break;
        case Action::SettingsRestart:
            message = connection_->control(
                ::ForgeConductor::Domain::ManagerControlAction::Restart,
                cancellation_.get_token());
            break;
        case Action::MaintenanceReset:
            maintenanceView = connection_->resetData(
                maintenanceScope, std::move(maintenanceProject),
                std::move(maintenanceToken), cancellation_.get_token());
            message = maintenanceView.message;
            break;
        case Action::RunStart:
            runView = connection_->startManagedRun(
                std::move(runProject), std::move(runClient), runGeneration,
                std::move(runTask), cancellation_.get_token());
            message = runView.message;
            break;
        case Action::RunStatus:
        case Action::RunPause:
        case Action::RunResume:
        case Action::RunCancel: {
            using RunAction = ::ForgeConductor::Hosts::App::ManagedRunAction;
            const auto control = action == Action::RunPause ? RunAction::Pause :
                action == Action::RunResume ? RunAction::Resume :
                action == Action::RunCancel ? RunAction::Cancel :
                RunAction::Status;
            runView = connection_->controlManagedRun(
                std::move(runId), control, cancellation_.get_token());
            message = runView.message;
            break;
        }
        case Action::ProjectList:
            projectsView = connection_->projects(cancellation_.get_token());
            message = projectsView.message;
            break;
        case Action::ProjectRegister:
            projectView = connection_->initializeProject(
                std::move(projectPath), std::move(projectDisplayName),
                cancellation_.get_token());
            message = projectView.message;
            break;
        case Action::ProjectLoad:
            projectView = connection_->projectMemory(
                selectedProjectId_, std::move(projectQuery),
                cancellation_.get_token());
            message = projectView.message;
            break;
        case Action::ProjectRemember:
            projectView = connection_->rememberProjectMemory(
                selectedProjectId_, std::move(memoryTitle),
                std::move(memorySummary), std::move(memoryBody),
                std::move(memoryTags), cancellation_.get_token());
            message = projectView.message;
            break;
        case Action::LmStudioInspect:
        case Action::LmStudioRepair:
        case Action::LmStudioActivate: {
            using LmAction = ::ForgeConductor::Hosts::App::LmStudioAction;
            const auto lmAction = action == Action::LmStudioRepair ? LmAction::Repair :
                action == Action::LmStudioActivate ? LmAction::Activate : LmAction::Inspect;
            lmStudioView = connection_->lmStudio(lmAction, cancellation_.get_token());
            message = lmStudioView.message;
            break;
        }
        case Action::ToolsList:
            toolsView = connection_->tools(cancellation_.get_token());
            message = toolsView.message;
            break;
        case Action::ToolInvoke:
            toolOutcomeView = connection_->invokeTool(
                std::move(toolProject), std::move(toolName), std::move(toolArguments),
                cancellation_.get_token());
            message = toolOutcomeView.message;
            break;
        case Action::OperationalInspect:
        case Action::OperationalPrune:
        case Action::OperationalClose: {
            using OpAction = ::ForgeConductor::Manager::ManagerOperationalAction;
            const auto op = action == Action::OperationalPrune ? OpAction::PruneSessions :
                action == Action::OperationalClose ? OpAction::CloseSession : OpAction::Inspect;
            operationalView = connection_->operational(
                operationalArea_, op, std::move(operationalSessionId),
                std::move(operationalSummary), cancellation_.get_token());
            message = operationalView.message;
            break;
        }
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
    std::optional<Action> followUp;
    if (!cancellation_.stop_requested()) {
        if (runAction) {
            if (runView.snapshot) {
                RunId().Text(winrt::to_hstring(
                    runView.snapshot->record.runId.value()));
            }
            RunState().Text(winrt::to_hstring(message));
        } else if (projectAction) {
            if (projectsView.loaded && projectsView.snapshot) {
                ApplyProjectList(*projectsView.snapshot);
                if (!selectedProjectId_.empty()) followUp = Action::ProjectLoad;
            }
            if (projectView.loaded && projectView.snapshot) {
                ApplyProjectWorkspace(*projectView.snapshot);
                if (action == Action::ProjectRegister) {
                    followUp = Action::ProjectList;
                } else if (action == Action::ProjectRemember) {
                    ProjectMemoryTitle().Text(L"");
                    ProjectMemorySummary().Text(L"");
                    ProjectMemoryBody().Text(L"");
                    ProjectMemoryTags().Text(L"");
                }
            }
            ProjectState().Text(winrt::to_hstring(message));
        } else if (lmStudioAction) {
            if (lmStudioView.snapshot) ApplyLmStudio(*lmStudioView.snapshot);
            if (!lmStudioView.loaded) {
                LmStudioRegistrationState().Text(winrt::to_hstring(message));
            }
        } else if (toolsAction) {
            if (toolsView.snapshot) ApplyTools(*toolsView.snapshot);
            ToolsState().Text(winrt::to_hstring(message));
            if (toolOutcomeView.snapshot) {
                ToolOutcome().Text(winrt::to_hstring(
                    message + "\n" + toolOutcomeView.snapshot->canonicalPayload));
            } else if (action == Action::ToolInvoke) {
                ToolOutcome().Text(winrt::to_hstring(message));
            }
        } else if (operationalAction) {
            if (operationalView.snapshot) {
                ApplyOperational(*operationalView.snapshot);
            } else {
                OperationalState().Text(winrt::to_hstring(message));
                OperationalListSummary().Text(winrt::to_hstring(message));
            }
        } else if (settingsAction) {
            if (action == Action::SettingsLoad && loaded.loaded) {
                std::string providerFormError;
                std::string settingsFormError;
                const auto pendingProvider = ReadProviderForm(providerFormError);
                const auto pendingSettings = ReadSettingsForm(settingsFormError);
                const bool providerEdited = !providerSettings_ || !pendingProvider ||
                    *pendingProvider != *providerSettings_;
                const bool settingsEdited = !providerSettings_ || !pendingSettings ||
                    *pendingSettings != *providerSettings_;
                providerSettings_ = loaded.settings;
                if (!providerEdited) ApplyProviderForm(*providerSettings_);
                if (!settingsEdited) ApplySettingsForm(*providerSettings_);
                SettingsState().Text(winrt::to_hstring(
                    "Effective settings read back from the Manager. Dashboard " +
                    loaded.settings.dashboardHost + ":" +
                    std::to_string(loaded.settings.dashboardPort) +
                    "; LM Studio " + loaded.settings.localModelHost + ":" +
                    std::to_string(loaded.settings.localModelPort) +
                    "; model " + (loaded.settings.localModelName.empty()
                        ? std::string{"automatic (first loaded model)"}
                        : loaded.settings.localModelName) + "." +
                    (providerEdited || settingsEdited
                        ? " Pending form edits were preserved; use Revert to replace them."
                        : "")));
            } else {
                SettingsState().Text(winrt::to_hstring(message));
                if (action == Action::SettingsSave ||
                    action == Action::SettingsRestart) {
                    followUp = Action::SettingsLoad;
                }
            }
        } else if (maintenanceAction) {
            MaintenanceState().Text(winrt::to_hstring(message));
            if (maintenanceView.loaded) {
                MaintenanceConfirmation().Text(L"");
                followUp = maintenanceScope == ::ForgeConductor::Manager::
                    ManagerMaintenanceScope::AllProjectsAllData
                    ? Action::ProjectList : Action::ProjectLoad;
            }
        } else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
            action == Action::ProviderTest) {
            if (action == Action::ProviderLoad && loaded.loaded) {
                std::string formError;
                const auto pending = ReadProviderForm(formError);
                const bool edited = !providerSettings_ || !pending ||
                    *pending != *providerSettings_;
                providerSettings_ = loaded.settings;
                if (!edited) {
                    ApplyProviderForm(*providerSettings_);
                } else {
                    message += " Pending provider edits were preserved; reload after saving or reverting.";
                }
            } else if (action == Action::ProviderSave && submitted && !failed) {
                providerSettings_ = submitted;
            }
            ProviderState().Text(winrt::to_hstring(message));
            if (action == Action::ProviderSave) followUp = Action::ProviderLoad;
        } else {
            if (action == Action::Refresh && telemetryView.snapshot) {
                telemetrySnapshot_ = std::move(telemetryView.snapshot);
                ApplyTelemetryPresentation(*telemetrySnapshot_);
            } else if (action == Action::Refresh) {
                ApplyDisconnectedTelemetry(message);
            }
            ManagerState().Text(winrt::to_hstring(message));
            GenericState().Text(winrt::to_hstring(message));
        }
    }
    const auto scheduled = actionScheduler_.complete(lane);
    if (scheduled) RunAction(static_cast<Action>(*scheduled));
    if (action == Action::Start || action == Action::Restart) {
        RunAction(Action::Refresh);
    }
    if (followUp) RunAction(*followUp);
}
}
