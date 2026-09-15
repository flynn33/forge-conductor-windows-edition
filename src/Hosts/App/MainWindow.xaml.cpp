#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
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

[[nodiscard]] std::vector<double> calmHistory(const std::vector<double>& values)
{
    constexpr std::size_t targetPoints = 72U;
    if (values.size() <= targetPoints) return values;
    std::vector<double> result;
    result.reserve(targetPoints);
    for (std::size_t index{}; index < targetPoints; ++index) {
        const auto begin = index * values.size() / targetPoints;
        const auto end = (index + 1U) * values.size() / targetPoints;
        double total{};
        for (auto sample = begin; sample < end; ++sample) total += values[sample];
        result.push_back(total / static_cast<double>(end - begin));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> dotFields(const std::string_view line)
{
    constexpr std::string_view separator{" · "};
    std::vector<std::string> result;
    std::size_t begin{};
    while (begin < line.size()) {
        const auto end = line.find(separator, begin);
        result.emplace_back(line.substr(begin,
            end == std::string_view::npos ? line.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + separator.size();
    }
    return result;
}

[[nodiscard]] hstring eventLocalTime(
    const ::ForgeConductor::Domain::UtcTimePoint timestamp)
{
    const auto instant = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
    if (::localtime_s(&local, &instant) != 0) return L"--:--:--";
    wchar_t text[16]{};
    static_cast<void>(swprintf_s(text, L"%02d:%02d:%02d",
        local.tm_hour, local.tm_min, local.tm_sec));
    return hstring{text};
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

[[nodiscard]] hstring currentDataRoot()
{
    wchar_t path[32768]{};
    const auto length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", path,
        static_cast<DWORD>(std::size(path)));
    if (length == 0U || length >= std::size(path))
        return L"%LOCALAPPDATA%\\Forge Conductor";
    std::wstring result{path};
    result += L"\\Forge Conductor";
    return hstring{result};
}

[[nodiscard]] hstring compactBytes(const std::uint64_t bytes)
{
    if (bytes < 1024U) return winrt::to_hstring(std::to_string(bytes) + " B");
    const auto amount = bytes >= 1024U * 1024U
        ? static_cast<double>(bytes) / (1024.0 * 1024.0)
        : static_cast<double>(bytes) / 1024.0;
    std::ostringstream text;
    text.precision(1);
    text << std::fixed << amount << (bytes >= 1024U * 1024U ? " MB" : " KB");
    return winrt::to_hstring(text.str());
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
    selectedRunValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedRunId", scope);
    selectedRunProjectValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedRunProjectId", scope);
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
        const auto savedRunProject = loadSavedText(
            selectedRunProjectValueName_.c_str());
        const auto savedRun = loadSavedText(selectedRunValueName_.c_str());
        if (savedRunProject && savedRun && *savedRunProject == *savedProject) {
            RunId().Text(*savedRun);
        }
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
void MainWindow::ProviderDiscoverClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderModels); }
void MainWindow::ProviderModelSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    if (rebuildingProviderModels_) return;
    const auto index = ProviderLoadedModels().SelectedIndex();
    if (index < 0) return;
    if (index > 0 && static_cast<std::size_t>(index - 1) >= loadedModels_.size()) return;
    ProviderModel().Text(index == 0 ? L"" :
        winrt::to_hstring(loadedModels_[static_cast<std::size_t>(index - 1)]));
    ProviderState().Text(L"Model selection is pending. Save and read back to make it effective.");
}
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
void MainWindow::OpenAutonomyClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Autonomy"); }
void MainWindow::OpenFeedClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Feed"); }
void MainWindow::OpenSettingsClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Settings"); }
void MainWindow::OpenProjectsClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Projects"); }

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
void MainWindow::RunIdTextChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    if (winrt::to_string(RunId().Text()) == verifiedRunId_) return;
    verifiedRunId_.clear();
    verifiedRunProjectId_.clear();
    RunPauseButton().IsEnabled(false);
    RunResumeButton().IsEnabled(false);
    RunCancelButton().IsEnabled(false);
    RunTokensValue().Text(L"— / — tokens");
    RunPendingCalls().Text(L"No pending tool activity");
    RunOutcomeText().Text(L"Refresh to inspect output from this exact run.");
    if (!RunId().Text().empty()) {
        RunState().Text(L"Unverified run identity · refresh to attach and check its project.");
    }
}
void MainWindow::ProjectRegisterClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectRegister); }
void MainWindow::ProjectBrowseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    try {
        auto nativeWindow = this->m_inner.as<::IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        winrt::com_ptr<::IFileDialog> dialog;
        winrt::check_hresult(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{};
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM));
        winrt::check_hresult(dialog->SetTitle(L"Choose an authorized project folder"));
        const auto shown = dialog->Show(hwnd);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> folder;
        winrt::check_hresult(dialog->GetResult(folder.put()));
        PWSTR path{};
        winrt::check_hresult(folder->GetDisplayName(SIGDN_FILESYSPATH, &path));
        ProjectPath().Text(path);
        ::CoTaskMemFree(path);
        ProjectState().Text(L"Folder chosen. Register it to authorize this work scope.");
    } catch (const winrt::hresult_error& error) {
        ProjectState().Text(L"The Windows folder picker failed: " + error.message());
    }
}
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
    if (index >= 0 && static_cast<std::size_t>(index) < visibleOperationalIndices_.size())
        SelectOperationalRecord(visibleOperationalIndices_[static_cast<std::size_t>(index)]);
}
void MainWindow::OperationalCardSelectionChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = OperationalCards().SelectedIndex();
    if (index >= 0 && static_cast<std::size_t>(index) < visibleOperationalIndices_.size())
        SelectOperationalRecord(visibleOperationalIndices_[static_cast<std::size_t>(index)]);
}
void MainWindow::OperationalAgentSearchChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Agents &&
        operationalSnapshot_) {
        const auto snapshot = *operationalSnapshot_;
        ApplyOperational(snapshot);
    }
}
void MainWindow::OperationalFeedFilterChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Feed &&
        operationalSnapshot_) {
        const auto snapshot = *operationalSnapshot_;
        ApplyOperational(snapshot);
    }
}
void MainWindow::OperationalFeedSeverityChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Feed &&
        operationalSnapshot_) {
        const auto snapshot = *operationalSnapshot_;
        ApplyOperational(snapshot);
    }
}
void MainWindow::OperationalFeedPauseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    feedDisplayPaused_ = !feedDisplayPaused_;
    OperationalFeedPause().Content(box_value(
        feedDisplayPaused_ ? L"Resume display" : L"Pause display"));
    if (!feedDisplayPaused_ && pendingFeedSnapshot_) {
        const auto latest = *pendingFeedSnapshot_;
        pendingFeedSnapshot_.reset();
        ApplyOperational(latest);
    }
}
void MainWindow::SelectOperationalRecord(const std::size_t index)
{
    if (index >= operationalLines_.size()) return;
    const auto& line = operationalLines_[index];
    const auto separator = line.find('\n');
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
        const auto fields = dotFields(std::string_view{line}.substr(0, separator));
        if (fields.size() >= 3U) {
            OperationalDetailTitle().Text(winrt::to_hstring(
                fields[1] + " · " + fields[2]));
            auto detail = "Observed: " + fields[0];
            for (std::size_t field = 3U; field < fields.size(); ++field) {
                detail += fields[field].ends_with(" ms")
                    ? "\nDuration: " + fields[field]
                    : "\nClient identity: " + fields[field];
            }
            if (separator != std::string::npos)
                detail += "\nOutcome detail: " + line.substr(separator + 1);
            OperationalDetailBody().Text(winrt::to_hstring(detail));
            return;
        }
    }
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
    const auto nextProjectId = projects_[static_cast<std::size_t>(index)].id.value();
    if (nextProjectId != selectedProjectId_) ClearSelectedRun();
    selectedProjectId_ = nextProjectId;
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);
    RunProjectId().Text(selected);
    ToolProjectId().Text(selected);
    RunAction(Action::ProjectLoad);
    UpdateRunProjectLabel();
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
            evidence ? L"Audit trail · not durable evidence" :
            runtimes ? L"Resource inventory" :
            diagnostics ? L"Doctor checks" : L"Manager state");
        OperationalHeroIcon().Glyph(agents ? L"\uE716" :
            feed || evidence ? L"\uE8D4" :
            runtimes ? L"\uE7F4" :
            diagnostics ? L"\uE713" : L"\uE77B");
        OperationalPruneButton().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalSessionCard().Visibility(Visibility::Collapsed);
        OperationalAgentSearch().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalFeedFilters().Visibility(feed || evidence ? Visibility::Visible : Visibility::Collapsed);
        OperationalEvidenceCard().Visibility(evidence ? Visibility::Visible : Visibility::Collapsed);
        OperationalStatusGrid().Visibility(runtimes || tag == L"Manager"
            ? Visibility::Visible : Visibility::Collapsed);
        OperationalManagerCard().Visibility(tag == L"Manager" ? Visibility::Visible : Visibility::Collapsed);
        OperationalCards().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalList().Visibility(agents ? Visibility::Collapsed : Visibility::Visible);
        OperationalEmptyTitle().Text(L"Live inventory unavailable");
        OperationalEmptyBody().Text(L"Connect to the Manager and refresh this view.");
        OperationalCount().Text(L"WAITING");
    }
    ProviderPanel().Visibility(provider ? Visibility::Visible : Visibility::Collapsed);
    AutonomyPanel().Visibility(autonomy ? Visibility::Visible : Visibility::Collapsed);
    AutonomyOverviewCard().Visibility(continuityPage ? Visibility::Collapsed : Visibility::Visible);
    ContinuityOverviewCard().Visibility(continuityPage ? Visibility::Visible : Visibility::Collapsed);
    ContinuityMetrics().Visibility(continuityPage ? Visibility::Visible : Visibility::Collapsed);
    ContinuityTimelineCard().Visibility(continuityPage ? Visibility::Visible : Visibility::Collapsed);
    RunMissionCard().Visibility(continuityPage ? Visibility::Collapsed : Visibility::Visible);
    Microsoft::UI::Xaml::Controls::Grid::SetColumn(RunReadbackCard(),
        continuityPage ? 0 : 1);
    Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(RunReadbackCard(),
        continuityPage ? 2 : 1);
    RunTask().Visibility(continuityPage ? Visibility::Collapsed : Visibility::Visible);
    RunStartButton().Visibility(continuityPage ? Visibility::Collapsed : Visibility::Visible);
    UpdateRunProjectLabel();
    RunControlHeading().Text(continuityPage ? L"Attach & control exact run" : L"Mission & run control");
    RunStateHeading().Text(continuityPage ? L"Latest run readback" : L"Live run");
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
        PageDescription().Text(L"Choose a loaded model and inspect the Manager-owned Responses endpoint.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
        if (telemetryUiInitialized_ && loadedModels_.empty()) RunAction(Action::ProviderModels);
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

void MainWindow::TelemetryGaugeSizeChanged(
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
    rebuildingProviderModels_ = true;
    auto modelIndex = 0;
    for (std::size_t index{}; index < loadedModels_.size(); ++index) {
        if (loadedModels_[index] == settings.localModelName) {
            modelIndex = static_cast<int>(index + 1U);
            break;
        }
    }
    ProviderLoadedModels().SelectedIndex(modelIndex);
    rebuildingProviderModels_ = false;
    if (!settings.localModelName.empty() && modelIndex == 0) {
        ProviderModelsState().Text(L"Saved model ID is not in the current discovery list. Discover to verify it.");
    }
    ContextCapacity().Value(settings.effectiveContextCapacity);
    ProviderCapacityValue().Text(winrt::to_hstring(
        std::to_string(settings.effectiveContextCapacity)));
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
    HistoryCpuLegend().Text(winrt::to_hstring("CPU " + presentation.cpu.value));
    HistoryRamLegend().Text(winrt::to_hstring("RAM " + presentation.ram.value));
    HistoryGpuLegend().Text(winrt::to_hstring("GPU " + presentation.gpu.value));
    const auto updateFill = [](const Microsoft::UI::Xaml::Controls::Border& track,
                               const Microsoft::UI::Xaml::Controls::Border& fill,
                               const ::ForgeConductor::Hosts::App::MetricPresentation& metric) {
        fill.Width(metric.gaugePercent
            ? track.ActualWidth() * std::clamp(*metric.gaugePercent, 0.0, 100.0) / 100.0
            : 0.0);
    };
    updateFill(CpuGaugeTrack(), CpuGaugeFill(), presentation.cpu);
    updateFill(RamGaugeTrack(), RamGaugeFill(), presentation.ram);
    updateFill(GpuGaugeTrack(), GpuGaugeFill(), presentation.gpu);
    updateFill(ContextGaugeTrack(), ContextGaugeFill(), presentation.context);
    ContinuityContextState().Text(winrt::to_hstring(
        presentation.context.value + " · " + presentation.context.state));
    ContinuityCapacityValue().Text(winrt::to_hstring(
        std::to_string(snapshot.context.capacityTokens)));
    ContinuityResponseReserveValue().Text(winrt::to_hstring(
        std::to_string(snapshot.context.nextResponseReserveTokens)));
    ContinuityHandoffReserveValue().Text(winrt::to_hstring(
        std::to_string(snapshot.context.handoffReserveTokens)));
    ContinuityRetainedValue().Text(winrt::to_hstring(
        snapshot.context.authoritative && snapshot.context.retainedTokens
            ? std::to_string(*snapshot.context.retainedTokens)
            : std::string{"No run"}));
    ContinuitySourceState().Text(winrt::to_hstring(
        snapshot.continuity.runId
            ? "Run " + snapshot.continuity.runId->value()
            : std::string{"Awaiting an exact run identity"}));
    ContinuityPolicyState().Text(winrt::to_hstring(
        std::string{snapshot.continuity.contextOnly ? "Context-only" : "Broader handoff"} +
        (snapshot.continuity.managerOwned ? " · Manager-owned" : " · ownership unavailable")));
    ContinuityRestorationState().Text(winrt::to_hstring(
        snapshot.continuity.canonicalResponseId
            ? "Canonical response " + snapshot.continuity.canonicalResponseId->value() +
                " · no verified successor projected"
            : std::string{"No verified successor projected"}));
    RunSelectedModel().Text(snapshot.provider.model
        ? winrt::to_hstring(*snapshot.provider.model)
        : L"Automatic · first compatible loaded model");
    if (snapshot.selectedRun && !selectedProjectId_.empty() &&
        snapshot.selectedRun->record.projectId.value() == selectedProjectId_ &&
        (RunId().Text().empty() || winrt::to_string(RunId().Text()) ==
            snapshot.selectedRun->record.runId.value())) {
        ApplyRunReadback(*snapshot.selectedRun);
    } else if (snapshot.selectedRun) {
        RunTelemetryState().Text(
            L"This run is not bound to the selected project. Control is disabled.");
        RunStatusDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 255, 200, 87}});
    } else {
        RunTelemetryState().Text(L"No Manager-owned run is currently attached.");
        RunStatusDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 255, 200, 87}});
    }
    ManagerHealth().Text(winrt::to_hstring(
        snapshot.manager.serviceActive
            ? "PID " + std::to_string(snapshot.manager.processId) +
                " · Native Manager online"
            : "Native Manager unavailable"));
    HeroManagerLabel().Text(snapshot.manager.serviceActive
        ? L"Service active" : L"Service unavailable");
    ProviderHealth().Text(winrt::to_hstring(presentation.providerStatus));
    const auto providerEndpoint = std::string{snapshot.provider.secure ? "https://" : "http://"} +
        snapshot.provider.host + ':' + std::to_string(snapshot.provider.port);
    ProviderActiveEndpoint().Text(winrt::to_hstring(providerEndpoint + " · Manager readback"));
    ProviderActiveModel().Text(snapshot.provider.model
        ? winrt::to_hstring(*snapshot.provider.model)
        : L"Automatic · first compatible loaded model");
    ProviderConnectionBadge().Text(providerEndpoint == providerDiscoveredEndpoint_
        ? L"DISCOVERED" : L"CONFIGURED");
    ProviderConnectionBadge().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
        providerEndpoint == providerDiscoveredEndpoint_
            ? Windows::UI::Color{255, 61, 220, 151}
            : Windows::UI::Color{255, 43, 168, 255}});
    StoreHealth().Text(winrt::to_hstring(presentation.storeStatus));
    ContinuityHealth().Text(winrt::to_hstring(presentation.continuityStatus));
    SystemStrip().Text(winrt::to_hstring(snapshot.resources.host));
    HeroOsText().Text(winrt::to_hstring(
        snapshot.resources.platform + " · " + snapshot.resources.architecture));
    SamplingStrip().Text(winrt::to_hstring(presentation.samplingStatus));
    DiskState().Text(winrt::to_hstring(presentation.diskStatus));
    WorkflowStatus().Text(winrt::to_hstring(presentation.managerStatus));
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
    CpuHistoryLine().Points(chartPoints(calmHistory(presentation.cpuHistory), width, height));
    RamHistoryLine().Points(chartPoints(calmHistory(presentation.ramHistory), width, height));
    GpuHistoryLine().Points(chartPoints(calmHistory(presentation.gpuHistory), width, height));
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
    const auto appendEvent = [this](const hstring& time,
                                    const std::string& label,
                                    const bool failed) {
        Microsoft::UI::Xaml::Controls::StackPanel row;
        row.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
        row.Spacing(9);
        Microsoft::UI::Xaml::Controls::TextBlock timestamp;
        timestamp.Text(time);
        timestamp.Width(76);
        timestamp.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 153, 173, 196}});
        row.Children().Append(timestamp);
        Microsoft::UI::Xaml::Shapes::Ellipse dot;
        dot.Width(7);
        dot.Height(7);
        dot.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
        dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
            failed ? Windows::UI::Color{255, 255, 115, 113}
                : Windows::UI::Color{255, 61, 220, 151}});
        row.Children().Append(dot);
        Microsoft::UI::Xaml::Controls::TextBlock outcome;
        outcome.Text(winrt::to_hstring(label));
        outcome.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
        outcome.Width(510);
        row.Children().Append(outcome);
        ActivityTimeline().Children().Append(row);
    };
    const auto sampleTime = eventLocalTime(snapshot.capturedAt);
    appendEvent(sampleTime,
        snapshot.manager.serviceActive
            ? "Native Manager active · PID " + std::to_string(snapshot.manager.processId)
            : "Native Manager unavailable",
        !snapshot.manager.serviceActive);
    if (snapshot.storeHealthy.value) {
        appendEvent(sampleTime,
            *snapshot.storeHealthy.value
                ? "Operational store health check succeeded"
                : "Operational store health check failed",
            !*snapshot.storeHealthy.value);
    }
    appendEvent(sampleTime,
        "Provider endpoint configured · " +
            std::string{snapshot.provider.secure ? "HTTPS " : "HTTP "} +
            snapshot.provider.host + ":" + std::to_string(snapshot.provider.port),
        false);
    for (const auto& event : snapshot.recentEvents) {
        auto label = event.tool + " · " + event.status;
        if (event.duration) label += " · " +
            std::to_string(event.duration->count()) + " ms";
        appendEvent(eventLocalTime(event.timestamp), label,
            event.error.has_value() || event.status == "error");
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

void MainWindow::ApplyRunReadback(
    const ::ForgeConductor::Domain::ManagedRunSnapshot& snapshot)
{
    const auto& run = snapshot.record;
    if (selectedProjectId_.empty() || run.projectId.value() != selectedProjectId_)
        return;
    using ::ForgeConductor::Domain::ManagedRunState;
    const auto state = [&] {
        switch (run.state) {
        case ManagedRunState::Running: return L"RUNNING";
        case ManagedRunState::Cancelling: return L"STOPPING";
        case ManagedRunState::Completed: return L"COMPLETED";
        case ManagedRunState::Failed: return L"FAILED";
        case ManagedRunState::Cancelled: return L"STOPPED";
        case ManagedRunState::Paused: return L"PAUSED";
        }
        return L"UNKNOWN";
    }();
    const auto runId = winrt::to_hstring(run.runId.value());
    if (RunId().Text() != runId) RunId().Text(runId);
    const bool newlyVerified = verifiedRunId_ != run.runId.value() ||
        verifiedRunProjectId_ != run.projectId.value();
    verifiedRunId_ = run.runId.value();
    verifiedRunProjectId_ = run.projectId.value();
    if (newlyVerified) {
        storeSavedText(selectedRunValueName_.c_str(), runId);
        storeSavedText(selectedRunProjectValueName_.c_str(),
            winrt::to_hstring(verifiedRunProjectId_));
    }
    RunState().Text(hstring{state} +
        (snapshot.pauseRequested ? L" · pause requested" : L" · Manager-owned"));
    RunTelemetryState().Text(winrt::to_hstring(
        std::string{"Exact run verified for this project · "} +
        (run.providerResponseId ? "provider response observed" :
            "waiting for provider response")));
    RunTokensValue().Text(winrt::to_hstring(
        std::to_string(run.inputTokens) + " / " +
        std::to_string(run.outputTokens) + " tokens"));
    RunPendingCalls().Text(winrt::to_hstring(
        run.pendingFunctionCalls.empty()
            ? std::string{"No pending tool activity"}
            : std::to_string(run.pendingFunctionCalls.size()) +
                " pending provider tool call" +
                (run.pendingFunctionCalls.size() == 1U ? "" : "s")));
    RunOutcomeText().Text(winrt::to_hstring(
        run.lastError ? "Error: " + run.lastError->message :
        run.outputText && !run.outputText->empty() ? *run.outputText :
        std::string{"No model output from this run yet."}));
    const bool running = run.state == ManagedRunState::Running;
    const bool paused = run.state == ManagedRunState::Paused;
    RunPauseButton().IsEnabled(running);
    RunResumeButton().IsEnabled(paused);
    RunCancelButton().IsEnabled(running || paused);
    RunStatusDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
        run.state == ManagedRunState::Failed || run.state == ManagedRunState::Cancelled
            ? Windows::UI::Color{255, 255, 115, 113}
            : run.state == ManagedRunState::Cancelling
                ? Windows::UI::Color{255, 255, 200, 87}
                : Windows::UI::Color{255, 61, 220, 151}});
    std::string exact = "Run ID: " + run.runId.value() +
        "\nProject ID: " + run.projectId.value() +
        "\nAuthority generation: " +
            std::to_string(run.authorityGeneration) +
        "\nClient ID: " + run.clientId.value() +
        "\nManager owned: " + (snapshot.managerOwned ? "yes" : "no");
    if (run.providerResponseId) exact +=
        "\nCanonical provider response: " + run.providerResponseId->value();
    for (const auto& call : run.pendingFunctionCalls)
        exact += "\nPending call: " + call.name + " · " + call.callId;
    RunRawDetail().Text(winrt::to_hstring(exact));
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
            project.displayName)));
        if (project.id.value() == selectedProjectId_) {
            selectedIndex = static_cast<std::int32_t>(index);
        }
    }
    ProjectSelector().SelectedIndex(selectedIndex);
    rebuildingProjects_ = false;

    if (selectedIndex >= 0) {
        const auto& project = projects_[static_cast<std::size_t>(selectedIndex)];
        selectedProjectId_ = project.id.value();
        const auto selected = winrt::to_hstring(selectedProjectId_);
        storeSavedText(selectedProjectValueName_.c_str(), selected);
        RunProjectId().Text(selected);
        ToolProjectId().Text(selected);
        ProjectHeroName().Text(winrt::to_hstring(project.displayName));
        ProjectHeroScope().Text(project.aliases.empty()
            ? L"Authorized folder pending"
            : winrt::to_hstring(project.aliases.front().value()));
    } else {
        ClearSelectedProject();
        ProjectHeroName().Text(L"Choose an authorized project");
        ProjectHeroScope().Text(L"No project selected");
        ProjectRecordCount().Text(L"—");
        ProjectEventCount().Text(L"—");
        ProjectStoreSize().Text(L"—");
        ProjectIntegrityValue().Text(L"PENDING");
        ProjectIntegrityValue().Foreground(
            Microsoft::UI::Xaml::Media::SolidColorBrush(Windows::UI::Color{255,255,200,87}));
        ProjectIdentity().Text(L"No registered project is selected.");
        ProjectFolders().Text(L"Register an authorized folder to begin.");
        ProjectPersistence().Text(L"No project memory store is active.");
        ProjectMemoryRecords().Children().Clear();
    }
}

void MainWindow::UpdateRunProjectLabel()
{
    if (selectedProjectId_.empty()) {
        RunSelectedProject().Text(
            L"No project selected · choose an authorized project before starting");
        RunControlProjectLabel().Text(L"Select a project to bind run control.");
        return;
    }
    for (const auto& project : projects_) {
        if (project.id.value() == selectedProjectId_) {
            RunSelectedProject().Text(winrt::to_hstring(
                project.displayName));
            RunControlProjectLabel().Text(winrt::to_hstring(
                "Bound to " + project.displayName + " · the Manager verifies an exact run before control."));
            return;
        }
    }
    RunSelectedProject().Text(L"Loading selected project readback");
    RunControlProjectLabel().Text(L"Loading exact project binding from the Manager.");
}

void MainWindow::ClearSelectedRun()
{
    verifiedRunId_.clear();
    verifiedRunProjectId_.clear();
    clearSavedText(selectedRunValueName_.c_str());
    clearSavedText(selectedRunProjectValueName_.c_str());
    RunId().Text(L"");
    RunPauseButton().IsEnabled(false);
    RunResumeButton().IsEnabled(false);
    RunCancelButton().IsEnabled(false);
    RunState().Text(L"No Manager-owned run is currently attached.");
    RunTokensValue().Text(L"— / — tokens");
    RunPendingCalls().Text(L"No pending tool activity");
    RunOutcomeText().Text(L"No output from an attached run.");
    RunRawDetail().Text(L"No exact run detail has been read.");
}

void MainWindow::ClearSelectedProject()
{
    ClearSelectedRun();
    selectedProjectId_.clear();
    clearSavedText(selectedProjectValueName_.c_str());
    RunProjectId().Text(L"");
    ToolProjectId().Text(L"");
    UpdateRunProjectLabel();
    MaintenanceState().Text(
        L"Select the exact project on the Projects page first.\n"
        L"All registered project data: RESET ALL PROJECT DATA");
}

void MainWindow::ApplyProjectWorkspace(
    const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot)
{
    if (!selectedProjectId_.empty() && selectedProjectId_ != snapshot.project.id.value())
        ClearSelectedRun();
    selectedProjectId_ = snapshot.project.id.value();
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);
    RunProjectId().Text(selected);
    ToolProjectId().Text(selected);
    UpdateRunProjectLabel();
    ProjectHeroName().Text(winrt::to_hstring(snapshot.project.displayName));
    ProjectHeroScope().Text(snapshot.project.aliases.empty()
        ? L"Authorized folder pending"
        : winrt::to_hstring(snapshot.project.aliases.front().value()));
    ProjectRecordCount().Text(winrt::to_hstring(std::to_string(snapshot.recordCount)));
    ProjectEventCount().Text(winrt::to_hstring(std::to_string(snapshot.eventCount)));
    ProjectStoreSize().Text(compactBytes(snapshot.databaseBytes));
    ProjectIntegrityValue().Text(snapshot.integrityOk ? L"VERIFIED" : L"ATTENTION");
    ProjectIntegrityValue().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
        snapshot.integrityOk ? Windows::UI::Color{255,61,220,151}
            : Windows::UI::Color{255,255,200,87}));

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
        empty.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255,184,197,211}));
        ProjectMemoryRecords().Children().Append(empty);
    }
    for (const auto& record : snapshot.records) {
        Microsoft::UI::Xaml::Controls::StackPanel content;
        content.Spacing(6);
        Microsoft::UI::Xaml::Controls::TextBlock title;
        title.Text(winrt::to_hstring(record.title));
        title.FontSize(16);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        content.Children().Append(title);
        Microsoft::UI::Xaml::Controls::TextBlock summary;
        summary.Text(winrt::to_hstring(record.summary));
        summary.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255,184,197,211}));
        summary.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        content.Children().Append(summary);
        std::string metadata = record.kind + " · v" +
            std::to_string(record.version);
        if (!record.tags.empty()) {
            metadata += " · ";
            for (std::size_t index{}; index < record.tags.size(); ++index) {
                if (index != 0U) metadata += ", ";
                metadata += record.tags[index];
            }
        }
        Microsoft::UI::Xaml::Controls::TextBlock meta;
        meta.Text(winrt::to_hstring(metadata));
        meta.FontSize(12);
        meta.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255,43,168,255}));
        content.Children().Append(meta);
        Microsoft::UI::Xaml::Controls::Expander detail;
        detail.Header(box_value(L"Record detail & identity"));
        Microsoft::UI::Xaml::Controls::TextBlock body;
        body.Text(winrt::to_hstring((record.body ? *record.body : std::string{}) +
            "\n\nExact record ID: " + record.id.value()));
        body.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        body.IsTextSelectionEnabled(true);
        detail.Content(body);
        content.Children().Append(detail);
        Microsoft::UI::Xaml::Controls::Border row;
        row.Padding(Microsoft::UI::Xaml::Thickness{12});
        row.CornerRadius(Microsoft::UI::Xaml::CornerRadius{9});
        row.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
        row.Background(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255,13,27,42}));
        row.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{90,102,128,153}));
        row.Child(content);
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
    HistoryCpuLegend().Text(L"CPU unavailable");
    HistoryRamLegend().Text(L"RAM unavailable");
    HistoryGpuLegend().Text(L"GPU unavailable");
    CpuGaugeFill().Width(0);
    RamGaugeFill().Width(0);
    GpuGaugeFill().Width(0);
    ContextGaugeFill().Width(0);
    ContinuityCapacityValue().Text(L"Unavailable");
    ContinuityResponseReserveValue().Text(L"Unavailable");
    ContinuityHandoffReserveValue().Text(L"Unavailable");
    ContinuityRetainedValue().Text(L"Unavailable");
    ContinuitySourceState().Text(L"Manager connection unavailable");
    ContinuityPolicyState().Text(L"Manager-owned policy unavailable");
    ContinuityRestorationState().Text(L"No verified successor projected");
    RunTelemetryState().Text(L"Manager run readback unavailable while disconnected.");
    RunStatusDot().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 255, 200, 87}});
    ManagerHealth().Text(winrt::to_hstring("Disconnected · " + explanation));
    HeroManagerLabel().Text(L"Service unavailable");
    ProviderHealth().Text(L"Unavailable while Manager is disconnected");
    ProviderActiveEndpoint().Text(L"Manager readback unavailable · endpoint not verified");
    ProviderActiveModel().Text(L"Manager readback unavailable");
    ProviderConnectionBadge().Text(L"UNAVAILABLE");
    ProviderConnectionBadge().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 255, 200, 87}});
    StoreHealth().Text(L"Unavailable while Manager is disconnected");
    ContinuityHealth().Text(L"Unavailable while Manager is disconnected");
    SystemStrip().Text(L"System telemetry disconnected");
    HeroOsText().Text(L"Waiting for native host identity");
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
    ToolEmptyState().Visibility(visibleTools_.empty()
        ? Visibility::Visible : Visibility::Collapsed);
    ToolEmptyTitle().Text(tools_.empty() ? L"Catalog unavailable" : L"No matching tools");
    ToolEmptyBody().Text(tools_.empty()
        ? L"Connect to the Manager and reload its registered capabilities."
        : L"Try another name, capability, or pack filter.");
    if (!visibleTools_.empty()) ToolList().SelectedIndex(0);
}

void MainWindow::ApplyOperational(
    const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot)
{
    operationalSnapshot_ = snapshot;
    operationalLines_.clear();
    visibleOperationalIndices_.clear();
    OperationalList().Items().Clear();
    OperationalCards().Items().Clear();
    OperationalState().Text(winrt::to_hstring(snapshot.title));
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Manager ||
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes) {
        const auto valueAfter = [&snapshot](const std::string_view prefix) {
            for (const auto& line : snapshot.lines)
                if (line.starts_with(prefix)) return line.substr(prefix.size());
            return std::string{"—"};
        };
        const bool manager = snapshot.area ==
            ::ForgeConductor::Manager::ManagerOperationalArea::Manager;
        OperationalStatusLabel0().Text(manager ? L"Manager process" : L"Owned operations");
        OperationalStatusLabel1().Text(manager ? L"Product version" : L"Background threads");
        OperationalStatusLabel2().Text(manager ? L"Owned operations" : L"Child processes");
        OperationalStatusLabel3().Text(manager ? L"Data root" : L"Open stores");
        if (manager) {
            const auto pid = valueAfter("Manager PID ");
            OperationalStatusValue0().Text(winrt::to_hstring(pid.substr(0, pid.find(" · "))));
            OperationalStatusValue1().Text(winrt::to_hstring(
                std::string{::ForgeConductor::Domain::ProductVersion}));
            OperationalStatusValue2().Text(winrt::to_hstring(valueAfter("Owned operations: ")));
            OperationalStatusValue3().Text(currentDataRoot());
        } else {
            OperationalStatusValue0().Text(winrt::to_hstring(valueAfter("Owned operations: ")));
            OperationalStatusValue1().Text(winrt::to_hstring(valueAfter("Background threads: ")));
            OperationalStatusValue2().Text(winrt::to_hstring(valueAfter("Child processes: ")));
            OperationalStatusValue3().Text(winrt::to_hstring(
                valueAfter("Open repositories/databases: ")));
        }
    }
    std::string summary;
    bool hasOpenSessions{};
    auto query = winrt::to_string(OperationalAgentSearch().Text());
    std::transform(query.begin(), query.end(), query.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    auto feedQuery = winrt::to_string(OperationalFeedSearch().Text());
    std::transform(feedQuery.begin(), feedQuery.end(), feedQuery.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const auto statusFilter = OperationalFeedSeverity().SelectedIndex();
    for (const auto& line : snapshot.lines) {
        auto text = winrt::to_string(OperationalState().Text());
        OperationalState().Text(winrt::to_hstring(text + "\n\n" + line));
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents &&
            (line.starts_with("Agent definitions:") ||
             line.starts_with("Open sessions:") ||
             line.starts_with("Recent sessions:"))) {
            if (line.starts_with("Open sessions: "))
                hasOpenSessions = line != "Open sessions: 0";
            if (!summary.empty()) summary += "  ·  ";
            summary += line;
            continue;
        }
        operationalLines_.push_back(line);
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents &&
            !query.empty()) {
            auto searchable = line;
            std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
            if (searchable.find(query) == std::string::npos) continue;
        }
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
            const auto headerEnd = line.find('\n');
            const auto fields = dotFields(std::string_view{line}.substr(0, headerEnd));
            const auto status = fields.size() >= 3U ? fields[2] : std::string{};
            if ((statusFilter == 1 && status != "error") ||
                (statusFilter == 2 && status != "denied") ||
                (statusFilter == 3 && status != "ok" && status != "success"))
                continue;
            if (!feedQuery.empty()) {
                auto searchable = line;
                std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                    [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
                if (searchable.find(feedQuery) == std::string::npos) continue;
            }
        }
        visibleOperationalIndices_.push_back(operationalLines_.size() - 1U);
        const auto separator = line.find('\n');
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents) {
            Microsoft::UI::Xaml::Controls::StackPanel cardContent;
            cardContent.Spacing(12);
            const auto body = separator == std::string::npos
                ? std::string{"Manager-owned session or inventory entry"}
                : line.substr(separator + 1);
            const auto toolsMarker = body.rfind("\nTools: ");
            const auto isPlaybook = toolsMarker != std::string::npos;
            const auto availableWidth = OperationalCards().ActualWidth();
            const auto proposedWidth = availableWidth <= 0.0 ? 390.0
                : availableWidth >= 600.0
                    ? (availableWidth - 38.0) / 2.0
                    : availableWidth - 24.0;
            const auto cardWidth = std::clamp(proposedWidth, 240.0, 410.0);
            Microsoft::UI::Xaml::Controls::StackPanel heading;
            heading.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
            heading.Spacing(11);
            Microsoft::UI::Xaml::Controls::Border iconChip;
            iconChip.Width(34);
            iconChip.Height(34);
            iconChip.CornerRadius(Microsoft::UI::Xaml::CornerRadius{9});
            iconChip.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 25, 67, 108}});
            Microsoft::UI::Xaml::Controls::FontIcon icon;
            icon.Glyph(isPlaybook ? L"\uE716" : L"\uE8A7");
            icon.FontSize(17);
            icon.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 84, 189, 255}});
            icon.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
            icon.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            iconChip.Child(icon);
            heading.Children().Append(iconChip);
            Microsoft::UI::Xaml::Controls::TextBlock name;
            name.Text(winrt::to_hstring(line.substr(0, separator)));
            name.FontSize(17);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            name.MaxWidth(cardWidth - 87.0);
            name.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            heading.Children().Append(name);
            cardContent.Children().Append(heading);
            Microsoft::UI::Xaml::Controls::TextBlock description;
            description.Text(winrt::to_hstring(isPlaybook
                ? body.substr(0, toolsMarker) : body));
            description.FontSize(13);
            description.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            description.MaxLines(3);
            description.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            description.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 183, 199, 216}});
            cardContent.Children().Append(description);
            Microsoft::UI::Xaml::Controls::TextBlock coverage;
            coverage.Text(winrt::to_hstring(isPlaybook
                ? "MANAGER PLAYBOOK  ·  " + body.substr(toolsMarker + 1)
                : "LIVE SESSION  ·  exact identity in detail"));
            coverage.FontSize(11);
            coverage.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            coverage.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 84, 189, 255}});
            cardContent.Children().Append(coverage);
            Microsoft::UI::Xaml::Controls::Border card;
            card.Width(cardWidth);
            card.MinHeight(160);
            card.Padding(Microsoft::UI::Xaml::Thickness{18, 18, 18, 18});
            card.CornerRadius(Microsoft::UI::Xaml::CornerRadius{12});
            card.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 20, 35, 52}});
            card.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 59, 88, 119}});
            card.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
            card.Child(cardContent);
            OperationalCards().Items().Append(card);
            continue;
        }
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
            const auto fields = dotFields(std::string_view{line}.substr(0, separator));
            if (fields.size() >= 3U) {
                const auto failed = fields[2] == "error";
                const auto denied = fields[2] == "denied";
                const auto color = failed ? Windows::UI::Color{255, 255, 115, 113}
                    : denied ? Windows::UI::Color{255, 255, 200, 87}
                    : Windows::UI::Color{255, 61, 220, 151};
                Microsoft::UI::Xaml::Controls::StackPanel row;
                row.Spacing(5);
                row.Padding(Microsoft::UI::Xaml::Thickness{13, 11, 13, 11});
                Microsoft::UI::Xaml::Controls::StackPanel heading;
                heading.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                heading.Spacing(10);
                Microsoft::UI::Xaml::Controls::TextBlock timestamp;
                timestamp.Text(winrt::to_hstring(fields[0]));
                timestamp.Width(183);
                timestamp.FontSize(12);
                timestamp.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                    Windows::UI::Color{255, 153, 173, 196}});
                heading.Children().Append(timestamp);
                Microsoft::UI::Xaml::Shapes::Ellipse dot;
                dot.Width(8);
                dot.Height(8);
                dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{color});
                dot.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
                heading.Children().Append(dot);
                Microsoft::UI::Xaml::Controls::TextBlock tool;
                tool.Text(winrt::to_hstring(fields[1]));
                tool.FontSize(15);
                tool.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                tool.Width(180);
                tool.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
                heading.Children().Append(tool);
                Microsoft::UI::Xaml::Controls::TextBlock status;
                status.Text(winrt::to_hstring(fields[2]));
                status.FontSize(12);
                status.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                status.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{color});
                status.Width(54);
                heading.Children().Append(status);
                if (fields.back().ends_with(" ms")) {
                    Microsoft::UI::Xaml::Controls::TextBlock duration;
                    duration.Text(winrt::to_hstring(fields.back()));
                    duration.FontSize(12);
                    duration.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                        Windows::UI::Color{255, 153, 173, 196}});
                    heading.Children().Append(duration);
                }
                row.Children().Append(heading);
                if (separator != std::string::npos) {
                    Microsoft::UI::Xaml::Controls::TextBlock error;
                    error.Text(winrt::to_hstring(line.substr(separator + 1)));
                    error.FontSize(12);
                    error.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                        Windows::UI::Color{255, 153, 173, 196}});
                    error.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
                    error.MaxLines(1);
                    row.Children().Append(error);
                }
                OperationalList().Items().Append(row);
                continue;
            }
        }
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Diagnostics &&
            (line.starts_with("PASS ") || line.starts_with("FAIL "))) {
            const auto passed = line.starts_with("PASS ");
            const auto check = line.substr(5);
            const auto detailMarker = check.find(" — ");
            Microsoft::UI::Xaml::Controls::StackPanel row;
            row.Spacing(5);
            row.Padding(Microsoft::UI::Xaml::Thickness{13, 11, 13, 11});
            Microsoft::UI::Xaml::Controls::StackPanel heading;
            heading.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
            heading.Spacing(10);
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(8);
            dot.Height(8);
            dot.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
            dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{
                passed ? Windows::UI::Color{255, 61, 220, 151}
                    : Windows::UI::Color{255, 255, 115, 113}});
            heading.Children().Append(dot);
            Microsoft::UI::Xaml::Controls::TextBlock name;
            name.Text(winrt::to_hstring(check.substr(0, detailMarker)));
            name.FontSize(15);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            heading.Children().Append(name);
            row.Children().Append(heading);
            if (detailMarker != std::string::npos) {
                Microsoft::UI::Xaml::Controls::TextBlock detail;
                detail.Text(winrt::to_hstring(check.substr(detailMarker + 5)));
                detail.FontSize(12);
                detail.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                    Windows::UI::Color{255, 153, 173, 196}});
                row.Children().Append(detail);
            }
            OperationalList().Items().Append(row);
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
    if (!summary.empty() && !query.empty()) summary += "  ·  Showing " +
        std::to_string(visibleOperationalIndices_.size()) + " of " +
        std::to_string(operationalLines_.size());
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed)
        summary = std::to_string(visibleOperationalIndices_.size()) + " of " +
            std::to_string(operationalLines_.size()) +
            " bounded Manager audit outcomes · newest first" +
            (feedDisplayPaused_ ? " · display paused" : "");
    OperationalListSummary().Text(winrt::to_hstring(summary.empty()
        ? std::to_string(operationalLines_.size()) + " records from the live Manager projection"
        : summary));
    OperationalCount().Text(winrt::to_hstring(
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? winrt::to_string(PageTitle().Text()) == "Events & Evidence"
                ? std::string{"AUDIT ONLY"}
                : std::to_string(visibleOperationalIndices_.size()) + " AUDIT"
            : std::to_string(visibleOperationalIndices_.size()) + " LIVE"));
    OperationalSessionCard().Visibility(
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents &&
            hasOpenSessions ? Visibility::Visible : Visibility::Collapsed);
    OperationalEmptyState().Visibility(visibleOperationalIndices_.empty()
        ? Visibility::Visible : Visibility::Collapsed);
    const auto filtered = snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
        ? !feedQuery.empty() || statusFilter != 0 : !query.empty();
    OperationalEmptyTitle().Text(!filtered ? L"No records yet" :
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? L"No matching audit outcomes" : L"No matching specialists");
    OperationalEmptyBody().Text(!filtered
        ? L"The Manager returned no entries for this view. Refresh to check again."
        : snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? L"Try another tool, client, or severity filter."
            : L"Try a different playbook, tool, or session search.");
    if (!visibleOperationalIndices_.empty()) {
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents)
            OperationalCards().SelectedIndex(0);
        else OperationalList().SelectedIndex(0);
    }
    else {
        OperationalDetailTitle().Text(L"No records");
        OperationalDetailBody().Text(!filtered
            ? L"The Manager returned no entries for this view."
            : snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
                ? L"No audited outcome matches the active filters."
                : L"No specialist or session matches the search.");
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
    if (action == Action::ProviderSave || action == Action::ProviderTest ||
        action == Action::ProviderModels) {
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
            if (runTask.empty()) {
                RunState().Text(L"Describe a mission for the selected project first.");
                co_return;
            }
        } else if (runId.empty()) {
            RunState().Text(L"Enter a run ID to attach or control a Manager-owned run.");
            co_return;
        } else if (selectedProjectId_.empty()) {
            RunState().Text(L"Select the project before attaching an exact run.");
            co_return;
        } else if (action != Action::RunStatus &&
            (runId != verifiedRunId_ || selectedProjectId_ != verifiedRunProjectId_)) {
            RunState().Text(
                L"Refresh this exact run first. The Manager must verify its project before control.");
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
                      action == Action::ProviderTest ||
                      action == Action::ProviderModels) ProviderState().Text(queued);
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
        action == Action::ProviderTest || action == Action::ProviderModels) {
        ProviderState().Text(L"Working…");
    } else {
        ManagerState().Text(L"Connecting…");
        GenericState().Text(L"Connecting…");
    }

    std::string message;
    ::ForgeConductor::Hosts::App::ProviderSettingsView loaded;
    ::ForgeConductor::Hosts::App::ProviderModelsView modelsView;
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
        case Action::ProviderModels:
            modelsView = connection_->providerModels(
                *submitted, cancellation_.get_token());
            message = modelsView.message;
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
                if (runView.snapshot->record.projectId.value() == selectedProjectId_) {
                    ApplyRunReadback(*runView.snapshot);
                } else {
                    RunPauseButton().IsEnabled(false);
                    RunResumeButton().IsEnabled(false);
                    RunCancelButton().IsEnabled(false);
                    RunState().Text(
                        L"Run readback belongs to a different project. No control was bound.");
                    RunRawDetail().Text(winrt::to_hstring(message));
                }
            } else {
                verifiedRunId_.clear();
                verifiedRunProjectId_.clear();
                RunPauseButton().IsEnabled(false);
                RunResumeButton().IsEnabled(false);
                RunCancelButton().IsEnabled(false);
                RunState().Text(winrt::to_hstring("Run readback unavailable · " + message));
                RunRawDetail().Text(winrt::to_hstring(message));
            }
        } else if (projectAction) {
            if (projectsView.loaded && projectsView.snapshot) {
                ApplyProjectList(*projectsView.snapshot);
                if (!selectedProjectId_.empty()) followUp = Action::ProjectLoad;
            }
            if (projectView.loaded && projectView.snapshot) {
                ApplyProjectWorkspace(*projectView.snapshot);
                if (action == Action::ProjectRegister) {
                    followUp = Action::ProjectList;
                } else if (action == Action::ProjectLoad &&
                    !RunId().Text().empty() && verifiedRunId_.empty()) {
                    followUp = Action::RunStatus;
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
                LmStudioOverview().Text(winrt::to_hstring(message));
                LmStudioBadge().Text(L"UNAVAILABLE");
            }
        } else if (toolsAction) {
            if (toolsView.snapshot) ApplyTools(*toolsView.snapshot);
            ToolsState().Text(winrt::to_hstring(message));
            if (!toolsView.snapshot && tools_.empty()) ToolEmptyBody().Text(winrt::to_hstring(message));
            if (toolOutcomeView.snapshot) {
                ToolOutcome().Text(winrt::to_hstring(
                    message + "\n" + toolOutcomeView.snapshot->canonicalPayload));
            } else if (action == Action::ToolInvoke) {
                ToolOutcome().Text(winrt::to_hstring(message));
            }
        } else if (operationalAction) {
            if (operationalView.snapshot) {
                if (feedDisplayPaused_ && operationalView.snapshot->area ==
                    ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
                    pendingFeedSnapshot_ = *operationalView.snapshot;
                } else {
                    ApplyOperational(*operationalView.snapshot);
                }
            } else {
                OperationalState().Text(winrt::to_hstring(message));
                OperationalListSummary().Text(winrt::to_hstring(message));
                OperationalCount().Text(L"UNAVAILABLE");
                if (operationalLines_.empty()) {
                    OperationalEmptyState().Visibility(Visibility::Visible);
                    OperationalEmptyTitle().Text(L"Live inventory unavailable");
                    OperationalEmptyBody().Text(winrt::to_hstring(message));
                }
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
            action == Action::ProviderTest || action == Action::ProviderModels) {
            if (action == Action::ProviderModels) {
                rebuildingProviderModels_ = true;
                loadedModels_ = modelsView.loaded ? std::move(modelsView.models)
                    : std::vector<std::string>{};
                ProviderLoadedModels().Items().Clear();
                ProviderLoadedModels().Items().Append(box_value(
                    L"Automatic · first compatible loaded model"));
                auto selectedIndex = 0;
                const auto pendingModel = winrt::to_string(ProviderModel().Text());
                for (std::size_t index{}; index < loadedModels_.size(); ++index) {
                    ProviderLoadedModels().Items().Append(box_value(
                        winrt::to_hstring(loadedModels_[index])));
                    if (loadedModels_[index] == pendingModel) {
                        selectedIndex = static_cast<int>(index + 1U);
                    }
                }
                ProviderLoadedModels().SelectedIndex(selectedIndex);
                rebuildingProviderModels_ = false;
                ProviderLoadedCount().Text(modelsView.loaded
                    ? winrt::to_hstring(std::to_string(loadedModels_.size()))
                    : L"—");
                ProviderModelsState().Text(winrt::to_hstring(message));
                providerDiscoveredEndpoint_ = modelsView.loaded && submitted
                    ? std::string{submitted->localModelSecure ? "https://" : "http://"} +
                        submitted->localModelHost + ':' +
                        std::to_string(submitted->localModelPort)
                    : std::string{};
                if (telemetrySnapshot_) ApplyTelemetryPresentation(*telemetrySnapshot_);
            }
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
