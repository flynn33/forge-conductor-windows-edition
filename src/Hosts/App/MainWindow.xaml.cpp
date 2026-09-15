#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <microsoft.ui.xaml.window.h>
#include <shobjidl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
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

[[nodiscard]] Microsoft::UI::Xaml::Media::PointCollection sparklinePoints(
    const std::vector<double>& observations,
    const double width,
    const double height)
{
    if (observations.empty()) return {};
    const auto [minimum, maximum] = std::minmax_element(
        observations.begin(), observations.end());
    // The main chart and gauge retain the absolute 0–100% scale. A miniature
    // sparkline shows only the measured local trend, centered on its range.
    const auto span = std::max(0.5, *maximum - *minimum);
    const auto midpoint = (*minimum + *maximum) / 2.0;
    std::vector<double> normalized;
    normalized.reserve(observations.size());
    for (const auto value : observations) {
        normalized.push_back(std::clamp(
            100.0 * (value - (midpoint - span / 2.0)) / span, 0.0, 100.0));
    }
    return chartPoints(normalized, width, height);
}

[[nodiscard]] std::vector<double> calmHistory(
    const std::vector<double>& values, const std::size_t targetPoints = 72U)
{
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

[[nodiscard]] std::string currentProductVersion()
{
    try {
        const auto version = Windows::ApplicationModel::Package::Current()
            .Id().Version();
        return std::to_string(version.Major) + "." +
            std::to_string(version.Minor) + "." +
            std::to_string(version.Build);
    } catch (...) {
        return std::string{::ForgeConductor::Domain::ProductVersion};
    }
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
    selectedEvidenceRunValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedEvidenceRunId", scope);
    selectedEvidenceProjectValueName_ =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"SelectedEvidenceProjectId", scope);
}

void MainWindow::WindowClosed(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::WindowEventArgs const&)
{
    if (telemetryTimer_) telemetryTimer_.Stop();
    actionScheduler_.cancel();
    cancellation_.request_stop();
    providerContractCancellation_.request_stop();
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
    try {
        auto nativeWindow = this->m_inner.as<::IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                &monitor)) {
            const auto workWidth = static_cast<int>(
                monitor.rcWork.right - monitor.rcWork.left);
            const auto workHeight = static_cast<int>(
                monitor.rcWork.bottom - monitor.rcWork.top);
            const auto dpi = ::GetDpiForWindow(hwnd);
            const auto scale = dpi == 0U
                ? 1.0 : static_cast<double>(dpi) / 96.0;
            const auto physical = [scale](double logical) {
                return static_cast<int>(logical * scale + 0.5);
            };
            const auto width = std::min(physical(1680.0),
                workWidth - physical(40.0));
            const auto height = std::min(physical(920.0), workHeight);
            if (width >= physical(760.0) && height >= physical(600.0)) {
                const auto window = AppWindow();
                window.Resize(Windows::Graphics::SizeInt32{width, height});
                window.Move(Windows::Graphics::PointInt32{
                    monitor.rcWork.left + (workWidth - width) / 2,
                    monitor.rcWork.top});
            }
        }
    } catch (...) {
        // Invalid monitor geometry must not prevent the native console from opening.
    }
    FooterMachineName().Text(currentMachineName());
    const auto profile = connection_
        ? connection_->profileSummary()
        : std::string{"Deployment profile unavailable"};
    ProfileState().Text(winrt::to_hstring(profile));
    const auto dataMarker = profile.find("\nData: ");
    const auto environment = dataMarker == std::string::npos
        ? profile : profile.substr(0U, dataMarker);
    const auto dataRoot = dataMarker == std::string::npos
        ? std::string{"Data root unavailable"}
        : profile.substr(dataMarker + 7U);
    NavigationEnvironment().Text(winrt::to_hstring(environment));
    HeaderEnvironment().Text(winrt::to_hstring(environment));
    HeaderDataRoot().Text(winrt::to_hstring(dataRoot));
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
        const auto savedEvidenceProject = loadSavedText(
            selectedEvidenceProjectValueName_.c_str());
        const auto savedEvidenceRun = loadSavedText(
            selectedEvidenceRunValueName_.c_str());
        if (savedEvidenceProject && savedEvidenceRun &&
            *savedEvidenceProject == *savedProject) {
            selectedEvidenceProjectId_ = winrt::to_string(*savedProject);
            selectedEvidenceRunId_ = winrt::to_string(*savedEvidenceRun);
        }
    }
    // The restored page may immediately enqueue a Manager readback. Attach or
    // launch the authenticated Manager first so a cold-launch catalog does not
    // time out behind startup.
    RunAction(Action::Start);
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
    RunAction(Action::SettingsLoad);
    RunAction(Action::ProjectList);
    RunAction(Action::Refresh);
    telemetryTimer_.Start();
}

void MainWindow::ConsoleSizeChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    const auto width = args.NewSize().Width;
    const auto narrowPane = width < 1400.0;
    const auto paneMode = narrowPane
        ? NavigationViewPaneDisplayMode::LeftCompact
        : NavigationViewPaneDisplayMode::Left;
    if (RootNavigation().PaneDisplayMode() != paneMode) {
        RootNavigation().PaneDisplayMode(paneMode);
        RootNavigation().IsPaneOpen(!narrowPane);
    }
    RootNavigation().IsPaneToggleButtonVisible(narrowPane);

    const auto compactCards = width < 1280.0;
    MetricColumn2().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Star});
    MetricColumn3().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Star});
    Grid::SetRow(GpuMetricCard(), compactCards ? 1 : 0);
    Grid::SetColumn(GpuMetricCard(), compactCards ? 0 : 2);
    Grid::SetRow(ContextMetricCard(), compactCards ? 1 : 0);
    Grid::SetColumn(ContextMetricCard(), compactCards ? 1 : 3);
    HeaderProfileColumn().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Auto});
    Grid::SetRow(ProfileCard(), compactCards ? 1 : 0);
    Grid::SetColumn(ProfileCard(), compactCards ? 0 : 1);
    ProfileCard().MinWidth(compactCards ? 0.0 : 610.0);
    ProfileCard().Margin(compactCards
        ? Thickness{0.0, 14.0, 0.0, 0.0}
        : Thickness{0.0, 0.0, 0.0, 0.0});
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
void MainWindow::ProviderContractClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProviderContract); }
void MainWindow::ProviderContractCancelClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    providerContractCancellation_.request_stop();
    ProviderContractCancelButton().IsEnabled(false);
    ProviderContractState().Text(L"Cancellation requested. Waiting for the bounded provider request to stop.");
}
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
void MainWindow::OpenToolsClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Tools"); }
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
void MainWindow::RunHistoryRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::RunHistory); }
void MainWindow::RunHistoryAttachClicked(Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto button = sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (!button || selectedProjectId_.empty()) return;
    const auto runId = winrt::unbox_value<winrt::hstring>(button.Tag());
    RunId().Text(runId);
    RunAction(Action::RunStatus);
}
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
void MainWindow::ProjectUpdateClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectUpdate); }
void MainWindow::ProjectForgetClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectForget); }
void MainWindow::ProjectEditCloseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    selectedMemoryRecord_.reset();
    selectedMemoryProjectId_.clear();
    ProjectForgetConfirmation().Text(L"");
    ProjectEditCard().Visibility(Visibility::Collapsed);
}
void MainWindow::ProjectArchiveExportClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectArchiveExport); }
void MainWindow::ProjectArchivePreviewClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectArchivePreview); }
void MainWindow::ProjectArchiveImportClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectArchiveImport); }
void MainWindow::ProjectArchivePathChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    ClearArchivePreview();
    ProjectArchivePreviewState().Text(L"Artifact selection changed. Verify it again before importing.");
}
void MainWindow::ProjectArchiveBrowseClicked(Windows::Foundation::IInspectable const&,
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
        winrt::check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST));
        winrt::check_hresult(dialog->SetTitle(L"Choose a project-memory export artifact"));
        const auto shown = dialog->Show(hwnd);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> file;
        winrt::check_hresult(dialog->GetResult(file.put()));
        PWSTR path{};
        winrt::check_hresult(file->GetDisplayName(SIGDN_FILESYSPATH, &path));
        ProjectArchivePath().Text(path);
        ::CoTaskMemFree(path);
        ProjectArchiveState().Text(L"Artifact selected. Verify and preview it before importing.");
    } catch (const winrt::hresult_error& error) {
        ProjectArchiveState().Text(L"The Windows artifact picker failed: " + error.message());
    }
}
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
    BuildToolForm(tool);
}
void MainWindow::OperationalRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::OperationalInspect); }
void MainWindow::RuntimeJobInspectClicked(Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto button = sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (!button || selectedProjectId_.empty()) return;
    RunId().Text(winrt::unbox_value<winrt::hstring>(button.Tag()));
    SelectPage(L"Autonomy");
    RunAction(Action::RunStatus);
}
void MainWindow::OperationalExportClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (!operationalSnapshot_ || operationalSnapshot_->area !=
        ::ForgeConductor::Manager::ManagerOperationalArea::Diagnostics) {
        OperationalExportState().Text(
            L"Refresh Diagnostics and wait for Manager readback before saving a snapshot.");
        return;
    }
    try {
        auto nativeWindow = this->m_inner.as<::IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        winrt::com_ptr<::IFileSaveDialog> dialog;
        winrt::check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{};
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT));
        const COMDLG_FILTERSPEC filter{L"JSON support snapshot", L"*.json"};
        winrt::check_hresult(dialog->SetFileTypes(1, &filter));
        winrt::check_hresult(dialog->SetDefaultExtension(L"json"));
        winrt::check_hresult(dialog->SetFileName(
            L"ForgeConductor-support-snapshot.json"));
        winrt::check_hresult(dialog->SetTitle(
            L"Save local Forge Conductor support snapshot"));
        const auto shown = dialog->Show(hwnd);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> destination;
        winrt::check_hresult(dialog->GetResult(destination.put()));
        PWSTR allocatedPath{};
        winrt::check_hresult(destination->GetDisplayName(
            SIGDN_FILESYSPATH, &allocatedPath));
        const std::wstring path{allocatedPath};
        ::CoTaskMemFree(allocatedPath);
        SYSTEMTIME utc{};
        ::GetSystemTime(&utc);
        char capturedAt[40]{};
        sprintf_s(capturedAt, "%04u-%02u-%02uT%02u:%02u:%02uZ",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
            utc.wSecond);
        const nlohmann::json snapshot{
            {"format", "forge-conductor-diagnostic-support-snapshot"},
            {"product_version", currentProductVersion()},
            {"captured_at_utc", capturedAt},
            {"source", "bounded Manager doctor and diagnostic readback"},
            {"verified_run_evidence", false},
            {"automatic_transmission", false},
            {"lines", operationalSnapshot_->lines}};
        const auto data = snapshot.dump(2);
        if (data.size() > (std::numeric_limits<DWORD>::max)()) {
            OperationalExportState().Text(L"Support snapshot exceeded the local file limit.");
            return;
        }
        const auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            winrt::check_hresult(HRESULT_FROM_WIN32(::GetLastError()));
        }
        DWORD written{};
        const auto saved = ::WriteFile(file, data.data(),
            static_cast<DWORD>(data.size()), &written, nullptr);
        const auto writeError = saved ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(file);
        if (!saved || written != data.size()) {
            winrt::check_hresult(HRESULT_FROM_WIN32(
                writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError));
        }
        OperationalExportState().Text(
            L"Local support snapshot saved. Review its diagnostic context before sharing; no data was transmitted.");
    } catch (const winrt::hresult_error& error) {
        OperationalExportState().Text(L"Support export failed: " + error.message());
    } catch (...) {
        OperationalExportState().Text(L"Support export failed safely.");
    }
}
void MainWindow::EvidenceRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::EvidenceLoad);
}

void MainWindow::EvidenceRunInspectClicked(
    Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto button = sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (!button) return;
    SelectEvidenceRun(winrt::to_string(
        winrt::unbox_value<winrt::hstring>(button.Tag())));
}

void MainWindow::EvidenceExportClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (!evidenceSnapshot_ || selectedEvidenceRunId_.empty() ||
        selectedEvidenceProjectId_ != selectedProjectId_) {
        OperationalEvidenceState().Text(
            L"Select a current exact-project durable run before saving evidence.");
        return;
    }
    try {
        std::optional<nlohmann::json> record;
        for (const auto& line : evidenceSnapshot_->lines) {
            const auto candidate = nlohmann::json::parse(line);
            if (candidate.value("run_id", std::string{}) == selectedEvidenceRunId_ &&
                candidate.value("project_id", std::string{}) == selectedProjectId_) {
                record = candidate;
                break;
            }
        }
        if (!record) {
            OperationalEvidenceState().Text(
                L"The selected run is no longer in current Manager readback. Refresh first.");
            return;
        }
        auto nativeWindow = this->m_inner.as<::IWindowNative>();
        HWND hwnd{};
        winrt::check_hresult(nativeWindow->get_WindowHandle(&hwnd));
        winrt::com_ptr<::IFileSaveDialog> dialog;
        winrt::check_hresult(::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        DWORD options{};
        winrt::check_hresult(dialog->GetOptions(&options));
        winrt::check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT));
        const COMDLG_FILTERSPEC filter{L"Redacted run evidence", L"*.json"};
        winrt::check_hresult(dialog->SetFileTypes(1, &filter));
        winrt::check_hresult(dialog->SetDefaultExtension(L"json"));
        winrt::check_hresult(dialog->SetFileName(
            L"ForgeConductor-run-evidence-redacted.json"));
        winrt::check_hresult(dialog->SetTitle(
            L"Save local redacted run evidence"));
        const auto shown = dialog->Show(hwnd);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> destination;
        winrt::check_hresult(dialog->GetResult(destination.put()));
        PWSTR allocatedPath{};
        winrt::check_hresult(destination->GetDisplayName(
            SIGDN_FILESYSPATH, &allocatedPath));
        const std::wstring path{allocatedPath};
        ::CoTaskMemFree(allocatedPath);
        SYSTEMTIME utc{};
        ::GetSystemTime(&utc);
        char capturedAt[40]{};
        sprintf_s(capturedAt, "%04u-%02u-%02uT%02u:%02u:%02uZ",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
            utc.wSecond);
        const nlohmann::json exportDocument{
            {"format", "forge-conductor-redacted-run-evidence-v1"},
            {"product_version", currentProductVersion()},
            {"captured_at_utc", capturedAt},
            {"source", "native Manager durable run repository readback"},
            {"redacted", true},
            {"automatic_transmission", false},
            {"task_text_included", false},
            {"model_output_included", false},
            {"evidence", *record}};
        const auto data = exportDocument.dump(2);
        if (data.size() > (std::numeric_limits<DWORD>::max)()) {
            OperationalEvidenceState().Text(L"Redacted export exceeded the local file limit.");
            return;
        }
        const auto file = ::CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            winrt::check_hresult(HRESULT_FROM_WIN32(::GetLastError()));
        }
        DWORD written{};
        const auto saved = ::WriteFile(file, data.data(),
            static_cast<DWORD>(data.size()), &written, nullptr);
        const auto writeError = saved ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(file);
        if (!saved || written != data.size()) {
            winrt::check_hresult(HRESULT_FROM_WIN32(
                writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError));
        }
        OperationalEvidenceState().Text(
            L"Redacted evidence saved locally. The stored seal is not task-outcome verification; review before sharing.");
    } catch (const winrt::hresult_error& error) {
        OperationalEvidenceState().Text(L"Evidence export failed: " + error.message());
    } catch (...) {
        OperationalEvidenceState().Text(L"Evidence export failed safely.");
    }
}
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
    if (nextProjectId != selectedProjectId_) {
        ClearSelectedRun();
        ClearArchivePreview();
        ProjectArchiveExportState().Text(L"No archive created for this selection.");
        ProjectArchivePreviewState().Text(L"Project selection changed. Verify an artifact for this project.");
        selectedMemoryRecord_.reset();
        selectedMemoryProjectId_.clear();
        ProjectEditCard().Visibility(Visibility::Collapsed);
    }
    selectedProjectId_ = nextProjectId;
    ProjectArchiveState().Text(winrt::to_hstring(
        "Archive scope bound to " +
        projects_[static_cast<std::size_t>(index)].displayName +
        ". Export or verify a memory artifact for this exact project."));
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);
    RunProjectId().Text(selected);
    ToolProjectId().Text(selected);
    RunAction(Action::ProjectLoad);
    UpdateRunProjectLabel();
    if (PageTitle().Text() == L"Runtimes") RunAction(Action::OperationalInspect);
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
        MainScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
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
        OperationalEvidencePath().Visibility(evidence ? Visibility::Visible : Visibility::Collapsed);
        OperationalEvidenceRunsCard().Visibility(evidence ? Visibility::Visible : Visibility::Collapsed);
        OperationalStatusGrid().Visibility(runtimes || tag == L"Manager"
            ? Visibility::Visible : Visibility::Collapsed);
        OperationalListCard().Visibility(runtimes ? Visibility::Collapsed : Visibility::Visible);
        OperationalRuntimeCard().Visibility(runtimes ? Visibility::Visible : Visibility::Collapsed);
        OperationalRuntimeJobsCard().Visibility(runtimes ? Visibility::Visible : Visibility::Collapsed);
        OperationalDetailCard().Visibility(runtimes ? Visibility::Collapsed : Visibility::Visible);
        OperationalListViewport().Height(runtimes || tag == L"Manager" ? 235.0 :
            evidence ? 355.0 : 545.0);
        OperationalManagerCard().Visibility(tag == L"Manager" ? Visibility::Visible : Visibility::Collapsed);
        OperationalRuntimePolicyCard().Visibility(runtimes ? Visibility::Visible : Visibility::Collapsed);
        OperationalAgentInsightsCard().Visibility(agents ? Visibility::Visible : Visibility::Collapsed);
        OperationalDiagnosticsCard().Visibility(tag == L"Diagnostics"
            ? Visibility::Visible : Visibility::Collapsed);
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
    RunHistoryCard().Visibility(autonomy ? Visibility::Visible : Visibility::Collapsed);
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
        if (telemetryUiInitialized_ && !providerDiscoveryAttempted_) RunAction(Action::ProviderModels);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else if (autonomy) {
        PageDescription().Text(continuityPage
            ? L"Inspect retained context and control the exact Manager-owned run."
            : L"Start, attach, pause, resume, and stop Manager-owned work.");
        RunAction(Action::RunHistory);
    } else if (tag == L"Projects") {
        PageDescription().Text(L"Register authorized folders, select exact project identities, and read or write persistent project memory.");
        RunAction(Action::ProjectList);
    } else if (lmStudioMcp) {
        PageDescription().Text(L"Inspect, repair, and synchronize the native LM Studio MCP registration.");
        RunAction(Action::LmStudioInspect);
        if (telemetryUiInitialized_ && !providerDiscoveryAttempted_) RunAction(Action::ProviderModels);
    } else if (tools) {
        PageDescription().Text(L"Inspect and run the Manager-owned native tool catalog.");
        if (!selectedProjectId_.empty()) {
            ToolProjectId().Text(winrt::to_hstring(selectedProjectId_));
        }
        RunAction(Action::ToolsList);
    } else if (settings) {
        PageDescription().Text(L"Edit and verify Manager-owned preferences without editing files.");
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
        if (tag == L"Events & Evidence") RunAction(Action::EvidenceLoad);
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
    SettingsHeroReadback().Text(L"Prepared settings · awaiting Manager verification");
    SettingsDashboardSummary().Text(winrt::to_hstring(
        settings.dashboardHost + ':' + std::to_string(settings.dashboardPort)));
    SettingsProviderSummary().Text(winrt::to_hstring(
        settings.localModelHost + ':' + std::to_string(settings.localModelPort)));
    SettingsContextSummary().Text(winrt::to_hstring(
        std::to_string(settings.effectiveContextCapacity)));
    SettingsShellSummary().Text(settings.shellEnabled ? L"ENABLED" : L"DISABLED");
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
    if (snapshot.resources.cpuFrequencyMhz.value &&
        snapshot.resources.cpuFrequencyMhz.availability ==
            ::ForgeConductor::Domain::TelemetryMetricAvailability::Available) {
        CpuState().Text(winrt::to_hstring(presentation.cpu.state + " · " +
            std::to_string(*snapshot.resources.cpuFrequencyMhz.value) + " MHz"));
    }
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
            : snapshot.continuity.runId
                ? std::string{"Awaiting usage"} : std::string{"No run"}));
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
    const auto providerEndpoint = std::string{snapshot.provider.secure ? "https://" : "http://"} +
        snapshot.provider.host + ':' + std::to_string(snapshot.provider.port);
    const auto providerDiscovered = providerEndpoint == providerDiscoveredEndpoint_;
    ProviderHealth().Text(winrt::to_hstring(
        std::string{providerDiscovered ? "Model discovery verified · " :
            "Endpoint configured · discovery not verified · "} +
        presentation.providerStatus));
    ProviderActiveEndpoint().Text(winrt::to_hstring(providerEndpoint + " · Manager readback"));
    ProviderActiveModel().Text(snapshot.provider.model
        ? winrt::to_hstring(*snapshot.provider.model)
        : L"Automatic · first compatible loaded model");
    ProviderConnectionBadge().Text(providerDiscovered
        ? L"DISCOVERED" : L"CONFIGURED");
    ProviderConnectionBadge().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
        providerDiscovered
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
    NavigationManagerState().Text(snapshot.manager.serviceActive
        ? L"System online" : L"Manager unavailable");
    LastUpdatedText().Text(currentLocalTime());
    const auto online = Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 61, 220, 151}};
    const auto configured = Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 43, 168, 255}};
    const auto attention = Microsoft::UI::Xaml::Media::SolidColorBrush{
        Windows::UI::Color{255, 255, 200, 87}};
    NavigationStatusDot().Fill(snapshot.manager.serviceActive ? online : attention);
    HeroManagerDot().Fill(snapshot.manager.serviceActive ? online : attention);
    WorkflowDot().Fill(snapshot.manager.serviceActive ? online : attention);
    ProviderDot().Fill(providerDiscovered ? online : configured);
    ContinuityDot().Fill(snapshot.selectedRun ? online : configured);
    StoreDot().Fill(snapshot.storeHealthy.value.value_or(false) ? online : attention);

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
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        Grid tiles;
        tiles.ColumnSpacing(8);
        tiles.RowSpacing(8);
        for (int column{}; column < 4; ++column) {
            ColumnDefinition definition;
            definition.Width(GridLength{1.0, GridUnitType::Star});
            tiles.ColumnDefinitions().Append(definition);
        }
        for (std::size_t index{}; index < presentation.cpuLogicalRows.size(); ++index) {
            if (index % 4U == 0U) {
                RowDefinition definition;
                definition.Height(GridLength{1.0, GridUnitType::Auto});
                tiles.RowDefinitions().Append(definition);
            }
            const auto value = index < presentation.cpuLogicalValues.size()
                ? std::clamp(presentation.cpuLogicalValues[index], 0.0, 100.0)
                : 0.0;
            auto frequency = snapshot.resources.cpuFrequencyMhz.value.value_or(0U);
            if (const auto& perCore = snapshot.resources.cpuPerLogicalFrequencyMhz.value;
                perCore && index < perCore->size()) frequency = (*perCore)[index];
            Border tile;
            tile.Padding(Thickness{9.0, 8.0, 9.0, 8.0});
            tile.CornerRadius(CornerRadius{8.0});
            tile.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 21, 39, 58}});
            tile.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 51, 75, 99}});
            tile.BorderThickness(Thickness{1.0});
            StackPanel content;
            content.Spacing(3);
            Grid heading;
            TextBlock name;
            name.Text(winrt::to_hstring("LOGICAL " + std::to_string(index)));
            name.FontSize(11);
            name.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 157, 179, 200}});
            heading.Children().Append(name);
            TextBlock percent;
            percent.Text(winrt::to_hstring(std::to_string(
                static_cast<int>(std::round(value))) + "%"));
            percent.HorizontalAlignment(HorizontalAlignment::Right);
            percent.FontSize(15);
            percent.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            heading.Children().Append(percent);
            content.Children().Append(heading);
            ProgressBar gauge;
            gauge.Minimum(0.0);
            gauge.Maximum(100.0);
            gauge.Value(value);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
                gauge, winrt::to_hstring(presentation.cpuLogicalRows[index]));
            content.Children().Append(gauge);
            TextBlock clock;
            clock.Text(frequency == 0U ? L"Frequency unavailable"
                : winrt::to_hstring(std::to_string(frequency) + " MHz"));
            clock.FontSize(11);
            clock.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 157, 179, 200}});
            content.Children().Append(clock);
            tile.Child(content);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
                tile, winrt::to_hstring(presentation.cpuLogicalRows[index]));
            Grid::SetRow(tile, static_cast<int>(index / 4U));
            Grid::SetColumn(tile, static_cast<int>(index % 4U));
            tiles.Children().Append(tile);
        }
        CpuCoreRows().Children().Append(tiles);
    }
    GpuAdapterRows().Children().Clear();
    if (snapshot.resources.gpus.empty()) {
        applyRows(GpuAdapterRows(), {}, L"No hardware GPU adapter was reported.");
    } else {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        for (const auto& adapter : snapshot.resources.gpus) {
            TextBlock title;
            title.Text(winrt::to_hstring(adapter.name));
            title.FontSize(15);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextWrapping(TextWrapping::Wrap);
            GpuAdapterRows().Children().Append(title);
            TextBlock memory;
            memory.Text(winrt::to_hstring(
                adapter.dedicatedBytesTotal
                    ? ::ForgeConductor::Hosts::App::bytesText(
                        adapter.dedicatedBytesUsed.value_or(0U)) + " / " +
                        ::ForgeConductor::Hosts::App::bytesText(*adapter.dedicatedBytesTotal) +
                        " dedicated · " + adapter.utilizationSource
                    : adapter.utilizationSource));
            memory.FontSize(11);
            memory.TextWrapping(TextWrapping::Wrap);
            memory.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 157, 179, 200}});
            GpuAdapterRows().Children().Append(memory);
            if (adapter.engines.empty()) continue;
            Grid engines;
            engines.ColumnSpacing(8);
            engines.RowSpacing(8);
            for (int column{}; column < 2; ++column) {
                ColumnDefinition definition;
                definition.Width(GridLength{1.0, GridUnitType::Star});
                engines.ColumnDefinitions().Append(definition);
            }
            for (std::size_t index{}; index < adapter.engines.size(); ++index) {
                if (index % 2U == 0U) {
                    RowDefinition definition;
                    definition.Height(GridLength{1.0, GridUnitType::Auto});
                    engines.RowDefinitions().Append(definition);
                }
                const auto& engine = adapter.engines[index];
                Border tile;
                tile.Padding(Thickness{9.0, 7.0, 9.0, 7.0});
                tile.CornerRadius(CornerRadius{8.0});
                tile.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
                    Windows::UI::Color{255, 21, 39, 58}});
                tile.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
                    Windows::UI::Color{255, 51, 75, 99}});
                tile.BorderThickness(Thickness{1.0});
                StackPanel content;
                content.Spacing(4);
                Grid heading;
                TextBlock name;
                name.Text(winrt::to_hstring(engine.name));
                name.FontSize(12);
                name.TextTrimming(TextTrimming::CharacterEllipsis);
                name.Margin(Thickness{0.0, 0.0, 48.0, 0.0});
                heading.Children().Append(name);
                TextBlock percent;
                percent.Text(winrt::to_hstring(std::to_string(
                    static_cast<int>(std::round(engine.utilizationPercent))) + "%"));
                percent.FontSize(12);
                percent.HorizontalAlignment(HorizontalAlignment::Right);
                percent.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                    Windows::UI::Color{255, 56, 214, 233}});
                heading.Children().Append(percent);
                content.Children().Append(heading);
                ProgressBar gauge;
                gauge.Minimum(0.0);
                gauge.Maximum(100.0);
                gauge.Value(std::clamp(engine.utilizationPercent, 0.0, 100.0));
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
                    gauge, winrt::to_hstring(engine.name + " GPU engine utilization"));
                content.Children().Append(gauge);
                tile.Child(content);
                Grid::SetRow(tile, static_cast<int>(index / 2U));
                Grid::SetColumn(tile, static_cast<int>(index % 2U));
                engines.Children().Append(tile);
            }
            GpuAdapterRows().Children().Append(engines);
        }
    }
    applyRows(VolumeRows(), presentation.volumeRows,
              L"No mounted fixed or removable volume was reported.");
    applyRows(ProcessRows(), presentation.processRows,
              L"No Forge or model-server process is currently visible.");

    const auto width = std::max(320.0, HistoryCanvas().ActualWidth());
    const auto height = std::max(1.0, HistoryCanvas().ActualHeight());
    CpuHistoryLine().Points(chartPoints(calmHistory(presentation.cpuHistory), width, height));
    RamHistoryLine().Points(chartPoints(calmHistory(presentation.ramHistory), width, height));
    GpuHistoryLine().Points(chartPoints(calmHistory(presentation.gpuHistory), width, height));
    MiniCpuLine().Points(sparklinePoints(calmHistory(presentation.cpuHistory, 30U),
        std::max(1.0, MiniCpuCanvas().ActualWidth()),
        std::max(1.0, MiniCpuCanvas().ActualHeight())));
    MiniRamLine().Points(sparklinePoints(calmHistory(presentation.ramHistory, 30U),
        std::max(1.0, MiniRamCanvas().ActualWidth()),
        std::max(1.0, MiniRamCanvas().ActualHeight())));
    MiniGpuLine().Points(sparklinePoints(calmHistory(presentation.gpuHistory, 30U),
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

    std::string timelineKey = std::to_string(snapshot.manager.processId) +
        (snapshot.manager.serviceActive ? ":active" : ":inactive") +
        (snapshot.storeHealthy.value
            ? (*snapshot.storeHealthy.value ? ":store-ok" : ":store-failed")
            : ":store-unknown") +
        snapshot.provider.host + ':' + std::to_string(snapshot.provider.port);
    for (const auto& event : snapshot.recentEvents) {
        timelineKey += event.tool + event.status +
            std::to_string(event.timestamp.time_since_epoch().count()) +
            (event.duration ? std::to_string(event.duration->count()) : "");
    }
    if (timelineKey != activityTimelineKey_) {
    activityTimelineKey_ = std::move(timelineKey);
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
        Microsoft::UI::Xaml::Controls::Border ruled;
        ruled.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{70, 102, 128, 153}});
        ruled.BorderThickness(Microsoft::UI::Xaml::Thickness{0, 0, 0, 1});
        ruled.Padding(Microsoft::UI::Xaml::Thickness{0, 0, 0, 4});
        ruled.Child(row);
        ActivityTimeline().Children().Append(ruled);
    };
    appendEvent(L"CURRENT",
        snapshot.manager.serviceActive
            ? "Native Manager active · PID " + std::to_string(snapshot.manager.processId)
            : "Native Manager unavailable",
        !snapshot.manager.serviceActive);
    if (snapshot.storeHealthy.value) {
        appendEvent(L"CURRENT",
            *snapshot.storeHealthy.value
                ? "Operational store health check succeeded"
                : "Operational store health check failed",
            !*snapshot.storeHealthy.value);
    }
    appendEvent(L"CURRENT",
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
    }

    const auto page = winrt::to_string(PageTitle().Text());
    const auto detail = ::ForgeConductor::Hosts::App::telemetryDetailText(
        snapshot, page);
    if (page != "Rig" && page != "Autonomy" && page != "Continuity" &&
        page != "Provider") {
        GenericState().Text(winrt::to_hstring(detail));
    }
    ApplyLmStudioIdentities();
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
    RunToolScopeReadback().Text(run.allowTools
        ? L"Tool scope · authorized native catalog"
        : L"Tool scope · model-only, no native tools");
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
        "\nManager owned: " + (snapshot.managerOwned ? "yes" : "no") +
        "\nNative tools allowed: " + (run.allowTools ? "yes" : "no");
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
        ProjectTechnicalIdentity().Text(L"No exact binding loaded.");
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
    ClearArchivePreview();
    ProjectArchiveExportState().Text(L"No archive created for this selection.");
    ProjectArchivePreviewState().Text(L"No artifact verified for this project.");
    ProjectArchiveState().Text(L"Choose an authorized project to archive its memory.");
    selectedMemoryRecord_.reset();
    selectedMemoryProjectId_.clear();
    ProjectEditCard().Visibility(Visibility::Collapsed);
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
    if (selectedMemoryProjectId_ != snapshot.project.id.value()) {
        selectedMemoryRecord_.reset();
        selectedMemoryProjectId_.clear();
        ProjectEditCard().Visibility(Visibility::Collapsed);
    }
    if (!selectedProjectId_.empty() && selectedProjectId_ != snapshot.project.id.value())
        ClearSelectedRun();
    selectedProjectId_ = snapshot.project.id.value();
    if (ProjectArchiveState().Text() ==
        L"Choose an authorized project to archive its memory.") {
        ProjectArchiveState().Text(winrt::to_hstring(
            "Archive scope bound to " + snapshot.project.displayName +
            ". Export or verify a memory artifact for this exact project."));
    }
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
    ProjectIdentity().Text(winrt::to_hstring(
        std::string{snapshot.integrityOk ? "Verified Manager binding" : "Manager binding needs attention"} +
        " · " + std::to_string(snapshot.project.aliases.size()) +
        (snapshot.project.aliases.size() == 1U ? " authorized folder" : " authorized folders")));

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
        std::to_string(snapshot.recordCount) + " active memory records · " +
        searchMode + " available"));
    ProjectTechnicalIdentity().Text(winrt::to_hstring(identity +
        "\nStore: " + std::to_string(snapshot.tombstoneCount) +
        " tombstones · " + std::to_string(snapshot.databaseBytes) +
        " database bytes · " + searchMode));

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
        Microsoft::UI::Xaml::Controls::Button select;
        select.Content(box_value(L"Inspect & edit this record"));
        const auto weak = get_weak();
        select.Click([weak, record](auto const&, auto const&) {
            if (const auto self = weak.get()) self->SelectMemoryRecord(record);
        });
        content.Children().Append(select);
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
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Manager ||
        operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes) {
        operationalSnapshot_.reset();
        operationalLines_.clear();
        visibleOperationalIndices_.clear();
        OperationalList().Items().Clear();
        OperationalCards().Items().Clear();
        OperationalStatusValue0().Text(L"Unavailable");
        OperationalStatusValue1().Text(operationalArea_ ==
            ::ForgeConductor::Manager::ManagerOperationalArea::Manager
                ? winrt::to_hstring(currentProductVersion()) : L"Unavailable");
        OperationalStatusValue2().Text(L"Unavailable");
        OperationalStatusValue3().Text(operationalArea_ ==
            ::ForgeConductor::Manager::ManagerOperationalArea::Manager
                ? currentDataRoot() : L"Unavailable");
        OperationalState().Text(winrt::to_hstring(
            "Live Manager readback unavailable · " + explanation));
        OperationalListSummary().Text(L"Last Manager ownership and resource sample was invalidated on disconnect.");
        OperationalCount().Text(L"UNAVAILABLE");
        OperationalDetailTitle().Text(L"Live readback unavailable");
        OperationalDetailBody().Text(winrt::to_hstring(explanation));
        OperationalEmptyState().Visibility(Microsoft::UI::Xaml::Visibility::Visible);
        OperationalEmptyTitle().Text(L"Live inventory unavailable");
        OperationalEmptyBody().Text(winrt::to_hstring(explanation));
        OperationalRuntimeReadiness().Text(L"Runtime readback unavailable");
        OperationalRuntimeOwnership().Text(winrt::to_hstring(explanation));
        OperationalRuntimeExecution().Text(L"Owned operations unavailable");
        OperationalRuntimeStores().Text(L"Open stores unavailable");
        OperationalRuntimeJobs().Text(L"Job state unavailable until the Manager reconnects.");
        OperationalRuntimeJobRows().Children().Clear();
        OperationalShellPolicy().Text(L"Unavailable");
        OperationalJobState().Text(L"Unavailable until the Manager reconnects.");
    }
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
    const bool anyRecordedTool = snapshot.primaryToolOutcomeRecorded ||
        snapshot.fallbackToolOutcomeRecorded ||
        snapshot.continuityToolOutcomeRecorded;
    LmStudioBadge().Text(installed ? L"REGISTERED" : L"ATTENTION");
    LmStudioHostReadiness().Text(snapshot.lmStudioPresent && snapshot.binaryExecutable
        ? L"LM Studio detected · Forge CLI ready"
        : snapshot.lmStudioPresent ? L"LM Studio detected · Forge CLI unavailable"
            : L"LM Studio not detected on this host");
    LmStudioRegistrationReadiness().Text(installed
        ? L"Primary, fallback & CLU registered"
        : L"Native registration incomplete");
    LmStudioConnectorReadiness().Text(!snapshot.connectionCheckPerformed
        ? L"Live role check not yet run"
        : snapshot.primaryConnectorReady && snapshot.fallbackConnectorReady &&
            snapshot.continuityConnectorReady && snapshot.connectedClientObserved
            ? (anyRecordedTool ? L"Three role hosts live · MCP result recorded"
                : L"Three role hosts live · no recorded tool result")
            : snapshot.connectedClientObserved
                ? (anyRecordedTool ? L"Role host live · MCP result recorded"
                    : L"Role host live · no recorded tool result")
                : (anyRecordedTool ? L"No live role host · prior MCP result recorded"
                    : L"No live role host · no recorded tool result"));
    LmStudioOverview().Text(winrt::to_hstring(
        std::string{installed ? "Three native roles registered" : "Registration requires attention"} +
        " · " + (snapshot.connectedClientObserved ? "live MCP role host observed" : "no live MCP role host") +
        (anyRecordedTool ? " · audited MCP result" : "")));
    LmStudioPrimaryRole().Text(winrt::to_hstring(
        std::string{snapshot.primaryPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.primaryConnectorReady ? "role host live" : "role host not live")
            : "not yet verified") +
        (snapshot.primaryToolOutcomeRecorded ? " · tool result recorded" : "")));
    LmStudioFallbackRole().Text(winrt::to_hstring(
        std::string{snapshot.fallbackPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.fallbackConnectorReady ? "role host live" : "role host not live")
            : "not yet verified") +
        (snapshot.fallbackToolOutcomeRecorded ? " · tool result recorded" : "")));
    LmStudioCluRole().Text(winrt::to_hstring(
        std::string{snapshot.continuityPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.continuityConnectorReady ? "role host live" : "role host not live")
            : "not yet verified") +
        (snapshot.continuityToolOutcomeRecorded ? " · tool result recorded" : "")));
    LmStudioRegistrationState().Text(winrt::to_hstring(
        std::string{"Installed registration: "} + (installed ? "complete" : "incomplete") +
        "\nPrimary: " + (snapshot.primaryPluginInstalled ? "installed" : "missing") +
        " · Fallback: " + (snapshot.fallbackPluginInstalled ? "installed" : "missing") +
        " · CLU: " + (snapshot.continuityPluginInstalled ? "installed" : "missing") +
        " · MCP config: " + (snapshot.mcpConfigurationRegistered ? "registered" : "missing") +
        "\n" + snapshot.detail + "\n" + snapshot.actionDetail));
    const auto connectionDetail = snapshot.connectionCheckPerformed
            ? std::string{"Live MCP role hosts: primary "} +
                (snapshot.primaryConnectorReady ? "yes" : "no") +
                ", fallback " + (snapshot.fallbackConnectorReady ? "yes" : "no") +
                ", CLU " + (snapshot.continuityConnectorReady ? "yes" : "no") +
                ". At least one live role host: " +
                (snapshot.connectedClientObserved ? "yes" : "no") +
                ". External LM Studio caller: not verified."
            : std::string{"Live role check: not run. External LM Studio caller: not verified."};
    LmStudioConnectionState().Text(winrt::to_hstring(
        connectionDetail + "\n" + snapshot.toolAuditDetail));
    LmStudioContinuityState().Text(winrt::to_hstring(
        "Manager continuity projects active: " +
        std::to_string(snapshot.managedContinuityProjects)));
    LmStudioActionStatus().Text(winrt::to_hstring(snapshot.actionDetail));
    LmStudioPaths().Text(winrt::to_hstring(
        "Binary: " + snapshot.binaryPath + "\nPrimary: " + snapshot.primaryPluginPath +
        "\nFallback: " + snapshot.fallbackPluginPath +
        "\nCLU: " + snapshot.continuityPluginPath +
        "\nConfiguration: " + snapshot.mcpConfigurationPath));
    ApplyLmStudioIdentities();
}

void MainWindow::ClearArchivePreview()
{
    archivePreviewProjectId_.clear();
    archivePreviewPath_.clear();
    archivePreviewChecksum_.clear();
    ProjectArchiveImportButton().IsEnabled(false);
}

void MainWindow::ApplyLmStudioIdentities()
{
    if (!telemetrySnapshot_) {
        LmStudioModelIdentity().Text(L"Model discovery not yet run");
        LmStudioBackendIdentity().Text(L"Reading Manager configuration");
        LmStudioProcessIdentity().Text(L"Awaiting native process sample");
        return;
    }

    const auto& telemetry = *telemetrySnapshot_;
    std::string model;
    if (!loadedModels_.empty()) {
        model = loadedModels_.front();
        if (loadedModels_.size() > 1U) {
            model += " +" + std::to_string(loadedModels_.size() - 1U);
        }
        model += " · endpoint discovered";
    } else if (providerDiscoveryAttempted_) {
        model = providerDiscoverySucceeded_
            ? "No loaded model · endpoint discovered"
            : "Model endpoint unavailable · discovery failed";
    } else if (telemetry.provider.model) {
        model = *telemetry.provider.model + " · configured, load unverified";
    } else {
        model = "Automatic · load discovery not run";
    }
    LmStudioModelIdentity().Text(winrt::to_hstring(model));

    const auto& provider = telemetry.provider;
    const auto backend = provider.host.empty() || provider.port == 0U
        ? std::string{"No Manager endpoint configured"}
        : std::string{provider.secure ? "HTTPS · " : "HTTP · "} +
            provider.host + ":" + std::to_string(provider.port) +
            " · configured";
    LmStudioBackendIdentity().Text(winrt::to_hstring(backend));

    std::optional<std::uint32_t> lmStudioProcess;
    std::vector<std::uint32_t> roleProcesses;
    for (const auto& process : telemetry.resources.processes) {
        auto name = process.name;
        std::transform(name.begin(), name.end(), name.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (name.find("lm studio") != std::string::npos ||
            name.find("lmstudio") != std::string::npos) {
            lmStudioProcess = process.processId;
        } else if (name.find("forge-conductor") != std::string::npos &&
                   roleProcesses.size() < 3U) {
            roleProcesses.push_back(process.processId);
        }
    }
    std::string processes = telemetry.manager.serviceActive
        ? "Manager PID " + std::to_string(telemetry.manager.processId)
        : "Manager not active in sample";
    if (lmStudioProcess) {
        processes += " · LM Studio PID " +
            std::to_string(*lmStudioProcess);
    } else {
        processes += " · LM Studio not in sample";
    }
    if (!roleProcesses.empty()) {
        processes += " · CLI PID";
        processes += roleProcesses.size() == 1U ? " " : "s ";
        for (std::size_t index{}; index < roleProcesses.size(); ++index) {
            if (index > 0U) {
                processes += ", ";
            }
            processes += std::to_string(roleProcesses[index]);
        }
    }
    LmStudioProcessIdentity().Text(winrt::to_hstring(processes));
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

void MainWindow::BuildToolForm(
    const ::ForgeConductor::Manager::ManagerToolDescriptor& tool)
{
    using Json = nlohmann::json;
    using namespace Microsoft::UI::Xaml::Controls;
    ToolFormFields().Children().Clear();
    toolFields_.clear();
    toolFormSupported_ = false;
    ToolAdvancedMode().IsOn(false);
    ToolArguments().Text(L"{}");
    try {
        const auto schema = Json::parse(tool.inputSchema);
        if (!schema.is_object() || schema.value("type", std::string{}) != "object" ||
            !schema.contains("properties") || !schema.at("properties").is_object() ||
            schema.at("properties").size() > 24U) {
            throw std::runtime_error{"The schema needs advanced JSON arguments."};
        }
        const auto required = schema.value("required", Json::array());
        if (!required.is_array()) throw std::runtime_error{"Invalid required fields."};
        for (auto it = schema.at("properties").begin();
             it != schema.at("properties").end(); ++it) {
            if (tool.requiresProject && it.key() == "project_id") continue;
            if (!it.value().is_object()) throw std::runtime_error{"Unsupported field schema."};
            const auto& property = it.value();
            ToolField field;
            field.name = it.key();
            field.required = std::find(required.begin(), required.end(), field.name) != required.end();
            field.type = property.value("type", std::string{});
            if (field.type == "array" && property.contains("items") &&
                property.at("items").is_object() &&
                property.at("items").value("type", std::string{}) == "string") {
                field.type = "string-array";
            }
            if (field.type != "string" && field.type != "integer" &&
                field.type != "number" && field.type != "boolean" &&
                field.type != "string-array" && field.type != "object") {
                throw std::runtime_error{"This schema has a complex field."};
            }
            const auto title = field.name + (field.required ? " · required" : " · optional");
            if (property.contains("enum") && property.at("enum").is_array() &&
                !property.at("enum").empty() && field.type == "string") {
                field.choice = ComboBox{};
                field.choice.Header(box_value(winrt::to_hstring(title)));
                field.choice.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                ComboBoxItem placeholder;
                placeholder.Content(box_value(L"Select a value…"));
                field.choice.Items().Append(placeholder);
                for (const auto& value : property.at("enum")) {
                    if (!value.is_string()) throw std::runtime_error{"Unsupported enum value."};
                    ComboBoxItem item;
                    item.Content(box_value(winrt::to_hstring(value.get<std::string>())));
                    field.choice.Items().Append(item);
                }
                field.choice.SelectedIndex(0);
                ToolFormFields().Children().Append(field.choice);
            } else if (field.type == "boolean") {
                field.toggle = ToggleSwitch{};
                field.toggle.Header(box_value(winrt::to_hstring(title)));
                field.toggle.OffContent(box_value(L"False"));
                field.toggle.OnContent(box_value(L"True"));
                ToolFormFields().Children().Append(field.toggle);
            } else {
                field.text = TextBox{};
                field.text.Header(box_value(winrt::to_hstring(title)));
                field.text.PlaceholderText(field.type == "string-array"
                    ? L"Comma-separated values" : field.type == "object"
                    ? L"JSON object · advanced field" : field.type == "integer" || field.type == "number"
                    ? L"Enter a number" : L"Enter a value");
                ToolFormFields().Children().Append(field.text);
            }
            if (property.contains("description") && property.at("description").is_string()) {
                TextBlock description;
                description.Text(winrt::to_hstring(property.at("description").get<std::string>()));
                description.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                description.FontSize(11);
                ToolFormFields().Children().Append(description);
            }
            toolFields_.push_back(std::move(field));
        }
        toolFormSupported_ = true;
        ToolFormState().Text(tool.requiresProject
            ? L"Project identity comes from the selected authorized project. Complete the remaining fields below."
            : toolFields_.empty()
                ? L"This capability has no arguments. Select Run to invoke it."
                : L"Complete the fields below. The Manager validates the canonical request before execution.");
    } catch (...) {
        ToolFormFields().Children().Clear();
        toolFields_.clear();
        ToolAdvancedMode().IsOn(true);
        ToolFormState().Text(L"This capability has nested or unsupported arguments. Open Advanced and provide canonical JSON.");
    }
}

void MainWindow::SelectMemoryRecord(
    const ::ForgeConductor::Manager::ManagerProjectMemoryRecord& record)
{
    if (selectedProjectId_.empty()) return;
    selectedMemoryRecord_ = record;
    selectedMemoryProjectId_ = selectedProjectId_;
    ProjectEditTitle().Text(winrt::to_hstring(record.title));
    ProjectEditBinding().Text(winrt::to_hstring(
        "Selected in " + winrt::to_string(ProjectHeroName().Text()) +
        " · version " + std::to_string(record.version) +
        " · Manager checks this exact record before saving."));
    ProjectEditName().Text(winrt::to_hstring(record.title));
    ProjectEditSummary().Text(winrt::to_hstring(record.summary));
    ProjectEditBody().Text(winrt::to_hstring(record.body.value_or(std::string{})));
    std::string tags;
    for (const auto& tag : record.tags) {
        if (!tags.empty()) tags += ", ";
        tags += tag;
    }
    ProjectEditTags().Text(winrt::to_hstring(tags));
    ProjectForgetConfirmation().Text(L"");
    ProjectMemoryActionState().Text(L"No edit submitted. Changes remain pending until Manager readback.");
    ProjectEditCard().Visibility(Visibility::Visible);
    ProjectEditCard().StartBringIntoView();
}

std::optional<std::string> MainWindow::ToolCanonicalArguments()
{
    using Json = nlohmann::json;
    const auto selected = ToolList().SelectedIndex();
    const auto requiresProject = selected >= 0 &&
        static_cast<std::size_t>(selected) < visibleTools_.size() &&
        tools_[visibleTools_[static_cast<std::size_t>(selected)]].requiresProject;
    if (requiresProject && selectedProjectId_.empty()) {
        ToolFormState().Text(L"Select an authorized project in Projects first.");
        return std::nullopt;
    }
    if (ToolAdvancedMode().IsOn() || !toolFormSupported_) {
        try {
            auto parsed = Json::parse(winrt::to_string(ToolArguments().Text()));
            if (!parsed.is_object()) throw std::runtime_error{"Arguments must be a JSON object."};
            if (requiresProject) parsed["project_id"] = selectedProjectId_;
            return parsed.dump();
        } catch (...) {
            ToolFormState().Text(L"Advanced arguments must be a valid JSON object.");
            return std::nullopt;
        }
    }
    Json arguments = Json::object();
    if (requiresProject) arguments["project_id"] = selectedProjectId_;
    for (const auto& field : toolFields_) {
        const auto value = field.text ? winrt::to_string(field.text.Text()) : std::string{};
        if (field.choice) {
            if (field.choice.SelectedIndex() <= 0) {
                if (field.required) {
                    ToolFormState().Text(winrt::to_hstring("Choose " + field.name + " before running."));
                    return std::nullopt;
                }
                continue;
            }
            const auto item = field.choice.SelectedItem().as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
            arguments[field.name] = winrt::to_string(unbox_value<hstring>(item.Content()));
        } else if (field.toggle) {
            arguments[field.name] = field.toggle.IsOn();
        } else if (value.empty()) {
            if (field.required) {
                ToolFormState().Text(winrt::to_hstring("Enter " + field.name + " before running."));
                return std::nullopt;
            }
        } else if (field.type == "integer" || field.type == "number" || field.type == "object") {
            try {
                auto parsed = Json::parse(value);
                if ((field.type == "integer" && !parsed.is_number_integer()) ||
                    (field.type == "number" && !parsed.is_number()) ||
                    (field.type == "object" && !parsed.is_object())) {
                    throw std::runtime_error{"Wrong argument type."};
                }
                arguments[field.name] = std::move(parsed);
            } catch (...) {
                ToolFormState().Text(winrt::to_hstring("Check the value of " + field.name + "."));
                return std::nullopt;
            }
        } else if (field.type == "string-array") {
            Json values = Json::array();
            std::stringstream stream{value};
            std::string item;
            while (std::getline(stream, item, ',')) {
                const auto start = item.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                const auto end = item.find_last_not_of(" \t\r\n");
                values.push_back(item.substr(start, end - start + 1));
            }
            arguments[field.name] = std::move(values);
        } else {
            arguments[field.name] = value;
        }
    }
    ToolFormState().Text(L"Canonical arguments ready for Manager validation.");
    return arguments.dump();
}

void MainWindow::RenderToolOutcome(
    const ::ForgeConductor::Manager::ManagerToolOutcomeSnapshot& snapshot,
    const std::string_view message)
{
    using Json = nlohmann::json;
    using namespace Microsoft::UI::Xaml::Controls;
    ToolOutcomeRecords().Children().Clear();
    ToolOutcome().Text(winrt::to_hstring(snapshot.canonicalPayload));
    ToolOutcomeStatus().Text(winrt::to_hstring(
        std::string{snapshot.ok ? "Completed" : "Did not complete"} +
        " · " + snapshot.toolName + " · " +
        (snapshot.elapsed.count() == 0 ? std::string{"<1 ms"}
            : std::to_string(snapshot.elapsed.count()) + " ms")));
    ToolLatestBadge().Text(L"LAST RESULT · " + ToolOutcomeStatus().Text());
    ToolLatestBadge().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
        snapshot.ok ? Windows::UI::Color{255, 61, 220, 151}
                    : Windows::UI::Color{255, 255, 200, 87}});
    ToolOutcomeStatus().Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
        snapshot.ok ? Windows::UI::Color{255, 61, 220, 151}
                    : Windows::UI::Color{255, 255, 200, 87}});
    ToolOutcomeSummary().Text(winrt::to_hstring(message));

    const auto addRecord = [this](const std::string& title, const std::string& body) {
        Border card;
        card.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{255, 21, 42, 63}});
        card.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::Color{110, 75, 109, 142}});
        card.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
        card.CornerRadius(Microsoft::UI::Xaml::CornerRadius{8});
        card.Padding(Microsoft::UI::Xaml::Thickness{12, 9, 12, 9});
        StackPanel content;
        content.Spacing(3);
        TextBlock heading;
        heading.Text(winrt::to_hstring(title));
        heading.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Segoe UI Variable Display"});
        heading.FontSize(14);
        heading.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        heading.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        content.Children().Append(heading);
        if (!body.empty()) {
            TextBlock detail;
            detail.Text(winrt::to_hstring(body));
            detail.FontSize(12);
            detail.MaxLines(3);
            detail.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            detail.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            detail.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, 184, 197, 211}});
            content.Children().Append(detail);
        }
        card.Child(content);
        ToolOutcomeRecords().Children().Append(card);
    };
    const auto displayValue = [](const Json& value) -> std::string {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_primitive()) return value.dump();
        return {};
    };
    const auto firstValue = [&displayValue](const Json& object,
        const std::initializer_list<std::string_view> keys) -> std::string {
        if (!object.is_object()) return displayValue(object);
        for (const auto key : keys) {
            const auto found = object.find(std::string{key});
            if (found != object.end()) {
                const auto value = displayValue(*found);
                if (!value.empty()) return value;
            }
        }
        return {};
    };
    try {
        const auto payload = Json::parse(snapshot.canonicalPayload);
        const Json* records = payload.is_array() ? &payload : nullptr;
        std::string group;
        if (payload.is_object()) {
            for (auto it = payload.begin(); it != payload.end(); ++it) {
                if (it.value().is_array()) {
                    records = &it.value();
                    group = it.key();
                    break;
                }
            }
        }
        if (records) {
            ToolOutcomeSummary().Text(winrt::to_hstring(
                std::to_string(records->size()) + " " +
                (group.empty() ? "returned records" : group) +
                " · Manager-owned result"));
            const auto limit = std::min<std::size_t>(records->size(), 8U);
            for (std::size_t index{}; index < limit; ++index) {
                const auto& record = records->at(index);
                auto title = firstValue(record,
                    {"display_name", "name", "title", "agent_id", "id", "tool"});
                if (title.empty()) title = "Record " + std::to_string(index + 1U);
                auto body = firstValue(record,
                    {"description", "summary", "status", "message", "detail"});
                if (body.empty() && !record.is_object()) body = displayValue(record);
                addRecord(title, body);
            }
            if (records->size() > limit) addRecord(
                std::to_string(records->size() - limit) + " more records",
                "Open Advanced for the complete canonical Manager result.");
        } else if (payload.is_object()) {
            std::size_t shown{};
            for (auto it = payload.begin(); it != payload.end() && shown < 8U; ++it) {
                const auto value = displayValue(it.value());
                if (value.empty()) continue;
                addRecord(it.key(), value);
                ++shown;
            }
            if (!shown) addRecord("Structured Manager result",
                "Open Advanced for the complete canonical payload.");
        } else {
            addRecord("Manager result", displayValue(payload));
        }
    } catch (...) {
        addRecord("Manager result available",
            "The returned payload is not a JSON object. Open Advanced to inspect it exactly.");
    }
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
    ToolListViewport().Height(visibleTools_.empty() ? 285.0 : std::clamp(
        72.0 + static_cast<double>(visibleTools_.size()) * 76.0,
        148.0, 570.0));
    ToolEmptyState().Visibility(visibleTools_.empty()
        ? Visibility::Visible : Visibility::Collapsed);
    ToolEmptyTitle().Text(tools_.empty() ? L"Catalog unavailable" : L"No matching tools");
    ToolEmptyBody().Text(tools_.empty()
        ? L"Connect to the Manager and reload its registered capabilities."
        : L"Try another name, capability, or pack filter.");
    if (!visibleTools_.empty()) ToolList().SelectedIndex(0);
}

void MainWindow::ApplyRunHistory(
    const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot)
{
    RunHistoryRows().Children().Clear();
    if (snapshot.area != ::ForgeConductor::Manager::ManagerOperationalArea::Runs)
        return;
    RunHistoryState().Text(snapshot.lines.empty()
        ? L"No recent Manager-owned runs were found for this project. Start a mission above to create one."
        : winrt::to_hstring("Showing " + std::to_string(snapshot.lines.size()) +
            " Manager-owned project runs from the bounded recent-session window."));
    for (const auto& line : snapshot.lines) {
        const auto firstEnd = line.find('\n');
        const auto first = line.substr(0, firstEnd);
        const auto separator = first.find(" · ");
        if (separator == std::string::npos) continue;
        const auto id = first.substr(0, separator);
        const auto state = first.substr(separator + std::string{" · "}.size());
        Microsoft::UI::Xaml::Controls::StackPanel content;
        content.Spacing(6);
        Microsoft::UI::Xaml::Controls::StackPanel heading;
        heading.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
        heading.Spacing(12);
        Microsoft::UI::Xaml::Controls::Button attach;
        attach.Content(box_value(L"Attach run"));
        attach.Tag(box_value(winrt::to_hstring(id)));
        attach.Click({this, &MainWindow::RunHistoryAttachClicked});
        heading.Children().Append(attach);
        Microsoft::UI::Xaml::Controls::TextBlock identity;
        identity.Text(winrt::to_hstring(state + " · " + id));
        identity.FontSize(14);
        identity.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        identity.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
        identity.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
        heading.Children().Append(identity);
        content.Children().Append(heading);
        Microsoft::UI::Xaml::Controls::TextBlock task;
        task.Text(firstEnd == std::string::npos ? L"No task summary was projected."
            : winrt::to_hstring(line.substr(firstEnd + 1U)));
        task.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        task.MaxLines(2);
        task.FontSize(13);
        content.Children().Append(task);
        Microsoft::UI::Xaml::Controls::Border row;
        row.Padding(Microsoft::UI::Xaml::Thickness{12});
        row.CornerRadius(Microsoft::UI::Xaml::CornerRadius{9});
        row.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
        row.Background(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255, 13, 27, 42}));
        row.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{90, 102, 128, 153}));
        row.Child(content);
        RunHistoryRows().Children().Append(row);
    }
}

void MainWindow::ApplyEvidence(
    const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot)
{
    OperationalEvidenceRunRows().Children().Clear();
    evidenceSnapshot_.reset();
    if (snapshot.area != ::ForgeConductor::Manager::ManagerOperationalArea::Evidence ||
        selectedProjectId_.empty()) {
        OperationalEvidenceState().Text(L"Exact-project evidence readback was not returned.");
        return;
    }
    try {
        auto accepted = snapshot;
        accepted.lines.clear();
        for (const auto& line : snapshot.lines) {
            const auto record = nlohmann::json::parse(line);
            if (!record.is_object() ||
                record.value("format", std::string{}) !=
                    "forge-conductor-managed-run-evidence-v1" ||
                record.value("project_id", std::string{}) != selectedProjectId_ ||
                record.value("run_id", std::string{}).empty()) {
                throw std::runtime_error{"Evidence readback has an invalid project or run identity."};
            }
            accepted.lines.push_back(line);
            const auto state = record.value("state", std::string{"unknown"});
            const auto integrity = record.value(
                "native_record_integrity", std::string{"unavailable"});
            const auto id = record.value("run_id", std::string{});
            Microsoft::UI::Xaml::Controls::Button inspect;
            inspect.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            inspect.Tag(box_value(winrt::to_hstring(id)));
            inspect.Click({this, &MainWindow::EvidenceRunInspectClicked});
            Microsoft::UI::Xaml::Controls::StackPanel content;
            content.Spacing(3);
            Microsoft::UI::Xaml::Controls::TextBlock title;
            title.Text(winrt::to_hstring(state + " · " + integrity));
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
            content.Children().Append(title);
            Microsoft::UI::Xaml::Controls::TextBlock identity;
            identity.Text(winrt::to_hstring(id));
            identity.FontSize(11);
            identity.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
            content.Children().Append(identity);
            inspect.Content(content);
            OperationalEvidenceRunRows().Children().Append(inspect);
        }
        evidenceSnapshot_ = std::move(accepted);
        auto projectName = selectedProjectId_;
        for (const auto& project : projects_) {
            if (project.id.value() == selectedProjectId_) {
                projectName = project.displayName;
                break;
            }
        }
        OperationalEvidenceProject().Text(winrt::to_hstring(
            projectName + " · " +
            std::to_string(evidenceSnapshot_->lines.size()) +
            " durable Manager-owned run" +
            (evidenceSnapshot_->lines.size() == 1U ? "" : "s") +
            " for this exact project"));
        OperationalEvidenceArtifactCount().Text(winrt::to_hstring(
            std::to_string(evidenceSnapshot_->lines.size()) + " project runs"));
        if (evidenceSnapshot_->lines.empty()) {
            selectedEvidenceRunId_.clear();
            selectedEvidenceProjectId_.clear();
            OperationalEvidenceSelectedState().Text(L"No durable run found.");
            OperationalEvidenceVerifyState().Text(L"No run selected");
            OperationalEvidenceState().Text(
                L"Start a Manager-owned mission in Autonomy to create a durable run. Audit entries alone are not run evidence.");
            return;
        }
        bool restored{};
        if (selectedEvidenceProjectId_ == selectedProjectId_) {
            for (const auto& line : evidenceSnapshot_->lines) {
                const auto record = nlohmann::json::parse(line);
                if (record.value("run_id", std::string{}) == selectedEvidenceRunId_) {
                    restored = true;
                    break;
                }
            }
        }
        const auto first = nlohmann::json::parse(evidenceSnapshot_->lines.front());
        SelectEvidenceRun(restored ? selectedEvidenceRunId_ :
            first.value("run_id", std::string{}));
        OperationalEvidenceState().Text(
            L"Native durable-store readback is current. Stored integrity and task outcome are separate checks.");
    } catch (...) {
        evidenceSnapshot_.reset();
        OperationalEvidenceRunRows().Children().Clear();
        OperationalEvidenceArtifactCount().Text(L"Readback invalid");
        OperationalEvidenceVerifyState().Text(L"Not verified");
        OperationalEvidenceSelectedState().Text(L"Evidence projection rejected.");
        OperationalEvidenceState().Text(
            L"The Manager evidence projection contained malformed or foreign-project data. No run was selected.");
    }
}

void MainWindow::SelectEvidenceRun(const std::string_view runId)
{
    if (!evidenceSnapshot_ || selectedProjectId_.empty()) return;
    try {
        for (const auto& line : evidenceSnapshot_->lines) {
            const auto record = nlohmann::json::parse(line);
            if (record.value("run_id", std::string{}) != runId ||
                record.value("project_id", std::string{}) != selectedProjectId_) {
                continue;
            }
            selectedEvidenceRunId_ = std::string{runId};
            selectedEvidenceProjectId_ = selectedProjectId_;
            storeSavedText(selectedEvidenceRunValueName_.c_str(),
                winrt::to_hstring(selectedEvidenceRunId_));
            storeSavedText(selectedEvidenceProjectValueName_.c_str(),
                winrt::to_hstring(selectedEvidenceProjectId_));
            const auto state = record.value("state", std::string{"unknown"});
            const auto integrity = record.value(
                "native_record_integrity", std::string{"unavailable"});
            OperationalEvidenceSelectedState().Text(winrt::to_hstring(
                state + " · record seal " + integrity));
            OperationalEvidenceVerifyState().Text(L"Task unverified");
            const auto optionalText = [&record](const char* key) {
                return record.contains(key) && record[key].is_string()
                    ? record[key].get<std::string>()
                    : std::string{"not recorded"};
            };
            const auto detail =
                "Run · " + selectedEvidenceRunId_ +
                "\nProject · " + selectedProjectId_ +
                "\nProvider response · " + optionalText("provider_response_id") +
                "\nTask SHA-256 · " + optionalText("task_sha256") +
                "\nStored output SHA-256 · " +
                    optionalText("stored_output_sha256") +
                "\nNative record seal · " +
                    optionalText("evidence_seal_sha256") +
                "\nNative record integrity · " + integrity +
                "\nTask outcome verification · not configured" +
                "\nNo approved native task check was attached; model text is not a verified assignment result.";
            OperationalEvidenceDigestDetail().Text(winrt::to_hstring(detail));
            return;
        }
        OperationalEvidenceState().Text(
            L"That run is not present for the current exact project. Refresh evidence first.");
    } catch (...) {
        OperationalEvidenceState().Text(
            L"The selected run evidence could not be parsed safely.");
    }
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
                currentProductVersion()));
            OperationalStatusValue2().Text(winrt::to_hstring(valueAfter("Owned operations: ")));
            OperationalStatusValue3().Text(currentDataRoot());
        } else {
            OperationalStatusValue0().Text(winrt::to_hstring(valueAfter("Owned operations: ")));
            OperationalStatusValue1().Text(winrt::to_hstring(valueAfter("Background threads: ")));
            OperationalStatusValue2().Text(winrt::to_hstring(valueAfter("Child processes: ")));
            OperationalStatusValue3().Text(winrt::to_hstring(
                valueAfter("Open repositories/databases: ")));
            OperationalShellPolicy().Text(winrt::to_hstring(
                valueAfter("Effective shell policy: ")));
            OperationalJobState().Text(winrt::to_hstring(
                valueAfter("Job inventory: ")));
            const auto operations = valueAfter("Owned operations: ");
            const auto threads = valueAfter("Background threads: ");
            const auto children = valueAfter("Child processes: ");
            const auto stores = valueAfter("Open repositories/databases: ");
            OperationalRuntimeReadiness().Text(
                operations == "0" && threads == "0" && children == "0"
                    ? L"Ready · execution lane idle"
                    : L"Manager resources are active");
            OperationalRuntimeOwnership().Text(winrt::to_hstring(
                threads + " background threads · " + children +
                " child processes · " + stores + " open stores"));
            OperationalRuntimeExecution().Text(winrt::to_hstring(
                operations + " owned operations"));
            OperationalRuntimeStores().Text(winrt::to_hstring(
                stores + " open stores"));
            OperationalRuntimeJobs().Text(winrt::to_hstring(
                valueAfter("Job inventory: ")));
            OperationalRuntimeJobRows().Children().Clear();
            for (const auto& line : snapshot.lines) {
                if (!line.starts_with("Job ") ||
                    line.starts_with("Job inventory: ")) continue;
                const auto firstEnd = line.find('\n');
                const auto first = line.substr(4U, firstEnd - 4U);
                const auto separator = first.find(" · ");
                if (separator == std::string::npos) continue;
                const auto id = first.substr(0U, separator);
                Microsoft::UI::Xaml::Controls::StackPanel content;
                content.Spacing(7);
                Microsoft::UI::Xaml::Controls::StackPanel heading;
                heading.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
                heading.Spacing(11);
                Microsoft::UI::Xaml::Controls::Button inspect;
                inspect.Content(box_value(L"Inspect & control"));
                inspect.Tag(box_value(winrt::to_hstring(id)));
                inspect.Click({this, &MainWindow::RuntimeJobInspectClicked});
                heading.Children().Append(inspect);
                Microsoft::UI::Xaml::Controls::TextBlock identity;
                identity.Text(winrt::to_hstring(first.substr(
                    separator + std::string{" · "}.size()) + " · " + id));
                identity.FontSize(14);
                identity.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                identity.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
                    Windows::UI::Color{255, 43, 168, 255}));
                identity.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                identity.IsTextSelectionEnabled(true);
                heading.Children().Append(identity);
                content.Children().Append(heading);
                Microsoft::UI::Xaml::Controls::TextBlock detail;
                detail.Text(firstEnd == std::string::npos ? L"No job detail recorded."
                    : winrt::to_hstring(line.substr(firstEnd + 1U)));
                detail.FontSize(13);
                detail.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
                detail.IsTextSelectionEnabled(true);
                content.Children().Append(detail);
                Microsoft::UI::Xaml::Controls::Border row;
                row.Padding(Microsoft::UI::Xaml::Thickness{12});
                row.CornerRadius(Microsoft::UI::Xaml::CornerRadius{9});
                row.BorderThickness(Microsoft::UI::Xaml::Thickness{1});
                row.Background(Microsoft::UI::Xaml::Media::SolidColorBrush(
                    Windows::UI::Color{255, 13, 27, 42}));
                row.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush(
                    Windows::UI::Color{90, 102, 128, 153}));
                row.Child(content);
                OperationalRuntimeJobRows().Children().Append(row);
            }
        }
    }
    std::string summary;
    bool hasOpenSessions{};
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Agents) {
        const auto countAfter = [&snapshot](std::string_view prefix) {
            for (const auto& line : snapshot.lines)
                if (line.starts_with(prefix)) return winrt::to_hstring(
                    line.substr(prefix.size()));
            return winrt::hstring{L"—"};
        };
        OperationalAgentDefinitionCount().Text(countAfter("Agent definitions: "));
        OperationalAgentOpenCount().Text(countAfter("Open sessions: "));
        OperationalAgentRecentCount().Text(countAfter("Recent sessions: "));
    }
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Diagnostics) {
        const auto passed = std::count_if(snapshot.lines.begin(), snapshot.lines.end(),
            [](const auto& line) { return line.starts_with("PASS "); });
        const auto failed = std::count_if(snapshot.lines.begin(), snapshot.lines.end(),
            [](const auto& line) { return line.starts_with("FAIL "); });
        OperationalDoctorPassed().Text(winrt::to_hstring(std::to_string(passed)));
        OperationalDoctorFailed().Text(winrt::to_hstring(std::to_string(failed)));
        const auto coreHealthy = std::any_of(snapshot.lines.begin(), snapshot.lines.end(),
            [](const auto& line) { return line == "Overall health: healthy"; });
        OperationalDoctorState().Text(coreHealthy
            ? failed == 0 ? L"Core health verified" : L"Core healthy · integrations unavailable"
            : L"Core health needs attention");
    }
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
        if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes &&
            (line.starts_with("Effective shell policy: ") ||
             line.starts_with("Job inventory: ") ||
             line.starts_with("Job "))) continue;
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
    if (winrt::to_string(PageTitle().Text()) == "Events & Evidence")
        OperationalEvidenceAuditCount().Text(winrt::to_hstring(
            std::to_string(operationalLines_.size()) + " bounded outcomes"));
    OperationalListSummary().Text(winrt::to_hstring(summary.empty()
        ? std::to_string(operationalLines_.size()) + " records from the live Manager projection"
        : summary));
    OperationalCount().Text(winrt::to_hstring(
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? winrt::to_string(PageTitle().Text()) == "Events & Evidence"
                ? std::string{"AUDIT ONLY"}
                : std::to_string(visibleOperationalIndices_.size()) + " AUDIT"
            : snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes
                ? std::string{"LIVE RESOURCE STATE"}
            : std::to_string(visibleOperationalIndices_.size()) + " RECORDS"));
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes ||
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Manager) {
        OperationalListViewport().Height(std::clamp(
            20.0 + static_cast<double>(visibleOperationalIndices_.size()) * 40.0,
            110.0, 235.0));
    }
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
        action == Action::ProjectRemember || action == Action::ProjectUpdate ||
        action == Action::ProjectForget ||
        action == Action::ProjectArchiveExport ||
        action == Action::ProjectArchivePreview ||
        action == Action::ProjectArchiveImport;
    const bool lmStudioAction = action == Action::LmStudioInspect ||
        action == Action::LmStudioRepair || action == Action::LmStudioActivate;
    const bool toolsAction = action == Action::ToolsList || action == Action::ToolInvoke;
    const bool operationalAction = action == Action::OperationalInspect ||
        action == Action::OperationalPrune || action == Action::OperationalClose;
    const bool historyAction = action == Action::RunHistory;
    const bool evidenceAction = action == Action::EvidenceLoad;
    const bool settingsAction = action == Action::SettingsLoad ||
        action == Action::SettingsSave || action == Action::SettingsTest ||
        action == Action::SettingsRestart;
    const bool maintenanceAction = action == Action::MaintenanceReset;
    std::string runProject;
    std::string runClient;
    std::string runTask;
    std::string runId;
    std::uint64_t runGeneration{};
    bool runAllowTools{};
    std::string projectPath;
    std::string projectDisplayName;
    std::string projectQuery;
    std::string requestedProjectId;
    std::string memoryTitle;
    std::string memorySummary;
    std::string memoryBody;
    std::vector<std::string> memoryTags;
    std::string toolProject;
    std::string toolName;
    std::string toolArguments;
    std::string editedProjectId;
    std::string editedRecordId;
    std::string archiveProject;
    std::string archivePath;
    std::string archiveChecksum;
    std::string operationalSessionId;
    std::string operationalSummary;
    const auto requestedOperationalArea = operationalArea_;
    const auto requestedOperationalProject = requestedOperationalArea ==
        ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes &&
        !selectedProjectId_.empty()
            ? std::optional<std::string>{selectedProjectId_} : std::nullopt;
    std::string requestedHistoryProject;
    const std::string requestedEvidenceProject = selectedProjectId_;
    if (evidenceAction && requestedEvidenceProject.empty()) {
        evidenceSnapshot_.reset();
        OperationalEvidenceRunRows().Children().Clear();
        OperationalEvidenceArtifactCount().Text(L"Choose a project");
        OperationalEvidenceVerifyState().Text(L"No run selected");
        OperationalEvidenceSelectedState().Text(L"Choose a project in Projects first.");
        OperationalEvidenceProject().Text(
            L"Select an authorized project in Projects to inspect durable run evidence.");
        OperationalEvidenceState().Text(L"No project identity is selected.");
        co_return;
    }
    ::ForgeConductor::Manager::ManagerMaintenanceScope maintenanceScope{
        ::ForgeConductor::Manager::ManagerMaintenanceScope::ProjectMemory};
    std::optional<std::string> maintenanceProject;
    std::string maintenanceToken;
    if (action == Action::Refresh) {
        runId = winrt::to_string(RunId().Text());
    }
    if (action == Action::ProviderSave || action == Action::ProviderTest ||
        action == Action::ProviderModels || action == Action::ProviderContract) {
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
            runAllowTools = RunAllowNativeTools().IsOn();
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
            requestedProjectId = selectedProjectId_;
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
        toolProject = selectedProjectId_;
        toolName = winrt::to_string(ToolName().Text());
        const auto index = ToolList().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= visibleTools_.size() ||
            tools_[visibleTools_[static_cast<std::size_t>(index)]].name != toolName) {
            ToolsState().Text(L"Select a registered capability from the catalog before invoking it.");
            co_return;
        }
        if (toolProject.empty()) {
            ToolsState().Text(L"Select an authorized project in Projects before invoking a tool.");
            co_return;
        }
        const auto canonical = ToolCanonicalArguments();
        if (!canonical) {
            ToolsState().Text(L"Review the required tool arguments before invoking.");
            co_return;
        }
        toolArguments = *canonical;
    }
    if (historyAction) {
        requestedHistoryProject = selectedProjectId_;
        if (requestedHistoryProject.empty()) {
            RunHistoryRows().Children().Clear();
            RunHistoryState().Text(L"Choose a project to inspect its recent Manager-owned runs.");
            co_return;
        }
    }
    if (action == Action::ProjectUpdate || action == Action::ProjectForget) {
        if (!selectedMemoryRecord_ || selectedProjectId_.empty() ||
            selectedMemoryProjectId_ != selectedProjectId_) {
            ProjectMemoryActionState().Text(
                L"Select a current record in the authorized project first.");
            co_return;
        }
        if (action == Action::ProjectForget &&
            ProjectForgetConfirmation().Text() != L"FORGET") {
            ProjectMemoryActionState().Text(
                L"Type FORGET exactly before tombstoning this selected record.");
            co_return;
        }
        nlohmann::json arguments = {
            {"project_id", selectedProjectId_},
            {"id", selectedMemoryRecord_->id.value()}};
        editedProjectId = selectedProjectId_;
        editedRecordId = selectedMemoryRecord_->id.value();
        toolProject = selectedProjectId_;
        toolName = action == Action::ProjectUpdate
            ? "project_memory.update" : "project_memory.forget";
        if (action == Action::ProjectUpdate) {
            const auto title = winrt::to_string(ProjectEditName().Text());
            const auto summary = winrt::to_string(ProjectEditSummary().Text());
            if (title.empty() || summary.empty()) {
                ProjectMemoryActionState().Text(L"Title and summary cannot be empty.");
                co_return;
            }
            arguments["expected_version"] = selectedMemoryRecord_->version;
            arguments["title"] = title;
            arguments["summary"] = summary;
            arguments["body"] = winrt::to_string(ProjectEditBody().Text());
            arguments["tags"] = nlohmann::json::array();
            std::stringstream stream{winrt::to_string(ProjectEditTags().Text())};
            std::string tag;
            while (std::getline(stream, tag, ',')) {
                const auto start = tag.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                const auto end = tag.find_last_not_of(" \t\r\n");
                arguments["tags"].push_back(tag.substr(start, end - start + 1));
            }
        }
        toolArguments = arguments.dump();
    }
    if (action == Action::ProjectArchiveExport ||
        action == Action::ProjectArchivePreview ||
        action == Action::ProjectArchiveImport) {
        archiveProject = selectedProjectId_;
        if (archiveProject.empty()) {
            ProjectArchiveState().Text(L"Select an authorized project before using its archive.");
            co_return;
        }
        toolProject = archiveProject;
        toolName = action == Action::ProjectArchiveExport
            ? "project_memory.export" : "project_memory.import";
        nlohmann::json arguments = {{"project_id", archiveProject}};
        if (action != Action::ProjectArchiveExport) {
            archivePath = winrt::to_string(ProjectArchivePath().Text());
            if (archivePath.empty()) {
                ProjectArchiveState().Text(L"Choose a project-memory export artifact first.");
                co_return;
            }
            arguments["artifact"] = archivePath;
            arguments["preview"] = action == Action::ProjectArchivePreview;
            if (action == Action::ProjectArchiveImport) {
                if (archivePreviewProjectId_ != archiveProject ||
                    archivePreviewPath_ != archivePath ||
                    archivePreviewChecksum_.empty() ||
                    ProjectArchiveConfirmation().Text() != L"IMPORT") {
                    ProjectArchiveState().Text(
                        L"Verify this exact project and artifact, then type IMPORT before applying records.");
                    co_return;
                }
                archiveChecksum = archivePreviewChecksum_;
                arguments["expected_checksum"] = archiveChecksum;
            }
        }
        toolArguments = arguments.dump();
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
            else if (projectAction) {
                ProjectState().Text(queued);
                if (action == Action::ProjectUpdate || action == Action::ProjectForget)
                    ProjectMemoryActionState().Text(queued);
                if (action == Action::ProjectArchiveExport ||
                    action == Action::ProjectArchivePreview ||
                    action == Action::ProjectArchiveImport)
                    ProjectArchiveState().Text(queued);
            }
            else if (lmStudioAction) {
                LmStudioRegistrationState().Text(queued);
                LmStudioActionStatus().Text(queued);
                LmStudioOverview().Text(L"Native command queued behind current Manager work");
                LmStudioBadge().Text(L"QUEUED");
            }
            else if (toolsAction) ToolsState().Text(queued);
            else if (operationalAction) OperationalState().Text(queued);
            else if (historyAction) RunHistoryState().Text(queued);
            else if (evidenceAction) OperationalEvidenceState().Text(queued);
            else if (settingsAction) SettingsState().Text(queued);
            else if (maintenanceAction) MaintenanceState().Text(queued);
            else if (action == Action::ProviderLoad ||
                      action == Action::ProviderSave ||
                      action == Action::ProviderTest ||
                      action == Action::ProviderModels ||
                      action == Action::ProviderContract) {
                ProviderState().Text(queued);
                if (action == Action::ProviderContract) ProviderContractState().Text(queued);
            }
            else ManagerState().Text(queued);
        } else if (admission ==
                ::ForgeConductor::Hosts::App::AppActionAdmission::Rejected) {
            GenericState().Text(
                L"The bounded Manager command queue is full; this action was not accepted. Retry after the current command completes.");
            if (lmStudioAction) {
                LmStudioActionStatus().Text(L"Manager command queue full · retry after current work");
                LmStudioOverview().Text(L"Native command was not accepted");
                LmStudioBadge().Text(L"WAITING");
            }
        }
        co_return;
    }
    winrt::apartment_context ui;
    if (runAction) {
        RunState().Text(L"Contacting the Manager…");
    } else if (projectAction) {
        ProjectState().Text(L"Contacting the Manager…");
        if (action == Action::ProjectUpdate || action == Action::ProjectForget)
            ProjectMemoryActionState().Text(L"Validating exact project and record binding…");
        if (action == Action::ProjectArchiveExport)
            ProjectArchiveState().Text(L"Exporting a bounded, redacted project snapshot…");
        else if (action == Action::ProjectArchivePreview)
            ProjectArchiveState().Text(L"Verifying project scope and artifact checksum without changes…");
        else if (action == Action::ProjectArchiveImport)
            ProjectArchiveState().Text(L"Rechecking preview checksum, then importing exact project records…");
    } else if (lmStudioAction) {
        const auto pending = action == Action::LmStudioRepair
            ? L"Repairing native registration · preserving foreign MCP entries · up to two minutes…"
            : action == Action::LmStudioActivate
                ? L"Launching LM Studio and synchronizing three registered roles…"
                : L"Inspecting native registration without changes…";
        LmStudioRegistrationState().Text(pending);
        LmStudioActionStatus().Text(pending);
        LmStudioOverview().Text(pending);
        LmStudioBadge().Text(action == Action::LmStudioRepair ? L"REPAIRING" : L"INSPECTING");
    } else if (toolsAction) {
        ToolsState().Text(L"Contacting the Manager…");
    } else if (historyAction) {
        RunHistoryState().Text(L"Reading project-bound run history from the Manager…");
    } else if (evidenceAction) {
        OperationalEvidenceState().Text(L"Verifying durable records for the exact selected project…");
    } else if (operationalAction) {
        OperationalState().Text(L"Contacting the Manager…");
    } else if (settingsAction) {
        SettingsState().Text(L"Contacting the Manager…");
    } else if (maintenanceAction) {
        MaintenanceState().Text(L"The Manager is fencing the selected data scope…");
    } else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
        action == Action::ProviderTest || action == Action::ProviderModels ||
        action == Action::ProviderContract) {
        ProviderState().Text(L"Working…");
        if (action == Action::ProviderContract) {
            providerContractCancellation_ = std::stop_source{};
            ProviderContractCancelButton().IsEnabled(true);
            ProviderContractState().Text(L"Waiting for a disposable model-only response…");
        }
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
        case Action::ProviderContract:
            message = connection_->probeProviderContract(
                *submitted, providerContractCancellation_.get_token());
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
                std::move(runTask), runAllowTools, cancellation_.get_token());
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
                requestedProjectId, std::move(projectQuery),
                cancellation_.get_token());
            message = projectView.message;
            break;
        case Action::ProjectRemember:
            projectView = connection_->rememberProjectMemory(
                requestedProjectId, std::move(memoryTitle),
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
        case Action::ProjectUpdate:
        case Action::ProjectForget:
        case Action::ProjectArchiveExport:
        case Action::ProjectArchivePreview:
        case Action::ProjectArchiveImport:
            toolOutcomeView = connection_->invokeTool(
                std::move(toolProject), std::move(toolName), std::move(toolArguments),
                cancellation_.get_token());
            message = toolOutcomeView.message;
            break;
        case Action::OperationalInspect:
        case Action::OperationalPrune:
        case Action::OperationalClose:
        case Action::RunHistory:
        case Action::EvidenceLoad: {
            using OpAction = ::ForgeConductor::Manager::ManagerOperationalAction;
            const auto op = action == Action::OperationalPrune ? OpAction::PruneSessions :
                action == Action::OperationalClose ? OpAction::CloseSession : OpAction::Inspect;
            operationalView = connection_->operational(
                evidenceAction ? ::ForgeConductor::Manager::ManagerOperationalArea::Evidence
                    : historyAction ? ::ForgeConductor::Manager::ManagerOperationalArea::Runs
                    : requestedOperationalArea, op, std::move(operationalSessionId),
                std::move(operationalSummary), evidenceAction
                    ? std::optional<std::string>{requestedEvidenceProject}
                    : historyAction
                        ? std::optional<std::string>{requestedHistoryProject}
                        : requestedOperationalProject,
                cancellation_.get_token());
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
                    if (action == Action::RunStart ||
                        (action == Action::RunStatus &&
                         runView.snapshot->record.state !=
                             ::ForgeConductor::Domain::ManagedRunState::Running &&
                         runView.snapshot->record.state !=
                             ::ForgeConductor::Domain::ManagedRunState::Paused)) {
                        followUp = Action::RunHistory;
                    }
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
            if (projectView.loaded && projectView.snapshot &&
                ((action != Action::ProjectLoad && action != Action::ProjectRemember) ||
                    (selectedProjectId_ == requestedProjectId &&
                     projectView.snapshot->project.id.value() == requestedProjectId))) {
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
            if (action == Action::ProjectUpdate || action == Action::ProjectForget) {
                const auto bindingCurrent = selectedProjectId_ == editedProjectId &&
                    selectedMemoryProjectId_ == editedProjectId &&
                    selectedMemoryRecord_ &&
                    selectedMemoryRecord_->id.value() == editedRecordId;
                if (!bindingCurrent) {
                    ProjectState().Text(
                        L"A previous project-memory command returned after the selection changed. The current editor was left untouched; refresh to inspect the stored result.");
                } else {
                    ProjectState().Text(winrt::to_hstring(message));
                    ProjectMemoryActionState().Text(winrt::to_hstring(message));
                }
                if (bindingCurrent && toolOutcomeView.snapshot &&
                    toolOutcomeView.snapshot->ok &&
                    toolOutcomeView.snapshot->projectId.value() == editedProjectId) {
                    selectedMemoryRecord_.reset();
                    selectedMemoryProjectId_.clear();
                    ProjectForgetConfirmation().Text(L"");
                    ProjectEditCard().Visibility(Visibility::Collapsed);
                    followUp = Action::ProjectLoad;
                }
            } else if (action == Action::ProjectArchiveExport ||
                action == Action::ProjectArchivePreview ||
                action == Action::ProjectArchiveImport) {
                const auto bindingCurrent = archiveProject == selectedProjectId_ &&
                    (action == Action::ProjectArchiveExport ||
                     archivePath == winrt::to_string(ProjectArchivePath().Text()));
                if (!bindingCurrent) {
                    ProjectArchiveState().Text(
                        L"Archive command returned after the project or artifact changed. The current selection was left untouched.");
                } else {
                    ProjectArchiveState().Text(winrt::to_hstring(message));
                    ProjectState().Text(winrt::to_hstring(message));
                    const auto* outcome = toolOutcomeView.snapshot
                        ? &*toolOutcomeView.snapshot : nullptr;
                    if (!outcome || !outcome->ok ||
                        outcome->projectId.value() != archiveProject ||
                        outcome->toolName != (action == Action::ProjectArchiveExport
                            ? "project_memory.export" : "project_memory.import")) {
                        if (action == Action::ProjectArchivePreview) {
                            ClearArchivePreview();
                            ProjectArchivePreviewState().Text(winrt::to_hstring(
                                "Verification failed · " + message));
                        } else if (action == Action::ProjectArchiveImport) {
                            ClearArchivePreview();
                            ProjectArchivePreviewState().Text(winrt::to_hstring(
                                "Import not confirmed · " + message +
                                ". Verify again before retrying."));
                        } else {
                            ProjectArchiveExportState().Text(winrt::to_hstring(
                                "Export unavailable · " + message));
                        }
                    } else {
                        try {
                        const auto payload = nlohmann::json::parse(
                            outcome->canonicalPayload, nullptr, false);
                        const auto valid = payload.is_object() &&
                            payload.value("ok", false) &&
                            payload.value("project_id", std::string{}) == archiveProject;
                        if (!valid) {
                            ClearArchivePreview();
                            ProjectArchiveState().Text(L"The Manager returned an invalid archive readback.");
                        } else if (action == Action::ProjectArchiveExport) {
                            const auto artifact = payload.value("artifact", std::string{});
                            const auto checksum = payload.value("checksum", std::string{});
                            const auto count = payload.value("record_count", 0U);
                            if (artifact.empty() || checksum.size() != 64U) {
                                ProjectArchiveExportState().Text(L"Export readback lacks an artifact or checksum.");
                            } else {
                                ProjectArchiveExportState().Text(winrt::to_hstring(
                                    std::to_string(count) + " records · SHA-256 " + checksum +
                                    "\n" + artifact));
                                ProjectArchivePath().Text(winrt::to_hstring(artifact));
                                ProjectArchivePreviewState().Text(
                                    L"Export created. Verify and preview it before importing.");
                            }
                        } else if (action == Action::ProjectArchivePreview) {
                            const auto checksum = payload.value("checksum", std::string{});
                            const auto count = payload.value("record_count", 0U);
                            const auto importable = payload.value("importable_count", 0U);
                            if (!payload.value("preview", false) ||
                                payload.value("disposition", std::string{}) != "preview" ||
                                checksum.size() != 64U || importable > count) {
                                ClearArchivePreview();
                                ProjectArchivePreviewState().Text(
                                    L"The preview did not return a valid project-bound verification.");
                            } else {
                                archivePreviewProjectId_ = archiveProject;
                                archivePreviewPath_ = archivePath;
                                archivePreviewChecksum_ = checksum;
                                ProjectArchiveImportButton().IsEnabled(importable > 0U);
                                ProjectArchivePreviewState().Text(winrt::to_hstring(
                                    "Verified SHA-256 " + checksum + " · " +
                                    std::to_string(count) + " records, " +
                                    std::to_string(importable) +
                                    " importable. No records changed." +
                                    (importable ? " Type IMPORT to apply." : " Nothing to import.")));
                            }
                        } else {
                            if (!payload.contains("count") ||
                                !payload.at("count").is_number_unsigned() ||
                                !payload.contains("results") ||
                                !payload.at("results").is_array() ||
                                payload.at("results").size() !=
                                    payload.at("count").get<std::size_t>()) {
                                ClearArchivePreview();
                                ProjectArchiveState().Text(
                                    L"Import readback lacks a verified record count. Refresh project memory to inspect it.");
                            } else {
                                const auto count = payload.at("count").get<std::size_t>();
                                std::size_t inserted{};
                                std::size_t deduplicated{};
                                std::size_t updated{};
                                for (const auto& result : payload.at("results")) {
                                    if (!result.is_object()) continue;
                                    const auto disposition = result.value(
                                        "disposition", std::string{});
                                    if (disposition == "inserted") ++inserted;
                                    else if (disposition == "deduplicated") ++deduplicated;
                                    else if (disposition == "updated") ++updated;
                                }
                                ClearArchivePreview();
                                ProjectArchiveConfirmation().Text(L"");
                                ProjectArchivePreviewState().Text(winrt::to_hstring(
                                    "Manager processed " + std::to_string(count) +
                                    " records after checksum revalidation: " +
                                    std::to_string(inserted) + " inserted, " +
                                    std::to_string(deduplicated) + " already present, " +
                                    std::to_string(updated) + " updated. Preview again for another import."));
                                followUp = Action::ProjectLoad;
                            }
                        }
                        } catch (const std::exception&) {
                            ClearArchivePreview();
                            ProjectArchiveState().Text(
                                L"The Manager returned a malformed archive readback. Refresh before another action.");
                        }
                    }
                }
            } else ProjectState().Text(winrt::to_hstring(message));
        } else if (lmStudioAction) {
            if (lmStudioView.snapshot) {
                lmStudioSnapshot_ = *lmStudioView.snapshot;
                ApplyLmStudio(*lmStudioView.snapshot);
            }
            if (!lmStudioView.loaded) {
                if (lmStudioSnapshot_) ApplyLmStudio(*lmStudioSnapshot_);
                LmStudioRegistrationState().Text(winrt::to_hstring(message));
                LmStudioActionStatus().Text(winrt::to_hstring(message));
                if (lmStudioSnapshot_) {
                    LmStudioOverview().Text(winrt::to_hstring(
                        "Command failed · " + message + " · last inspected state retained"));
                    LmStudioBadge().Text(L"ATTENTION");
                } else {
                    LmStudioOverview().Text(winrt::to_hstring(message));
                    LmStudioBadge().Text(L"UNAVAILABLE");
                    LmStudioHostReadiness().Text(L"Host inspection unavailable");
                    LmStudioRegistrationReadiness().Text(L"Registration not read");
                    LmStudioConnectorReadiness().Text(L"Connection not verified");
                }
            }
        } else if (toolsAction) {
            if (toolsView.snapshot) ApplyTools(*toolsView.snapshot);
            ToolsState().Text(winrt::to_hstring(message));
            if (!toolsView.snapshot && tools_.empty()) {
                ToolListViewport().Height(285.0);
                ToolEmptyState().Visibility(Visibility::Visible);
                ToolEmptyBody().Text(winrt::to_hstring(message));
            }
            if (toolOutcomeView.snapshot) {
                RenderToolOutcome(*toolOutcomeView.snapshot, message);
            } else if (action == Action::ToolInvoke) {
                ToolOutcomeRecords().Children().Clear();
                ToolOutcomeStatus().Text(L"Invocation unavailable");
                ToolOutcomeSummary().Text(winrt::to_hstring(message));
                ToolOutcome().Text(L"No canonical Manager payload was returned.");
            }
        } else if (evidenceAction) {
            if (requestedEvidenceProject == selectedProjectId_ &&
                PageTitle().Text() == L"Events & Evidence") {
                if (operationalView.snapshot) ApplyEvidence(*operationalView.snapshot);
                else OperationalEvidenceState().Text(winrt::to_hstring(
                    "Durable evidence readback unavailable · " + message));
            }
        } else if (historyAction) {
            if (requestedHistoryProject == selectedProjectId_) {
                if (operationalView.snapshot) ApplyRunHistory(*operationalView.snapshot);
                else RunHistoryState().Text(winrt::to_hstring(
                    "Run history unavailable · " + message));
            }
        } else if (operationalAction && requestedOperationalArea == operationalArea_ &&
            (requestedOperationalArea !=
                ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes ||
             requestedOperationalProject.value_or("") == selectedProjectId_)) {
            if (operationalView.snapshot) {
                if (feedDisplayPaused_ && operationalView.snapshot->area ==
                    ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
                    pendingFeedSnapshot_ = *operationalView.snapshot;
                } else {
                    ApplyOperational(*operationalView.snapshot);
                }
            } else {
                OperationalState().Text(winrt::to_hstring(message));
                if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes) {
                    OperationalRuntimeReadiness().Text(L"Runtime readback unavailable");
                    OperationalRuntimeOwnership().Text(winrt::to_hstring(message));
                    OperationalRuntimeJobs().Text(L"Job state cannot be inspected until the Manager reconnects.");
                    OperationalRuntimeJobRows().Children().Clear();
                }
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
                SettingsHeroReadback().Text(providerEdited || settingsEdited
                    ? L"Effective Manager readback · pending edits preserved"
                    : L"Effective Manager readback · verified");
                SettingsDashboardSummary().Text(winrt::to_hstring(
                    loaded.settings.dashboardHost + ':' +
                    std::to_string(loaded.settings.dashboardPort)));
                SettingsProviderSummary().Text(winrt::to_hstring(
                    loaded.settings.localModelHost + ':' +
                    std::to_string(loaded.settings.localModelPort)));
                SettingsContextSummary().Text(winrt::to_hstring(
                    std::to_string(loaded.settings.effectiveContextCapacity)));
                SettingsShellSummary().Text(loaded.settings.shellEnabled ? L"ENABLED" : L"DISABLED");
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
            action == Action::ProviderTest || action == Action::ProviderModels ||
            action == Action::ProviderContract) {
            if (action == Action::ProviderModels) {
                providerDiscoveryAttempted_ = true;
                providerDiscoverySucceeded_ = modelsView.loaded;
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
            if (action == Action::ProviderContract) {
                ProviderContractCancelButton().IsEnabled(false);
                ProviderContractState().Text(winrt::to_hstring(message));
            }
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
        if (!failed && OperationalPanel().Visibility() == Visibility::Visible)
            RunAction(Action::OperationalInspect);
        if (!failed && PageTitle().Text() == L"Events & Evidence")
            RunAction(Action::EvidenceLoad);
    }
    if (followUp) RunAction(*followUp);
}
}
