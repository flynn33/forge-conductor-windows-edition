#include "pch.h"
#include "MainWindow.xaml.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"
#include "SetupKnowledge.h"
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
#include <cstdlib>
#include <ctime>
#include <cwctype>
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

[[nodiscard]] Windows::UI::Color evidenceColor(
    const std::string_view verification) noexcept
{
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize,
            &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto systemColor = ::GetSysColor(COLOR_WINDOWTEXT);
        return Windows::UI::Color{255,
            GetRValue(systemColor), GetGValue(systemColor), GetBValue(systemColor)};
    }
    if (verification == "native_check_passed") {
        return Windows::UI::Color{255, 61, 220, 151};
    }
    if (verification == "native_check_failed" ||
        verification == "record_integrity_unverified") {
        return Windows::UI::Color{255, 255, 107, 122};
    }
    return Windows::UI::Color{255, 255, 200, 87};
}

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
    FooterProductVersion().Text(winrt::to_hstring(
        "Version " + currentProductVersion() + " · Windows 11 · x64"));
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
    }
    // Guided setup was retired in 1.3.0. Delete its scoped view-state values
    // after carrying forward the independently persisted project/provider data.
    const auto retiredScope = connection_ ? connection_->viewStateScope() : std::nullopt;
    const auto retiredGuidedMode =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"GuidedModeEnabled", retiredScope);
    const auto retiredGuidedStep =
        ::ForgeConductor::Hosts::App::scopedViewStateValueName(
            L"GuidedModeStep", retiredScope);
    clearSavedText(retiredGuidedMode.c_str());
    clearSavedText(retiredGuidedStep.c_str());
    // The restored page may immediately enqueue a Manager readback. Attach or
    // launch the authenticated Manager first so a cold-launch catalog does not
    // time out behind startup.
    RunAction(Action::Start);
    if (const auto folder = loadSavedText((selectedPageValueName_ + L".SetupFolder").c_str())) {
        SetupFolder().Text(*folder);
        SetupRetryButton().IsEnabled(!folder->empty());
        AutomaticSetupState().Text(L"Your folder is saved. Retry preparation to check current readiness and continue.");
    }
    if (const auto saved = loadSavedText(selectedPageValueName_.c_str())) {
        auto destination = *saved;
        if (destination == L"Guided setup" || destination == L"Projects" ||
            destination == L"Provider" || destination == L"Autonomy" ||
            destination == L"Continuity" || destination == L"LM Studio MCP") {
            destination = L"Workspace";
        } else if (destination == L"Agents" || destination == L"Feed" ||
            destination == L"Events & Evidence" || destination == L"Runtimes" ||
            destination == L"Diagnostics" || destination == L"Manager" ||
            destination == L"Tools") {
            destination = L"Activity";
        }
        if (destination != *saved) {
            storeSavedText(selectedPageValueName_.c_str(), destination);
        }
        const auto items = RootNavigation().MenuItems();
        for (std::uint32_t index{}; index < items.Size(); ++index) {
            const auto item = items.GetAt(index).try_as<
                Microsoft::UI::Xaml::Controls::NavigationViewItem>();
            if (item && unbox_value_or<hstring>(item.Tag(), L"") == destination) {
                RootNavigation().SelectedItem(item);
                break;
            }
        }
    }
    const auto weak = get_weak();
    telemetryTimer_ = Microsoft::UI::Xaml::DispatcherTimer{};
    // A selected project from an older release is not proof of completed setup.
    if (!loadSavedText((selectedPageValueName_ + L".SetupCompleted").c_str()))
        SelectPage(L"Workspace");
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
    const auto narrowPane = width < 1000.0;
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
    SettingsSummaryColumn2().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Star});
    SettingsSummaryColumn3().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Star});
    Grid::SetRow(SettingsContextCard(), compactCards ? 1 : 0);
    Grid::SetColumn(SettingsContextCard(), compactCards ? 0 : 2);
    Grid::SetRow(SettingsShellCard(), compactCards ? 1 : 0);
    Grid::SetColumn(SettingsShellCard(), compactCards ? 1 : 3);
    HeaderProfileColumn().Width(compactCards
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{1.0, GridUnitType::Auto});
    Grid::SetRow(ProfileCard(), compactCards ? 1 : 0);
    Grid::SetColumn(ProfileCard(), compactCards ? 0 : 1);
    ProfileCard().MinWidth(compactCards ? 0.0 : 610.0);
    ProfileCard().Margin(compactCards
        ? Thickness{0.0, 14.0, 0.0, 0.0}
        : Thickness{0.0, 0.0, 0.0, 0.0});

    const auto stackedOperational = width < 1080.0;
    RigHistoryColumn0().Width(stackedOperational
        ? GridLength{1.0, GridUnitType::Star}
        : GridLength{5.0, GridUnitType::Star});
    RigHistoryColumn1().Width(stackedOperational
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{7.0, GridUnitType::Star});
    Grid::SetRow(RigHistoryCard(), stackedOperational ? 1 : 0);
    Grid::SetColumn(RigHistoryCard(), stackedOperational ? 0 : 1);
    RigActivityColumn0().Width(stackedOperational
        ? GridLength{1.0, GridUnitType::Star}
        : GridLength{7.0, GridUnitType::Star});
    RigActivityColumn1().Width(stackedOperational
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{5.0, GridUnitType::Star});

    OperationalGrid().ColumnDefinitions().GetAt(0).Width(GridLength{
        8.0, GridUnitType::Star});
    OperationalGrid().ColumnDefinitions().GetAt(1).Width(stackedOperational
        ? GridLength{0.0, GridUnitType::Pixel}
        : GridLength{4.0, GridUnitType::Star});
    Grid::SetRow(OperationalSupplementPanel(), stackedOperational ? 1 : 0);
    Grid::SetColumn(OperationalSupplementPanel(), stackedOperational ? 0 : 1);
    Grid::SetRow(OperationalRuntimeCard(), 0);
    Grid::SetRow(OperationalRuntimeJobsCard(), stackedOperational ? 2 : 1);
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

void MainWindow::MaintenanceRefreshClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (selectedProjectId_.empty()) {
        MaintenanceState().Text(
            L"Select an authorized project on the Projects page first.");
        return;
    }
    RefreshMaintenanceRecords();
}

void MainWindow::MaintenanceSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    MaintenanceDeleteSelected().IsEnabled(
        MaintenanceRecords().SelectedItems().Size() != 0U);
}

void MainWindow::MaintenanceRowDeleteClicked(
    Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto button =
        sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (!button || !button.Tag()) return;
    ConfirmMaintenanceDelete({
        winrt::to_string(unbox_value<hstring>(button.Tag()))});
}

void MainWindow::MaintenanceDeleteSelectedClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    std::vector<std::string> keys;
    for (const auto& selected : MaintenanceRecords().SelectedItems()) {
        const auto row = selected.try_as<Microsoft::UI::Xaml::FrameworkElement>();
        if (!row || !row.Tag()) continue;
        keys.push_back(winrt::to_string(unbox_value<hstring>(row.Tag())));
    }
    ConfirmMaintenanceDelete(std::move(keys));
}

winrt::fire_and_forget MainWindow::ConfirmMaintenanceDelete(
    std::vector<std::string> keys)
{
    const auto lifetime = get_strong();
    if (keys.empty()) co_return;
    Microsoft::UI::Xaml::Controls::ContentDialog dialog;
    dialog.XamlRoot(Content().XamlRoot());
    dialog.Title(box_value(keys.size() == 1U
        ? L"Delete selected memory record?"
        : L"Delete selected memory records?"));
    dialog.Content(box_value(winrt::to_hstring(
        std::to_string(keys.size()) +
        " selected project-memory record(s) will be forgotten. "
        "This cannot be undone from Saved records.")));
    dialog.PrimaryButtonText(L"Delete");
    dialog.CloseButtonText(L"Cancel");
    dialog.DefaultButton(Microsoft::UI::Xaml::Controls::ContentDialogButton::Close);
    const auto result = co_await dialog.ShowAsync();
    if (result != Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary)
        co_return;
    DeleteMaintenanceRecords(std::move(keys));
}

winrt::fire_and_forget MainWindow::DeleteMaintenanceRecords(
    std::vector<std::string> keys)
{
    const auto lifetime = get_strong();
    const auto projectId = selectedProjectId_;
    if (projectId.empty() || keys.empty()) co_return;
    MaintenanceDeleteSelected().IsEnabled(false);
    MaintenanceRecords().IsEnabled(false);
    MaintenanceState().Text(L"Deleting the confirmed selection through the Manager…");
    winrt::apartment_context ui;
    std::size_t removed{};
    std::string failure;
    try {
        co_await winrt::resume_background();
        for (const auto& key : keys) {
            if (!key.starts_with("memory:")) {
                failure = "Only project-memory rows can be deleted as records.";
                break;
            }
            const auto arguments = nlohmann::json{
                {"project_id", projectId}, {"id", key.substr(7U)}}.dump();
            const auto result = connection_->projectRecord(
                projectId, true, arguments,
                cancellation_.get_token());
            if (!result.snapshot || !result.snapshot->ok) {
                failure = result.message.empty()
                    ? "A project-memory record could not be deleted." : result.message;
                break;
            }
            ++removed;
        }
    } catch (const std::exception& exception) {
        failure = exception.what();
    } catch (...) {
        failure = "The confirmed delete did not complete.";
    }
    try { co_await ui; } catch (...) { co_return; }
    MaintenanceRecords().IsEnabled(true);
    if (selectedProjectId_ != projectId) {
        MaintenanceState().Text(
            L"The project selection changed while deletion was running. Refresh to inspect the result.");
        co_return;
    }
    if (!failure.empty()) {
        RefreshMaintenanceRecords(
            std::to_string(removed) +
            " memory record(s) deleted before the Manager stopped: " + failure);
    } else {
        RefreshMaintenanceRecords(
            std::to_string(removed) + " selected memory record(s) deleted.");
    }
}

winrt::fire_and_forget MainWindow::RefreshMaintenanceRecords(
    std::string completionMessage)
{
    const auto lifetime = get_strong();
    const auto projectId = selectedProjectId_;
    if (projectId.empty()) co_return;
    MaintenanceRecords().IsEnabled(false);
    MaintenanceDeleteSelected().IsEnabled(false);
    MaintenanceState().Text(L"Refreshing saved project records through the Manager…");
    winrt::apartment_context ui;
    ::ForgeConductor::Hosts::App::ProjectWorkspaceView view;
    try {
        co_await winrt::resume_background();
        view = connection_->projectMemory(
            projectId, {}, cancellation_.get_token(), true);
    } catch (const std::exception& exception) {
        view.message = exception.what();
    } catch (...) {
        view.message = "Could not refresh saved project records.";
    }
    try { co_await ui; } catch (...) { co_return; }
    MaintenanceRecords().IsEnabled(true);
    if (selectedProjectId_ != projectId) {
        MaintenanceState().Text(
            L"The project selection changed while records were loading.");
        co_return;
    }
    if (view.loaded && view.snapshot) {
        RenderMaintenanceRecords(*view.snapshot);
        if (!completionMessage.empty())
            MaintenanceState().Text(winrt::to_hstring(completionMessage));
        co_return;
    }
    const auto message = view.message.empty()
        ? std::string{"Could not refresh saved project records."}
        : view.message;
    MaintenanceState().Text(winrt::to_hstring(
        completionMessage.empty()
            ? message
            : completionMessage + " Refresh failed: " + message));
}

void MainWindow::OpenWorkspaceClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Workspace"); }
void MainWindow::OpenActivityClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Activity"); }
void MainWindow::OpenSettingsClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { SelectPage(L"Settings"); }

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

void MainWindow::ShowProjectRegistration()
{
    SelectPage(L"Workspace");
    ProjectRegistrationExpander().IsExpanded(true);
    ProjectRegistrationExpander().StartBringIntoView();
    ProjectPath().Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
}

void MainWindow::SetupFolderClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (!BrowseForProjectFolder()) return;
    SetupFolder().Text(ProjectPath().Text());
    storeSavedText((selectedPageValueName_ + L".SetupFolder").c_str(), SetupFolder().Text());
    RunAction(Action::SetupPrepare);
}

void MainWindow::SetupRetryClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{ RunAction(Action::SetupPrepare); }

void MainWindow::PolicyClicked(Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto button = sender.as<Microsoft::UI::Xaml::Controls::Button>();
    const auto tag = unbox_value<hstring>(button.Tag());
    if (tag == L"browse" || tag == L"browse-file") {
        const bool chooseFolder = tag == L"browse";
        const auto projectId = selectedProjectId_;
        try {
            HWND hwnd{};
            winrt::check_hresult(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
            winrt::com_ptr<::IFileDialog> dialog;
            winrt::check_hresult(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
            DWORD options{};
            winrt::check_hresult(dialog->GetOptions(&options));
            winrt::check_hresult(dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                (chooseFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)));
            winrt::check_hresult(dialog->SetTitle(chooseFolder
                ? L"Choose development-policy repository folder"
                : L"Choose development-policy file"));
            const auto shown = dialog->Show(hwnd);
            if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
            winrt::check_hresult(shown);
            winrt::com_ptr<::IShellItem> folder;
            winrt::check_hresult(dialog->GetResult(folder.put()));
            PWSTR path{};
            winrt::check_hresult(folder->GetDisplayName(SIGDN_FILESYSPATH, &path));
            PolicySource().Text(path);
            ::CoTaskMemFree(path);
            if (projectId.empty()) {
                PolicyState().Text(L"Policy selected. Prepare or select a project, then choose its policy again to activate CLU governance.");
            } else if (selectedProjectId_ != projectId) {
                PolicyState().Text(L"The project selection changed. Choose its policy again to bind it.");
            } else {
                RunAction(Action::PolicyBind);
            }
        } catch (const winrt::hresult_error& error) {
            PolicyState().Text(L"The Windows policy picker failed: " + error.message());
        }
    }
    else if (tag == L"refresh") RunAction(Action::PolicyRefresh);
    else if (tag == L"inspect") RunAction(Action::PolicyInspect);
    else if (tag == L"read") RunAction(Action::PolicyRead);
    else if (tag == L"next") RunAction(Action::PolicyNext);
    else if (tag == L"findings") RunAction(Action::PolicyFindings);
    else if (tag == L"export") RunAction(Action::PolicyExport);
}

void MainWindow::ApplyPolicyView(const ::ForgeConductor::Hosts::App::ProjectPolicyView& view, bool document)
{
    if (!view.loaded) { PolicyState().Text(to_hstring(view.message)); return; }
    try {
        const auto value = nlohmann::json::parse(view.canonicalJson);
        if (document) {
            policyDocumentPath_ = value.at("path").get<std::string>();
            PolicyDocumentText().Text(to_hstring(value.at("content").get<std::string>()));
            policyDocumentOffset_ = value.at("next_offset").get<std::size_t>();
            PolicyState().Text(value.at("complete").get<bool>() ? L"End of pinned document." : L"More content is available. Choose Next part to continue reading.");
            return;
        }
        if (value.contains("findings")) {
            const auto open = std::count_if(value.at("findings").begin(),
                value.at("findings").end(), [](const auto& finding) {
                    return finding.value("state", std::string{}) != "resolved";
                });
            PolicyDocumentText().Text(to_hstring(value.at("findings").dump(2)));
            PolicyState().Text(to_hstring("CLU findings: " +
                std::to_string(open) + " open of " +
                std::to_string(value.at("findings").size()) +
                ". Activity contains correlated evidence and correction history."));
            return;
        }
        if (value.value("schema", std::string{}) ==
            "forge-clu-governance-log-v1") {
            PolicyDocumentText().Text(to_hstring(value.dump(2)));
            PolicyState().Text(L"Redacted CLU governance log is ready for local inspection and copy/export.");
            return;
        }
        policySummaryJson_ = view.canonicalJson;
        policyProject_ = selectedProjectId_;
        PolicySource().Text(to_hstring(value.value("source", "")));
        policyRevision_ = value.value("revision", "");
        policyDocumentOffset_ = 0;
        policyDocumentPath_.clear();
        PolicyDocument().Items().Clear();
        PolicyDocumentText().Text(L"");
        for (const auto& file : value.value("coverage", nlohmann::json::array()))
            PolicyDocument().Items().Append(box_value(to_hstring(file.at("path").get<std::string>())));
        if (PolicyDocument().Items().Size()) PolicyDocument().SelectedIndex(0);
        if (policyRevision_.empty()) { PolicyState().Text(L"No development policy is bound to this project."); return; }
        std::string detail = "CLU governance is active and non-blocking.";
        const auto commit = value.value("commit", "");
        detail += "\nSource: " + value.value("source", "") + "\nCommit: " + (commit.empty() ? "local content revision" : commit) +
            "\nSnapshot: " + policyRevision_ + "\nEntries: " +
            std::to_string(value.value("entry_count", 0U)) +
            " · Coverage gaps: " + std::to_string(value.value("coverage_gap_count", 0U)) +
            " · Open findings: " + std::to_string(value.value("open_findings", 0U)) +
            " · Highest severity: " + value.value("highest_severity", "none");
        PolicyState().Text(to_hstring(detail));
    } catch (const std::exception& error) { PolicyState().Text(to_hstring(std::string{"Policy response needs attention: "} + error.what())); }
}

void MainWindow::SetupCancelClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{ setupCancellation_.request_stop(); }

void MainWindow::ApplySetupProgress(
    const ::ForgeConductor::Application::ProjectSetupSnapshot& snapshot)
{
    using namespace ::ForgeConductor::Application;
    std::string text;
    for (const auto& check : snapshot.checks) {
        const char* stage = check.stage == SetupStage::Manager ? "Manager"
            : check.stage == SetupStage::Project ? "Project"
            : check.stage == SetupStage::Plugins ? "LM Studio plugins"
            : check.stage == SetupStage::Provider ? "Model" : "Connection check";
        const char* state = check.state == SetupState::Ready ? "Ready"
            : check.state == SetupState::Running ? "Working"
            : check.state == SetupState::NeedsAction ? "Needs attention"
            : check.state == SetupState::Cancelled ? "Cancelled" : "Waiting";
        text += std::string{stage} + " · " + state;
        if (!check.detail.empty()) text += " — " + check.detail;
        text += "\n";
    }
    if (snapshot.ready) text += "\nPreparation complete. Next: add your policy in step 2 (optional), then describe your task in step 3.";
    AutomaticSetupState().Text(winrt::to_hstring(text));
}

void MainWindow::HelpSearchChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    using namespace Microsoft::UI::Xaml::Controls;
    if (!HelpArticles()) return;
    auto lower = [](std::wstring text) {
        std::transform(text.begin(), text.end(), text.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
        return text;
    };
    const auto query = lower(std::wstring{HelpSearch().Text()});
    HelpArticles().Children().Clear();
    std::size_t count{};
    for (const auto& article : ::ForgeConductor::Hosts::App::SetupKnowledge) {
        const auto searchable = lower(std::wstring{article.title} + L" " + std::wstring{article.body});
        if (!query.empty() && searchable.find(query) == std::wstring::npos) continue;
        Expander entry;
        entry.Header(box_value(hstring{article.title}));
        entry.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
        TextBlock body;
        body.Text(hstring{article.body});
        body.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        body.IsTextSelectionEnabled(true);
        entry.Content(body);
        HelpArticles().Children().Append(entry);
        ++count;
    }
    HelpSearchState().Text(count == 0U
        ? L"No matching article. Try project, model, policy, tools, or memory."
        : winrt::to_hstring(count) + L" help articles · available offline");
}

void MainWindow::ProjectRegisterClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::ProjectRegister);
}

bool MainWindow::BrowseForProjectFolder()
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
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> folder;
        winrt::check_hresult(dialog->GetResult(folder.put()));
        PWSTR path{};
        winrt::check_hresult(folder->GetDisplayName(SIGDN_FILESYSPATH, &path));
        ProjectPath().Text(path);
        ::CoTaskMemFree(path);
        ProjectState().Text(L"Folder chosen. Register it to authorize this work scope.");
        return true;
    } catch (const winrt::hresult_error& error) {
        ProjectState().Text(L"The Windows folder picker failed: " + error.message());
        return false;
    }
}
void MainWindow::ProjectBrowseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    static_cast<void>(BrowseForProjectFolder());
}
void MainWindow::ProjectRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectList); }
void MainWindow::ProjectSearchClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectLoad); }
void MainWindow::ProjectDisplayAllClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (selectedProjectId_.empty()) {
        ProjectState().Text(L"Select or register a project first.");
        return;
    }
    ProjectMemoryQuery().Text(L"");
    RunAction(Action::ProjectDisplayAll);
}
void MainWindow::ProjectMemorySelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = ProjectMemoryRecords().SelectedIndex();
    if (index < 0 ||
        static_cast<std::size_t>(index) >= visibleMemoryRecords_.size()) {
        return;
    }
    SelectMemoryRecord(visibleMemoryRecords_[static_cast<std::size_t>(index)]);
}
void MainWindow::ProjectRememberClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectRemember); }
void MainWindow::InstructionPackagePreviewClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::InstructionPackagePreview);
}
void MainWindow::InstructionPackageActivateClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunAction(Action::InstructionPackageActivate);
}
void MainWindow::InstructionPackagePathChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
{
    ClearInstructionPackagePreview();
    InstructionPackageState().Text(
        L"Package selection changed. Validate this folder before activation.");
}
void MainWindow::InstructionPackageBrowseClicked(
    Windows::Foundation::IInspectable const&,
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
        winrt::check_hresult(dialog->SetTitle(
            L"Choose a project instruction package folder"));
        const auto shown = dialog->Show(hwnd);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        winrt::check_hresult(shown);
        winrt::com_ptr<::IShellItem> folder;
        winrt::check_hresult(dialog->GetResult(folder.put()));
        PWSTR path{};
        winrt::check_hresult(folder->GetDisplayName(SIGDN_FILESYSPATH, &path));
        InstructionPackagePath().Text(path);
        ::CoTaskMemFree(path);
        InstructionPackageState().Text(
            L"Folder chosen. Validate it through the Manager before activation.");
    } catch (const winrt::hresult_error& error) {
        InstructionPackageState().Text(
            L"The Windows folder picker failed: " + error.message());
    }
}
void MainWindow::InstructionQueueRefreshClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::List);
}
void MainWindow::InstructionQueueMoveUpClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto index = InstructionPackageQueue().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= instructionQueueRows_.size()) return;
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::Move,
        index == 0 ? 0U : static_cast<std::size_t>(index - 1));
}
void MainWindow::InstructionQueueMoveDownClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto index = InstructionPackageQueue().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= instructionQueueRows_.size()) return;
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::Move,
        (std::min)(instructionQueueRows_.size() - 1U,
            static_cast<std::size_t>(index + 1)));
}
void MainWindow::InstructionQueueRetryClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::Retry);
}
void MainWindow::InstructionQueueRemoveClicked(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::Remove);
}
void MainWindow::InstructionQueueSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = InstructionPackageQueue().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= instructionQueueRows_.size()) return;
    const auto& row = instructionQueueRows_[index];
    InstructionPackageRevision().Text(winrt::to_hstring(
        row.revision.value().substr(0U, 16U) + "…"));
    InstructionPackageFileCount().Text(winrt::to_hstring(
        std::to_string(row.entryCount) + " entries"));
    InstructionPackageFiles().Text(winrt::to_hstring(
        "Source: " + row.packagePath.value() + "\nState: " + row.state +
        " · Cursor: " + std::to_string(row.cursorEntry) + "/" +
        std::to_string(row.entryCount) + " · Coverage gaps: " +
        std::to_string(row.coverageGapCount) +
        (row.lastError ? "\nNeeds attention: " + *row.lastError : "")));
}

void MainWindow::InstructionQueueDragItemsStarting(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::DragItemsStartingEventArgs const& args)
{
    draggedInstructionQueueRowId_.reset();
    if (args.Items().Size() != 1U) return;
    const auto dragged = args.Items().GetAt(0);
    std::uint32_t index{};
    if (!InstructionPackageQueue().Items().IndexOf(dragged, index) ||
        index >= instructionQueueRows_.size()) {
        return;
    }
    draggedInstructionQueueRowId_ = instructionQueueRows_[index].queueRowId;
}

void MainWindow::InstructionQueueDragItemsCompleted(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const& args)
{
    if (!draggedInstructionQueueRowId_ || args.Items().Size() != 1U) return;
    const auto dragged = args.Items().GetAt(0);
    std::uint32_t target{};
    if (!InstructionPackageQueue().Items().IndexOf(dragged, target)) {
        draggedInstructionQueueRowId_.reset();
        RunInstructionQueueAction(
            ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::List);
        return;
    }
    auto rowId = std::move(draggedInstructionQueueRowId_);
    draggedInstructionQueueRowId_.reset();
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::Move,
        static_cast<std::size_t>(target), std::move(rowId));
}

winrt::fire_and_forget MainWindow::RunInstructionQueueAction(
    const ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction action,
    std::optional<std::size_t> targetOrder,
    std::optional<std::string> rowIdOverride)
{
    auto lifetime = get_strong();
    if (!connection_ || selectedProjectId_.empty() || cancellation_.stop_requested()) co_return;
    auto project = ::ForgeConductor::Domain::ProjectId::parse(selectedProjectId_);
    if (!project) {
        InstructionPackageState().Text(winrt::to_hstring(project.error().message));
        co_return;
    }
    auto rowId = std::move(rowIdOverride);
    if (action != ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::List &&
        !rowId) {
        const auto index = InstructionPackageQueue().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= instructionQueueRows_.size()) {
            InstructionPackageState().Text(L"Select an instruction-package queue row first.");
            co_return;
        }
        rowId = instructionQueueRows_[index].queueRowId;
    }
    ::ForgeConductor::Manager::ManagerInstructionPackageQueueRequest request{
        project.value(), action, std::move(rowId), std::move(targetOrder)};
    InstructionPackageState().Text(L"Updating the ordered instruction-package queue…");
    winrt::apartment_context ui;
    ::ForgeConductor::Hosts::App::InstructionPackageQueueView view;
    co_await winrt::resume_background();
    view = connection_->instructionPackageQueue(
        std::move(request), cancellation_.get_token());
    try { co_await ui; } catch (...) { co_return; }
    if (!view.loaded || !view.snapshot) {
        InstructionPackageState().Text(winrt::to_hstring(view.message));
        co_return;
    }
    ApplyInstructionQueue(*view.snapshot);
    InstructionPackageState().Text(winrt::to_hstring(view.message));
}

void MainWindow::ApplyInstructionQueue(
    const ::ForgeConductor::Manager::ManagerInstructionPackageQueueSnapshot& snapshot)
{
    instructionQueueRows_ = snapshot.rows;
    InstructionPackageQueue().Items().Clear();
    for (const auto& row : instructionQueueRows_) {
        InstructionPackageQueue().Items().Append(box_value(winrt::to_hstring(
            std::to_string(InstructionPackageQueue().Items().Size() + 1U) + ". " + row.packageName +
            " · " + row.state + " · " + std::to_string(row.coverageGapCount) +
            " coverage gap(s)")));
    }
    if (!instructionQueueRows_.empty()) InstructionPackageQueue().SelectedIndex(0);
    else {
        InstructionPackageRevision().Text(L"No package selected");
        InstructionPackageFileCount().Text(L"0 entries");
        InstructionPackageFiles().Text(L"Add a folder to create the first queue row.");
    }
}
void MainWindow::ProjectUpdateClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectUpdate); }
void MainWindow::ProjectForgetClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::ProjectForget); }
void MainWindow::ProjectEditCloseClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    selectedMemoryRecord_.reset();
    selectedMemoryProjectId_.clear();
    ProjectMemoryRecords().SelectedIndex(-1);
    ProjectForgetConfirmation().Text(L"");
    ProjectEditCard().Visibility(Visibility::Collapsed);
}

void MainWindow::LmStudioInspectClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioInspect); }
void MainWindow::LmStudioRepairClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioRepair); }
void MainWindow::LmStudioActivateClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::LmStudioActivate); }

void MainWindow::OperationalRefreshClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) { RunAction(Action::OperationalInspect); }
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

void MainWindow::EvidenceVerifyClicked(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (!evidenceSnapshot_ || selectedEvidenceRunId_.empty() ||
        selectedEvidenceProjectId_ != selectedProjectId_) {
        OperationalEvidenceState().Text(
            L"Select a current exact-project completed run before checking it.");
        return;
    }
    const auto approval = OperationalEvidenceCheckApproval().IsChecked();
    if (!approval || !approval.Value() ||
        OperationalEvidenceCheckCommand().Text().empty()) {
        OperationalEvidenceState().Text(
            L"Enter a native check command and explicitly approve its workspace execution.");
        return;
    }
    RunAction(Action::EvidenceVerify);
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
void MainWindow::OperationalFeedCategoryChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    if (operationalArea_ == ::ForgeConductor::Manager::ManagerOperationalArea::Feed &&
        operationalSnapshot_) {
        const auto snapshot = *operationalSnapshot_;
        ApplyOperational(snapshot);
    }
}
void MainWindow::OperationalFeedProjectChanged(Windows::Foundation::IInspectable const&,
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
                    : fields[field].starts_with("project ")
                    ? "\nProject identity: " + fields[field].substr(8)
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
        selectedMemoryRecord_.reset();
        selectedMemoryProjectId_.clear();
        ProjectEditCard().Visibility(Visibility::Collapsed);
    }
    selectedProjectId_ = nextProjectId;
    AutomaticContinuityState().Text(
        L"Reading this project/provider preference from the Manager…");

    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);

    RunAction(Action::ProjectLoad);
    RunAction(Action::ContinuityRead);
    RunAction(Action::LmStudioInspect);
}

void MainWindow::NavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
{
    const auto item = args.SelectedItem().try_as<
        Microsoft::UI::Xaml::Controls::NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"Workspace");
    PageTitle().Text(tag);
    if (telemetryUiInitialized_) {
        storeSavedText(selectedPageValueName_.c_str(), tag);
        MainScrollViewer().ChangeView(nullptr, 0.0, nullptr, true);
    }
    const bool workspace = tag == L"Workspace";
    const bool settings = tag == L"Settings";
    const bool rig = tag == L"Rig";
    const bool continuity = tag == L"Continuity";
    ContinuityPacketsPanel().Visibility(continuity ? Visibility::Visible : Visibility::Collapsed);
    if (continuity) RefreshContinuityPackets();
    const bool activity = tag == L"Activity";
    if (activity) operationalArea_ =
        ::ForgeConductor::Manager::ManagerOperationalArea::Feed;
    if (activity) {
        OperationalHeading().Text(L"Recent tool activity");
        OperationalSubtitle().Text(L"Manager-owned chronological audit outcomes.");
        OperationalListTitle().Text(L"Recent outcomes");
        OperationalHeroIcon().Glyph(L"\uE8D4");
        OperationalPruneButton().Visibility(Visibility::Collapsed);
        OperationalSessionCard().Visibility(Visibility::Collapsed);
        OperationalAgentSearch().Visibility(Visibility::Collapsed);
        OperationalFeedFilters().Visibility(Visibility::Visible);
        OperationalFeedInsightsCard().Visibility(Visibility::Visible);
        OperationalFeedBoundaryCard().Visibility(Visibility::Visible);
        OperationalEvidenceCard().Visibility(Visibility::Collapsed);
        OperationalEvidencePath().Visibility(Visibility::Collapsed);
        OperationalEvidenceRunsCard().Visibility(Visibility::Collapsed);
        OperationalStatusGrid().Visibility(Visibility::Collapsed);
        OperationalListCard().Visibility(Visibility::Visible);
        OperationalRuntimeCard().Visibility(Visibility::Collapsed);
        OperationalRuntimeJobsCard().Visibility(Visibility::Collapsed);
        OperationalDetailCard().Visibility(Visibility::Visible);
        OperationalListViewport().Height(545.0);
        OperationalManagerCard().Visibility(Visibility::Collapsed);
        OperationalRuntimePolicyCard().Visibility(Visibility::Collapsed);
        OperationalAgentInsightsCard().Visibility(Visibility::Collapsed);
        OperationalDiagnosticsCard().Visibility(Visibility::Collapsed);
        OperationalCards().Visibility(Visibility::Collapsed);
        OperationalList().Visibility(Visibility::Visible);
        OperationalEmptyTitle().Text(L"Live inventory unavailable");
        OperationalEmptyBody().Text(L"Connect to the Manager and refresh this view.");
        OperationalCount().Text(L"WAITING");
    }
    WorkspaceReadinessPanel().Visibility(
        (workspace || rig || settings) ? Visibility::Visible : Visibility::Collapsed);
    WorkspaceProviderSection().Visibility(workspace ? Visibility::Visible : Visibility::Collapsed);
    WorkspacePanel().Visibility(workspace ? Visibility::Visible : Visibility::Collapsed);
    ProfileCard().Visibility(Visibility::Visible);

    RigPanel().Visibility(rig ? Visibility::Visible : Visibility::Collapsed);
    WorkspaceProjectSection().Visibility(workspace ? Visibility::Visible : Visibility::Collapsed);
    SettingsConnectorSection().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);

    OperationalPanel().Visibility(activity ? Visibility::Visible : Visibility::Collapsed);
    SettingsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
    if (workspace) {
        HelpSearchChanged(nullptr, nullptr);
        PageDescription().Text(L"Bind the provider, project, instruction queue, CLU governance, and automatic continuity.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
        if (telemetryUiInitialized_ && !providerDiscoveryAttempted_) RunAction(Action::ProviderModels);
        RunAction(Action::ProjectList);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else if (activity) {
        PageDescription().Text(L"Read chronological tool activity, CLU findings, and package events.");
        RunAction(Action::OperationalInspect);
    } else if (continuity) {
        PageDescription().Text(L"Inspect and delete saved LM Studio chat packets.");
    } else if (settings) {
        PageDescription().Text(L"Edit and verify Manager-owned preferences, provider integration, and native tools.");
        RunAction(Action::SettingsLoad);
        RunAction(Action::LmStudioInspect);
        if (telemetryUiInitialized_ && !providerDiscoveryAttempted_) RunAction(Action::ProviderModels);
        if (!selectedProjectId_.empty()) {
        }
        if (selectedProjectId_.empty()) {
            MaintenanceState().Text(
                L"Select a project on the Projects page, then refresh records.");
        } else {
            RefreshMaintenanceRecords();
        }
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
    const auto chatContext = ::ForgeConductor::Hosts::App::MetricPresentation{
        nativeChatContext_.value, nativeChatContext_.state, nativeChatContext_.gaugePercent};
    applyMetric(ContextValue(), ContextState(), ContextGauge(), chatContext);
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
    updateFill(ContextGaugeTrack(), ContextGaugeFill(), chatContext);

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
    ContinuityDot().Fill(configured);
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
    if (page == "Rig") ManagerState().Text(winrt::to_hstring(detail));
    ApplyLmStudioIdentities();
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
        AutomaticContinuityState().Text(
            L"Reading this project/provider preference from the Manager…");

        ProjectHeroName().Text(winrt::to_hstring(project.displayName));
        ProjectHeroScope().Text(project.aliases.empty()
            ? L"Authorized folder pending"
            : winrt::to_hstring(project.aliases.front().value()));
        RunAction(Action::ContinuityRead);
    } else {
        ClearSelectedProject();
        AutomaticContinuityState().Text(
            L"Select a project to persist this preference.");
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
    }
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::List);
}

void MainWindow::AutomaticContinuityToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // XAML raises Toggled while loading the toggle itself, before the following
    // status TextBlock has necessarily been connected.  Defer all persistence
    // and presentation work until the complete visual tree is available.
    const auto continuityState = AutomaticContinuityState();
    if (!continuityState) return;
    if (updatingAutomaticContinuity_) return;
    if (selectedProjectId_.empty()) {
        updatingAutomaticContinuity_ = true;
        AutomaticContinuityToggle().IsOn(true);
        updatingAutomaticContinuity_ = false;
        continuityState.Text(
            L"Select a project before changing automatic continuity.");
        return;
    }
    continuityState.Text(
        L"Saving this project/provider preference through the Manager…");
    RunAction(Action::ContinuitySave);
}

void MainWindow::ClearSelectedProject()
{
    ClearInstructionPackagePreview();
    InstructionPackageRevision().Text(L"No revision activated");
    InstructionPackageFileCount().Text(L"— files");
    InstructionPackageFiles().Text(L"Validated file manifest will appear here.");
    InstructionPackageState().Text(
        L"Choose an authorized project and package folder to begin.");

    selectedMemoryRecord_.reset();
    selectedMemoryProjectId_.clear();
    ProjectEditCard().Visibility(Visibility::Collapsed);
    visibleMemoryRecords_.clear();
    ProjectMemoryRecords().Items().Clear();
    ProjectMemoryEmptyState().Visibility(Visibility::Visible);
    selectedProjectId_.clear();
    clearSavedText(selectedProjectValueName_.c_str());

    MaintenanceRecords().Items().Clear();
    MaintenanceEmptyState().Visibility(Visibility::Visible);
    MaintenanceDeleteSelected().IsEnabled(false);

    MaintenanceProject().Text(L"Select a project on the Projects page.");

    MaintenanceState().Text(
        L"Select the exact project on the Projects page first.");
}

void MainWindow::RenderMaintenanceRecords(
    const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot)
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    MaintenanceRecords().Items().Clear();
    MaintenanceDeleteSelected().IsEnabled(false);
    MaintenanceProject().Text(winrt::to_hstring(
        snapshot.project.displayName + " · " + snapshot.project.id.value()));
    const auto projectId = snapshot.project.id.value();

    const auto appendRow = [this](const std::string& key,
                                  const std::string& titleText,
                                  const std::string& detailText) {
        StackPanel content;
        content.Spacing(7);
        TextBlock title;
        title.Text(winrt::to_hstring(titleText));
        title.FontSize(16);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(TextWrapping::Wrap);
        content.Children().Append(title);
        TextBlock detail;
        detail.Text(winrt::to_hstring(detailText));
        detail.TextWrapping(TextWrapping::Wrap);
        detail.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
            Windows::UI::Color{255,184,197,211}));
        content.Children().Append(detail);
        content.Tag(box_value(winrt::to_hstring(key)));
        content.HorizontalAlignment(HorizontalAlignment::Stretch);
        MaintenanceRecords().Items().Append(content);
    };

    for (const auto& record : snapshot.records) {
        appendRow(
            "memory:" + record.id.value(),
            record.title,
            "Project memory · " + record.kind + " · v" +
                std::to_string(record.version) + " · " + record.summary);
    }

    const bool empty = MaintenanceRecords().Items().Size() == 0U;
    MaintenanceEmptyState().Visibility(
        empty ? Visibility::Visible : Visibility::Collapsed);
    MaintenanceState().Text(winrt::to_hstring(empty
        ? "No project-memory records remain."
        : std::to_string(MaintenanceRecords().Items().Size()) +
            " visible memory record(s). Use Ctrl or Shift to select multiple rows."));
}

void MainWindow::ApplyProjectWorkspace(
    ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot,
    const bool updateMaintenance)
{
    if (selectedMemoryProjectId_ != snapshot.project.id.value()) {
        selectedMemoryRecord_.reset();
        selectedMemoryProjectId_.clear();
        ProjectEditCard().Visibility(Visibility::Collapsed);
    }
    if (!selectedProjectId_.empty() && selectedProjectId_ != snapshot.project.id.value()) {
        ClearInstructionPackagePreview();
        InstructionPackageRevision().Text(L"No revision activated");
        InstructionPackageFileCount().Text(L"— files");
        InstructionPackageFiles().Text(L"Validate a package for this project.");
        InstructionPackageState().Text(
            L"Project selection changed. Validate a package for this exact project.");
    }
    selectedProjectId_ = snapshot.project.id.value();
    const auto existing = std::find_if(projects_.begin(), projects_.end(),
        [&](const auto& project) { return project.id == snapshot.project.id; });
    if (existing == projects_.end()) projects_.push_back(snapshot.project);
    else *existing = snapshot.project;
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(selectedProjectValueName_.c_str(), selected);

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

    if (ProjectMemoryQuery().Text().empty() &&
        instructionPreviewRevision_.empty()) {
        const auto active = std::find_if(
            snapshot.records.begin(), snapshot.records.end(),
            [](const auto& record) {
                return record.kind == "instruction_package" && record.body;
            });
        const auto* activeRecord = snapshot.activeInstructionManifest
            ? &*snapshot.activeInstructionManifest
            : active == snapshot.records.end() ? nullptr : &*active;
        if (activeRecord == nullptr) {
            InstructionPackageRevision().Text(L"No revision activated");
            InstructionPackageFileCount().Text(L"— files");
            InstructionPackageFiles().Text(
                L"Validate a package folder to create the first active revision.");
            InstructionPackageState().Text(
                L"No active instruction manifest is stored for this project.");
        } else {
            try {
                const auto manifest = nlohmann::json::parse(*activeRecord->body);
                const auto revision = manifest.value(
                    "revision", std::string{"unknown"});
                const auto& files = manifest.at("files");
                std::string fileList;
                if (files.is_array()) {
                    for (const auto& file : files) {
                        if (!file.is_object() || !file.contains("path") ||
                            !file.at("path").is_string()) continue;
                        if (!fileList.empty()) fileList += "\n";
                        fileList += "• " + file.at("path").get<std::string>();
                    }
                }
                InstructionPackageRevision().Text(winrt::to_hstring(
                    revision.substr(0U, (std::min)(
                        revision.size(), std::size_t{16U})) +
                    "… · ACTIVE"));
                InstructionPackageFileCount().Text(winrt::to_hstring(
                    std::to_string(files.is_array() ? files.size() : 0U) +
                    " files"));
                InstructionPackageFiles().Text(winrt::to_hstring(fileList));
                InstructionPackageState().Text(winrt::to_hstring(
                    "Active package " + activeRecord->title +
                    " is available to LM Studio through instruction_package.read."));
            } catch (...) {
                InstructionPackageRevision().Text(L"Manifest needs attention");
                InstructionPackageState().Text(
                    L"The active instruction record could not be projected safely. Validate and activate the source folder again.");
            }
        }
    }

    ProjectMemoryRecords().Items().Clear();
    visibleMemoryRecords_ = updateMaintenance
        ? snapshot.records
        : std::move(snapshot.records);
    ProjectMemoryEmptyState().Visibility(visibleMemoryRecords_.empty()
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    for (const auto& record : visibleMemoryRecords_) {
        std::string metadata = record.kind + " · v" +
            std::to_string(record.version);
        if (!record.tags.empty()) {
            metadata += " · ";
            for (std::size_t index{}; index < record.tags.size(); ++index) {
                if (index != 0U) metadata += ", ";
                metadata += record.tags[index];
            }
        }
        ProjectMemoryRecords().Items().Append(box_value(winrt::to_hstring(
            record.title + "\n" + record.summary + "\n" + metadata +
            "\nSelect to inspect and edit.")));
    }
    if (updateMaintenance) RenderMaintenanceRecords(snapshot);
    RunInstructionQueueAction(
        ::ForgeConductor::Manager::ManagerInstructionPackageQueueAction::List);
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
                ? HeaderDataRoot().Text() : L"Unavailable");
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
            ? (snapshot.primaryConnectorReady ? "role host live" : "select in LM Studio chat to use")
            : "not yet verified") +
        (snapshot.primaryToolOutcomeRecorded ? " · tool result recorded" : "")));
    LmStudioFallbackRole().Text(winrt::to_hstring(
        std::string{snapshot.fallbackPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.fallbackConnectorReady ? "role host live" : "select in LM Studio chat to use")
            : "not yet verified") +
        (snapshot.fallbackToolOutcomeRecorded ? " · tool result recorded" : "")));
    LmStudioCluRole().Text(winrt::to_hstring(
        std::string{snapshot.continuityPluginInstalled ? "Installed" : "Missing"} +
        " · " + (snapshot.connectionCheckPerformed
            ? (snapshot.continuityConnectorReady ? "role host live" : "select in LM Studio chat to use")
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

void MainWindow::ClearInstructionPackagePreview()
{
    instructionPreviewProjectId_.clear();
    instructionPreviewPath_.clear();
    instructionPreviewRevision_.clear();
    InstructionPackageActivateButton().IsEnabled(false);
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

void MainWindow::ApplyEvidence(
    const ::ForgeConductor::Manager::ManagerOperationalSnapshot& snapshot)
{
    OperationalEvidenceRunRows().Children().Clear();
    evidenceSnapshot_.reset();
    OperationalEvidenceRunIdentity().Text(L"No run selected");
    OperationalEvidenceProviderIdentity().Text(L"Not recorded");
    OperationalEvidenceTaskHash().Text(L"—");
    OperationalEvidenceOutputHash().Text(L"—");
    OperationalEvidenceIntegrityNote().Text(L"Native record integrity has not been read.");
    OperationalEvidenceTrustNote().Text(
        L"Task outcome requires an independently approved native check.");
    const auto unverifiedBrush =
        Microsoft::UI::Xaml::Media::SolidColorBrush(
            evidenceColor("not_configured"));
    OperationalEvidenceNativeStageLabel().Foreground(unverifiedBrush);
    OperationalEvidenceVerifyState().Foreground(unverifiedBrush);
    OperationalEvidenceTrustNote().Foreground(unverifiedBrush);
    OperationalEvidenceCheckResult().Text(L"No native check attached to this run.");
    OperationalEvidenceCheckButton().IsEnabled(false);
    OperationalEvidenceDigestDetail().Text(L"No exact provenance selected.");
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
            std::to_string(evidenceSnapshot_->lines.size()) + " project run" +
            (evidenceSnapshot_->lines.size() == 1U ? "" : "s")));
        if (evidenceSnapshot_->lines.empty()) {
            selectedEvidenceRunId_.clear();
            selectedEvidenceProjectId_.clear();
            OperationalEvidenceSelectedState().Text(L"No durable run found.");
            OperationalEvidenceVerifyState().Text(L"No run selected");
            OperationalEvidenceState().Text(
                L"Start Manager-owned work to create a durable run. Audit entries alone are not run evidence.");
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
            const auto verification = record.value(
                "task_outcome_verification", std::string{"not_configured"});
            OperationalEvidenceSelectedState().Text(winrt::to_hstring(
                state + " · record seal " + integrity));
            OperationalEvidenceVerifyState().Text(winrt::to_hstring(
                verification == "native_check_passed" ? "Specified check passed" :
                verification == "native_check_failed" ? "Specified check failed" :
                verification == "record_integrity_unverified" ? "Record integrity failed" :
                "Task unverified"));
            const auto verificationBrush =
                Microsoft::UI::Xaml::Media::SolidColorBrush(
                    evidenceColor(verification));
            OperationalEvidenceNativeStageLabel().Foreground(verificationBrush);
            OperationalEvidenceVerifyState().Foreground(verificationBrush);
            OperationalEvidenceTrustNote().Foreground(verificationBrush);
            const bool canCheck = state == "completed" && integrity == "verified" &&
                (!record.contains("native_check") || record["native_check"].is_null() ||
                    (record["native_check"].is_object() &&
                     record["native_check"].value("check_count", 8U) < 8U));
            OperationalEvidenceCheckButton().IsEnabled(canCheck);
            const auto optionalText = [&record](const char* key) {
                return record.contains(key) && record[key].is_string()
                    ? record[key].get<std::string>()
                    : std::string{"not recorded"};
            };
            const auto compactDigest = [&optionalText](const char* key) {
                const auto value = optionalText(key);
                return value.size() == 64U
                    ? value.substr(0U, 16U) + "…" : value;
            };
            OperationalEvidenceRunIdentity().Text(
                winrt::to_hstring(selectedEvidenceRunId_));
            OperationalEvidenceProviderIdentity().Text(winrt::to_hstring(
                optionalText("provider_response_id")));
            OperationalEvidenceTaskHash().Text(winrt::to_hstring(
                compactDigest("task_sha256")));
            OperationalEvidenceOutputHash().Text(winrt::to_hstring(
                compactDigest("stored_output_sha256")));
            OperationalEvidenceIntegrityNote().Text(winrt::to_hstring(
                integrity == "verified"
                    ? "Native durable record seal matches readback. This is metadata consistency, not task success."
                    : integrity == "legacy_unsealed"
                        ? "Legacy run has no native record seal; stored provenance cannot be integrity-checked."
                        : integrity == "mismatch"
                            ? "Native durable record seal mismatch. Do not rely on this record."
                            : "Native record integrity is not yet verified."));
            OperationalEvidenceTrustNote().Text(winrt::to_hstring(record.value(
                "task_outcome_detail", std::string{
                    "No approved native task check was attached; model output is not verified completion."})));
            std::string nativeDetail{
                canCheck ? "No native check attached to this run. The command and its output are not saved in evidence; only the native receipt digests and exit state are." :
                    "Native checks require a completed, sealed run with check capacity."};
            if (record.contains("native_check") &&
                record["native_check"].is_object()) {
                const auto& check = record["native_check"];
                nativeDetail = std::string{check.value("passed", false)
                    ? "Latest specified native check passed" :
                        "Latest specified native check did not pass"} +
                    " · exit " + std::to_string(check.value("exit_code", -1)) +
                    " · " + std::to_string(check.value("elapsed_ms", 0ULL)) +
                    " ms · " + std::to_string(check.value("check_count", 0U)) +
                    " total check(s). A passing check does not certify every assignment requirement.";
            }
            OperationalEvidenceCheckResult().Text(winrt::to_hstring(nativeDetail));
            std::string checkDigests;
            if (record.contains("native_check") &&
                record["native_check"].is_object()) {
                const auto& check = record["native_check"];
                const auto safeString = [&check](const char* key) {
                    return check.contains(key) && check[key].is_string()
                        ? check[key].get<std::string>() : std::string{"not recorded"};
                };
                checkDigests = "\nLatest native check command SHA-256 · " +
                    safeString("command_sha256") +
                    "\nLatest native check stdout SHA-256 · " +
                    safeString("stdout_sha256") +
                    "\nLatest native check stderr SHA-256 · " +
                    safeString("stderr_sha256");
            }
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
                "\nTask outcome verification · " + verification +
                checkDigests +
                "\nOnly the specified native check is verified; model text is not a verified assignment result.";
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
            OperationalStatusValue3().Text(HeaderDataRoot().Text());
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
    if (snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed) {
        std::size_t successful{};
        std::size_t failed{};
        std::size_t denied{};
        std::string latest{"No audited outcome in this bounded window"};
        for (const auto& line : snapshot.lines) {
            const auto fields = dotFields(std::string_view{line}.substr(0, line.find('\n')));
            if (fields.size() < 3U) continue;
            if (latest == "No audited outcome in this bounded window") latest = fields[0];
            if (fields[2] == "ok" || fields[2] == "success") ++successful;
            else if (fields[2] == "error") ++failed;
            else if (fields[2] == "denied") ++denied;
        }
        OperationalFeedSuccessCount().Text(winrt::to_hstring(std::to_string(successful)));
        OperationalFeedErrorCount().Text(winrt::to_hstring(std::to_string(failed)));
        OperationalFeedDeniedCount().Text(winrt::to_hstring(std::to_string(denied)));
        OperationalFeedWindowCount().Text(winrt::to_hstring(
            std::to_string(snapshot.lines.size()) + " bounded Manager outcomes"));
        OperationalFeedLatestTime().Text(winrt::to_hstring(latest));
    }
    auto query = winrt::to_string(OperationalAgentSearch().Text());
    std::transform(query.begin(), query.end(), query.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    auto feedQuery = winrt::to_string(OperationalFeedSearch().Text());
    std::transform(feedQuery.begin(), feedQuery.end(), feedQuery.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const auto statusFilter = OperationalFeedSeverity().SelectedIndex();
    const auto categoryFilter = OperationalFeedCategory().SelectedIndex();
    const auto projectFilter = OperationalFeedProject().SelectedIndex();
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
            const auto tool = fields.size() >= 2U ? std::string_view{fields[1]} :
                std::string_view{};
            const auto category = tool.starts_with("project_memory.") ? 1 :
                tool.starts_with("shell_") ? 2 :
                tool.starts_with("agent_") ? 3 :
                (tool.starts_with("continuity.") || tool.starts_with("clu_")) ? 4 : 5;
            if (categoryFilter > 0 && categoryFilter != category) continue;
            const auto projectField = std::find_if(fields.begin(), fields.end(),
                [](const auto& field) { return field.starts_with("project "); });
            if (projectFilter == 1 &&
                (selectedProjectId_.empty() || projectField == fields.end() ||
                 projectField->substr(8) != selectedProjectId_)) continue;
            if (projectFilter == 2 && projectField != fields.end()) continue;
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
            (projectFilter == 1 ? " · current project" :
             projectFilter == 2 ? " · unscoped" : "") +
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
                ? std::string{"NATIVE EVIDENCE"}
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
        ? !feedQuery.empty() || statusFilter != 0 || categoryFilter != 0 ||
              projectFilter != 0
        : !query.empty();
    OperationalEmptyTitle().Text(!filtered ? L"No records yet" :
        snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? L"No matching audit outcomes" : L"No matching specialists");
    OperationalEmptyBody().Text(!filtered
        ? L"The Manager returned no entries for this view. Refresh to check again."
        : snapshot.area == ::ForgeConductor::Manager::ManagerOperationalArea::Feed
            ? L"Try another tool, client, status, category, or project filter."
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

void MainWindow::ContinuityPacketsClicked(Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    const auto tag = unbox_value<hstring>(sender.as<Microsoft::UI::Xaml::Controls::Button>().Tag());
    const auto selected = ContinuityPackets().SelectedIndex();
    if (tag == L"delete" && (selected < 0 || static_cast<std::size_t>(selected) >= continuityPacketRows_.size())) {
        ContinuityPacketsState().Text(L"Select a packet to delete."); return;
    }
    RefreshContinuityPackets(to_string(tag), tag == L"delete"
        ? continuityPacketRows_[static_cast<std::size_t>(selected)] : std::string{});
}

void MainWindow::ContinuityPacketSelectionChanged(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    const auto index = ContinuityPackets().SelectedIndex();
    ContinuityPacketDetail().Text(index >= 0 && static_cast<std::size_t>(index) < continuityPacketRows_.size()
        ? to_hstring(nlohmann::json::parse(continuityPacketRows_[static_cast<std::size_t>(index)]).dump(2)) : L"");
}

winrt::fire_and_forget MainWindow::RefreshContinuityPackets(std::string action, std::string selected)
{
    const auto lifetime = get_strong();
    if (continuityPacketsBusy_ || !connection_ || cancellation_.stop_requested()) co_return;
    continuityPacketsBusy_ = true;
    ContinuityPacketsPanel().IsHitTestVisible(false);
    winrt::apartment_context ui;
    ::ForgeConductor::Hosts::App::OperationalView view;
    try {
        using Operation = ::ForgeConductor::Manager::ManagerOperationalAction;
        std::string packetId;
        if (!selected.empty()) packetId = nlohmann::json::parse(selected).at("id").get<std::string>();
        co_await winrt::resume_background();
        view = connection_->operational(::ForgeConductor::Manager::ManagerOperationalArea::Continuity,
            action == "delete" ? Operation::DeletePacket : action == "clear" ? Operation::ClearPackets : Operation::Inspect,
            {}, packetId, std::nullopt, cancellation_.get_token());
    } catch (const std::exception& error) { view.message = error.what(); }
    try { co_await ui; } catch (...) { co_return; }
    continuityPacketsBusy_ = false;
    ContinuityPacketsPanel().IsHitTestVisible(true);
    ContinuityPacketsState().Text(to_hstring(view.message));
    if (!view.loaded || !view.snapshot) co_return;
    continuityPacketRows_ = view.snapshot->lines;
    ContinuityPackets().Items().Clear();
    ContinuityPacketDetail().Text(L"");
    for (const auto& line : continuityPacketRows_) {
        const auto packet = nlohmann::json::parse(line);
        ContinuityPackets().Items().Append(box_value(to_hstring(packet.value("goal", std::string{}) +
            " — " + packet.at("id").get<std::string>())));
    }
}

winrt::fire_and_forget MainWindow::RunAction(const Action action)
{
    auto lifetime = get_strong();
    const bool displayAllAction = action == Action::ProjectDisplayAll;
    if (!connection_ || cancellation_.stop_requested()) {
        if (displayAllAction) ProjectDisplayAllButton().IsEnabled(true);
        co_return;
    }
    if (displayAllAction) ProjectDisplayAllButton().IsEnabled(false);

    std::optional<::ForgeConductor::Domain::ManagerSettings> submitted;
    const bool policyAction = action == Action::PolicyBind || action == Action::PolicyRefresh ||
        action == Action::PolicyInspect || action == Action::PolicyRead || action == Action::PolicyNext ||
        action == Action::PolicyFindings || action == Action::PolicyExport;
    std::optional<::ForgeConductor::Contracts::ProjectPolicyRequest> policyRequest;
    if (policyAction) {
        using PolicyAction = ::ForgeConductor::Contracts::ProjectPolicyAction;
        auto project = ::ForgeConductor::Domain::ProjectId::parse(selectedProjectId_);
        if (!project) { PolicyState().Text(L"Prepare or select a project before importing a policy."); co_return; }
        const auto operation = action == Action::PolicyBind ? PolicyAction::Bind : action == Action::PolicyRefresh ? PolicyAction::Refresh
            : action == Action::PolicyInspect ? PolicyAction::Inspect : action == Action::PolicyFindings ? PolicyAction::ListFindings
            : action == Action::PolicyExport ? PolicyAction::ExportLog : PolicyAction::ReadDocument;
        if (operation != PolicyAction::Bind && operation != PolicyAction::Inspect &&
            (policyProject_ != selectedProjectId_ || policyRevision_.empty())) {
            PolicyState().Text(L"Inspect or bind the policy for this selected project first."); co_return;
        }
        policyRequest.emplace(::ForgeConductor::Contracts::ProjectPolicyRequest{project.value(), operation, {}, policyRevision_, {}});
        if (operation == PolicyAction::Bind) policyRequest->source = to_string(PolicySource().Text());
        if (operation == PolicyAction::ReadDocument) {
            if (!PolicyDocument().SelectedItem()) { PolicyState().Text(L"Select an adopted policy document."); co_return; }
            policyRequest->source = to_string(unbox_value<hstring>(PolicyDocument().SelectedItem()));
            if (action == Action::PolicyRead || policyDocumentPath_ != policyRequest->source) policyDocumentOffset_ = 0;
            policyRequest->detailsJson = nlohmann::json{{"offset", policyDocumentOffset_}}.dump();
        }
        PolicyState().Text(L"Updating CLU governance for the selected project…");
    }
    const bool projectReadAction = action == Action::ProjectLoad ||
        action == Action::ProjectDisplayAll;
    const bool projectAction = action == Action::ProjectList ||
        action == Action::ProjectRegister || projectReadAction ||
        action == Action::ProjectRemember || action == Action::ProjectUpdate ||
        action == Action::ProjectForget ||
        action == Action::InstructionPackagePreview ||
        action == Action::InstructionPackageActivate;
    const bool lmStudioAction = action == Action::LmStudioInspect ||
        action == Action::LmStudioRepair || action == Action::LmStudioActivate;
    const bool operationalAction = action == Action::OperationalInspect ||
        action == Action::OperationalPrune || action == Action::OperationalClose;
    const bool evidenceAction = action == Action::EvidenceLoad ||
        action == Action::EvidenceVerify;
    const bool settingsAction = action == Action::SettingsLoad ||
        action == Action::SettingsSave || action == Action::SettingsTest ||
        action == Action::SettingsRestart;
    const bool continuityPreferenceAction =
        action == Action::ContinuityRead || action == Action::ContinuitySave;
    std::string projectPath;
    if (action == Action::SetupPrepare) {
        projectPath = winrt::to_string(SetupFolder().Text());
        if (projectPath.empty()) co_return;
    }
    std::string projectDisplayName;
    std::string projectQuery;
    std::string requestedProjectId;
    const std::string requestedContinuityProject =
        continuityPreferenceAction ? selectedProjectId_ : std::string{};
    const std::optional<bool> requestedContinuityEnabled =
        action == Action::ContinuitySave
            ? std::optional<bool>{AutomaticContinuityToggle().IsOn()}
            : std::nullopt;
    std::string memoryTitle;
    std::string memorySummary;
    std::string memoryBody;
    std::vector<std::string> memoryTags;
    std::string toolProject;
    std::string toolName;
    std::string toolArguments;
    std::string editedProjectId;
    std::string editedRecordId;
    std::string instructionProject;
    std::string instructionPath;
    std::string operationalSessionId;
    std::string operationalSummary;
    const auto requestedOperationalArea = operationalArea_;
    const auto requestedOperationalProject = requestedOperationalArea ==
        ::ForgeConductor::Manager::ManagerOperationalArea::Runtimes &&
        !selectedProjectId_.empty()
            ? std::optional<std::string>{selectedProjectId_} : std::nullopt;
    const std::string requestedEvidenceProject = selectedProjectId_;
    const std::string requestedLmStudioProject = selectedProjectId_;
    const std::string requestedEvidenceRun = selectedEvidenceRunId_;
    std::string requestedEvidenceCommand;
    if (continuityPreferenceAction && requestedContinuityProject.empty()) {
        AutomaticContinuityState().Text(
            L"Select a project before reading automatic continuity.");
        co_return;
    }
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
    if (action == Action::EvidenceVerify) {
        const auto approval = OperationalEvidenceCheckApproval().IsChecked();
        if (!evidenceSnapshot_ || requestedEvidenceRun.empty() ||
            selectedEvidenceProjectId_ != requestedEvidenceProject ||
            !approval || !approval.Value()) {
            OperationalEvidenceState().Text(
                L"Refresh and approve a current exact-project run before a native check.");
            co_return;
        }
        requestedEvidenceCommand = winrt::to_string(
            OperationalEvidenceCheckCommand().Text());
        if (requestedEvidenceCommand.empty() ||
            requestedEvidenceCommand.size() > 1'024U) {
            OperationalEvidenceState().Text(
                L"A native check command must be 1–1024 UTF-8 bytes.");
            co_return;
        }
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
        } else if (projectReadAction || action == Action::ProjectRemember) {
            if (selectedProjectId_.empty()) {
                ProjectState().Text(L"Select or register a project first.");
                if (displayAllAction) ProjectDisplayAllButton().IsEnabled(true);
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
    if (action == Action::InstructionPackagePreview ||
        action == Action::InstructionPackageActivate) {
        instructionProject = selectedProjectId_;
        instructionPath = winrt::to_string(InstructionPackagePath().Text());
        if (instructionProject.empty()) {
            InstructionPackageState().Text(
                L"Select an authorized project before validating instructions.");
            co_return;
        }
        if (instructionPath.empty()) {
            InstructionPackageState().Text(
                L"Choose an instruction package folder first.");
            co_return;
        }
        if (action == Action::InstructionPackageActivate &&
            (instructionPreviewProjectId_ != instructionProject ||
             instructionPreviewPath_ != instructionPath ||
             instructionPreviewRevision_.empty())) {
            InstructionPackageState().Text(
                L"Validate this exact project and folder before activation.");
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

    const auto lane = action == Action::Refresh
        ? ::ForgeConductor::Hosts::App::AppActionLane::Observation
        : ::ForgeConductor::Hosts::App::AppActionLane::Command;
    const auto admission = actionScheduler_.admit(
        lane, static_cast<std::size_t>(action));
    if (admission != ::ForgeConductor::Hosts::App::AppActionAdmission::Started) {
        if (admission == ::ForgeConductor::Hosts::App::AppActionAdmission::Queued) {
            const auto queued = L"Queued behind the current Manager command.";
           if (projectAction) {
                ProjectState().Text(queued);
                if (action == Action::ProjectUpdate || action == Action::ProjectForget)
                    ProjectMemoryActionState().Text(queued);

                if (action == Action::InstructionPackagePreview ||
                    action == Action::InstructionPackageActivate)
                    InstructionPackageState().Text(queued);
            }
            else if (lmStudioAction) {
                LmStudioRegistrationState().Text(queued);
                LmStudioActionStatus().Text(queued);
                LmStudioOverview().Text(L"Native command queued behind current Manager work");
                LmStudioBadge().Text(L"QUEUED");
            }

            else if (operationalAction) OperationalState().Text(queued);

            else if (evidenceAction) OperationalEvidenceState().Text(queued);
            else if (settingsAction) SettingsState().Text(queued);

            else if (continuityPreferenceAction)
                AutomaticContinuityState().Text(queued);
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
            ManagerState().Text(
                L"The bounded Manager command queue is full; retry after current work.");
            if (lmStudioAction) {
                LmStudioActionStatus().Text(L"Manager command queue full · retry after current work");
                LmStudioOverview().Text(L"Native command was not accepted");
                LmStudioBadge().Text(L"WAITING");
            }
        }
        if (displayAllAction && admission !=
                ::ForgeConductor::Hosts::App::AppActionAdmission::Queued) {
            ProjectDisplayAllButton().IsEnabled(true);
        }
        co_return;
    }
    winrt::apartment_context ui;
    if (projectAction) {
        ProjectState().Text(L"Contacting the Manager…");
        if (action == Action::ProjectUpdate || action == Action::ProjectForget)
            ProjectMemoryActionState().Text(L"Validating exact project and record binding…");
        if (action == Action::InstructionPackagePreview)
            InstructionPackageState().Text(
                L"Manager is validating every supported file and computing the revision…");
        else if (action == Action::InstructionPackageActivate)
            InstructionPackageState().Text(
                L"Manager is revalidating and binding this revision to the project…");
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
    }   else if (evidenceAction) {
        OperationalEvidenceState().Text(action == Action::EvidenceVerify
            ? L"Running the approved native check and sealing its result for the exact run…"
            : L"Verifying durable records for the exact selected project…");
    } else if (operationalAction) {
        OperationalState().Text(L"Contacting the Manager…");
    } else if (settingsAction) {
        SettingsState().Text(L"Contacting the Manager…");
    }  else if (continuityPreferenceAction) {
        AutomaticContinuityState().Text(action == Action::ContinuitySave
            ? L"Saving this project/provider preference through the Manager…"
            : L"Reading this project/provider preference from the Manager…");
    } else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
        action == Action::ProviderTest || action == Action::ProviderModels ||
        action == Action::ProviderContract) {
        ProviderState().Text(L"Working…");
        if (action == Action::ProviderContract) {
            providerContractCancellation_ = std::stop_source{};
            ProviderContractCancelButton().IsEnabled(true);
            ProviderContractState().Text(L"Waiting for a disposable model-only response…");
        }
    } else if (action != Action::Refresh) {
        ManagerState().Text(L"Connecting…");
    }

    std::string message;
    ::ForgeConductor::Hosts::App::ProviderSettingsView loaded;
    ::ForgeConductor::Hosts::App::ProjectPolicyView policyView;
    ::ForgeConductor::Application::ProjectSetupSnapshot setupResult;
    const auto setupDispatcher = DispatcherQueue();
    const auto setupWeak = get_weak();
    if (action == Action::SetupPrepare) {
        setupCancellation_ = std::stop_source{};
        SetupFolderButton().IsEnabled(false);
        SetupRetryButton().IsEnabled(false);
        SetupCancelButton().IsEnabled(true);
    }
    ::ForgeConductor::Hosts::App::ProviderModelsView modelsView;
    ::ForgeConductor::Hosts::App::TelemetryView telemetryView;
    ::ForgeConductor::Hosts::App::ProjectsView projectsView;
    ::ForgeConductor::Hosts::App::ProjectWorkspaceView projectView;
    ::ForgeConductor::Hosts::App::InstructionPackageView instructionView;
    ::ForgeConductor::Hosts::App::LmStudioView lmStudioView;
    ::ForgeConductor::Hosts::App::ToolOutcomeView toolOutcomeView;
    ::ForgeConductor::Hosts::App::OperationalView operationalView;
    ::ForgeConductor::Hosts::App::AutomaticContinuityPreferenceView
        continuityPreferenceView;
    bool failed{};
    try {
        co_await winrt::resume_background();
        switch (action) {
        case Action::PolicyBind: case Action::PolicyRefresh: case Action::PolicyInspect:
        case Action::PolicyRead: case Action::PolicyNext: case Action::PolicyFindings:
        case Action::PolicyExport:
            policyView = connection_->projectPolicy(*policyRequest, cancellation_.get_token());
            message = policyView.message;
            break;
        case Action::SetupPrepare: {
            std::stop_callback closed{cancellation_.get_token(),
                [source = setupCancellation_]() mutable { source.request_stop(); }};
            setupResult = connection_->prepareProject(projectPath, setupCancellation_.get_token(),
                [setupDispatcher, setupWeak](const auto& progress) {
                    setupDispatcher.TryEnqueue([setupWeak, progress]() {
                        if (const auto window = setupWeak.get()) window->ApplySetupProgress(progress);
                    });
                });
            if (!setupResult.projectId.empty())
                projectView = connection_->projectMemory(setupResult.projectId, {}, cancellation_.get_token());
            if (projectView.loaded && projectView.snapshot)
                policyView = connection_->projectPolicy({projectView.snapshot->project.id,
                    ::ForgeConductor::Contracts::ProjectPolicyAction::Inspect}, cancellation_.get_token());
            loaded = connection_->providerSettings(cancellation_.get_token());
            message = setupResult.ready ? "Project preparation completed." : "Project preparation needs attention.";
            break;
        }
        case Action::Start:
            message = connection_->start(cancellation_.get_token());
            break;
        case Action::Refresh:
            telemetryView = connection_->telemetry(
                {}, cancellation_.get_token());
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
        case Action::ContinuityRead:
        case Action::ContinuitySave:
            continuityPreferenceView = connection_->automaticContinuityPreference(
                requestedContinuityProject, requestedContinuityEnabled,
                cancellation_.get_token());
            message = continuityPreferenceView.message;
            break;
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
        case Action::ProjectDisplayAll:
            projectView = connection_->projectMemory(
                requestedProjectId, std::move(projectQuery),
                cancellation_.get_token(),
                action == Action::ProjectDisplayAll);
            message = projectView.message;
            break;
        case Action::ProjectRemember:
            projectView = connection_->rememberProjectMemory(
                requestedProjectId, std::move(memoryTitle),
                std::move(memorySummary), std::move(memoryBody),
                std::move(memoryTags), cancellation_.get_token());
            message = projectView.message;
            break;
        case Action::InstructionPackagePreview:
        case Action::InstructionPackageActivate:
            instructionView = connection_->instructionPackage(
                instructionProject, instructionPath,
                action == Action::InstructionPackageActivate,
                action == Action::InstructionPackageActivate
                    ? instructionPreviewRevision_ : std::string{},
                cancellation_.get_token());
            message = instructionView.message;
            break;
        case Action::LmStudioInspect:
        case Action::LmStudioRepair:
        case Action::LmStudioActivate: {
            using LmAction = ::ForgeConductor::Hosts::App::LmStudioAction;
            const auto lmAction = action == Action::LmStudioRepair ? LmAction::Repair :
                action == Action::LmStudioActivate ? LmAction::Activate : LmAction::Inspect;
            lmStudioView = connection_->lmStudio(lmAction, cancellation_.get_token(), requestedLmStudioProject);
            message = lmStudioView.message;
            break;
        }
        case Action::ProjectUpdate:
        case Action::ProjectForget:
            toolOutcomeView = connection_->projectRecord(
                std::move(toolProject), action == Action::ProjectForget, std::move(toolArguments),
                cancellation_.get_token());
            message = toolOutcomeView.message;
            break;
        case Action::OperationalInspect:
        case Action::OperationalPrune:
        case Action::OperationalClose:
        case Action::EvidenceLoad:
        case Action::EvidenceVerify: {
            using OpAction = ::ForgeConductor::Manager::ManagerOperationalAction;
            const auto op = action == Action::OperationalPrune ? OpAction::PruneSessions :
                action == Action::OperationalClose ? OpAction::CloseSession :
                action == Action::EvidenceVerify ? OpAction::VerifyTask : OpAction::Inspect;
            operationalView = connection_->operational(
                evidenceAction ? ::ForgeConductor::Manager::ManagerOperationalArea::Evidence
                    : requestedOperationalArea, op,
                action == Action::EvidenceVerify ? requestedEvidenceRun
                    : std::move(operationalSessionId),
                action == Action::EvidenceVerify ? std::move(requestedEvidenceCommand)
                    : std::move(operationalSummary), evidenceAction
                    ? std::optional<std::string>{requestedEvidenceProject}
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
        if (continuityPreferenceAction &&
            requestedContinuityProject == selectedProjectId_) {
            if (continuityPreferenceView.loaded &&
                continuityPreferenceView.preference &&
                continuityPreferenceView.preference->projectId.value() ==
                    requestedContinuityProject) {
                const auto& preference = *continuityPreferenceView.preference;
                updatingAutomaticContinuity_ = true;
                AutomaticContinuityToggle().IsOn(preference.enabled);
                updatingAutomaticContinuity_ = false;
                AutomaticContinuityState().Text(winrt::to_hstring(
                    std::string{preference.enabled ? "Enabled" : "Disabled"} +
                    " for " + preference.providerId +
                    ". Preference saved. Automatic handoff of the visible LM Studio chat is unavailable; tools and agents remain available."));
            } else {
                AutomaticContinuityState().Text(winrt::to_hstring(
                    "Manager preference readback unavailable · " + message));
            }
        }
        if (policyAction) {
            if (policyRequest->projectId.value() != selectedProjectId_) PolicyState().Text(L"The selected project changed. Inspect its policy to continue.");
            else if (failed) PolicyState().Text(to_hstring(message));
            else ApplyPolicyView(policyView, action == Action::PolicyRead || action == Action::PolicyNext);
        }
        if (action == Action::SetupPrepare) {
            ApplySetupProgress(setupResult);
            preparedProjectId_ = setupResult.ready ? setupResult.projectId : std::string{};
            if (setupResult.ready) {
                storeSavedText((selectedPageValueName_ + L".SetupCompleted").c_str(), L"1");
            }
            SetupFolderButton().IsEnabled(true);
            SetupRetryButton().IsEnabled(true);
            SetupCancelButton().IsEnabled(false);
            if (projectView.loaded && projectView.snapshot) ApplyProjectWorkspace(*projectView.snapshot);
            if (policyView.loaded) ApplyPolicyView(policyView, false);
            if (loaded.loaded) ApplyProviderForm(loaded.settings);
        }
       if (projectAction) {
            if (projectsView.loaded && projectsView.snapshot) {
                ApplyProjectList(*projectsView.snapshot);
                if (!selectedProjectId_.empty()) followUp = Action::ProjectLoad;
            }
            if (projectView.loaded && projectView.snapshot &&
                ((!projectReadAction && action != Action::ProjectRemember) ||
                    (selectedProjectId_ == requestedProjectId &&
                     projectView.snapshot->project.id.value() == requestedProjectId))) {
                ApplyProjectWorkspace(
                    *projectView.snapshot,
                    action != Action::ProjectDisplayAll);
                if (action == Action::ProjectRegister) {
                    followUp = Action::ProjectList;
                } else if (action == Action::ProjectRemember) {
                    ProjectMemoryTitle().Text(L"");
                    ProjectMemorySummary().Text(L"");
                    ProjectMemoryBody().Text(L"");
                    ProjectMemoryTags().Text(L"");
                }
            }
            if (action == Action::ProjectRegister && projectView.loaded &&
                projectView.snapshot) {
                ProjectRegistrationExpander().IsExpanded(false);
            }
            if (action == Action::InstructionPackagePreview ||
                action == Action::InstructionPackageActivate) {
                const auto bindingCurrent =
                    instructionProject == selectedProjectId_ &&
                    instructionPath == winrt::to_string(
                        InstructionPackagePath().Text());
                if (!bindingCurrent) {
                    ClearInstructionPackagePreview();
                    InstructionPackageState().Text(
                        L"Instruction validation returned after the project or folder changed. Current controls were left untouched.");
                } else if (!instructionView.loaded || !instructionView.snapshot ||
                    instructionView.snapshot->projectId.value() !=
                        instructionProject) {
                    ClearInstructionPackagePreview();
                    InstructionPackageState().Text(winrt::to_hstring(
                        "Instruction package failed · " + message));
                } else {
                    const auto& package = *instructionView.snapshot;
                    std::string files;
                    for (const auto& file : package.files) {
                        if (!files.empty()) files += "\n";
                        files += "• " + file;
                    }
                    InstructionPackageFiles().Text(winrt::to_hstring(files));
                    InstructionPackageRevision().Text(winrt::to_hstring(
                        package.revision.value().substr(0U, 16U) + "…"));
                    InstructionPackageFileCount().Text(winrt::to_hstring(
                        std::to_string(package.fileCount) + " files · " +
                        std::to_string(package.contentBytes / 1024U) + " KiB"));
                    InstructionPackageState().Text(winrt::to_hstring(message +
                        (package.ignoredFileCount == 0U
                            ? std::string{}
                            : " " + std::to_string(package.ignoredFileCount) +
                                " entry or entries require follow-up interpretation; none were omitted from inventory.")));
                 if (action == Action::InstructionPackagePreview) {
                        instructionPreviewProjectId_ = instructionProject;
                        instructionPreviewPath_ = instructionPath;
                        instructionPreviewRevision_ = package.revision.value();
                        InstructionPackageActivateButton().IsEnabled(true);
                    } else {
                        ClearInstructionPackagePreview();
                        InstructionPackageRevision().Text(winrt::to_hstring(
                            package.revision.value().substr(0U, 16U) +
                            "… · ACTIVE"));
                        followUp = Action::ProjectLoad;
                    }
                    ProjectState().Text(winrt::to_hstring(message));
                }
            } else if (action == Action::ProjectUpdate || action == Action::ProjectForget) {
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
            }  else ProjectState().Text(winrt::to_hstring(message));
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
        }  else if (evidenceAction) {
            if (requestedEvidenceProject == selectedProjectId_ &&
                (action != Action::EvidenceVerify ||
                    requestedEvidenceRun == selectedEvidenceRunId_) &&
                PageTitle().Text() == L"Events & Evidence") {
                if (operationalView.snapshot) {
                    ApplyEvidence(*operationalView.snapshot);
                    if (action == Action::EvidenceVerify) {
                        OperationalEvidenceCheckApproval().IsChecked(false);
                        OperationalEvidenceCheckCommand().Text(L"");
                    }
                }
                else OperationalEvidenceState().Text(winrt::to_hstring(
                    (action == Action::EvidenceVerify
                        ? "Native check unavailable · "
                        : "Durable evidence readback unavailable · ") + message));
            }
        }  else if (operationalAction && requestedOperationalArea == operationalArea_ &&
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
                if (!selectedProjectId_.empty()) {
                    followUp = Action::ContinuityRead;
                }
            } else {
                SettingsState().Text(winrt::to_hstring(message));
                if (action == Action::SettingsSave ||
                    action == Action::SettingsRestart) {
                    followUp = Action::SettingsLoad;
                }
            }
        }  else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
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
                if (!selectedProjectId_.empty()) {
                    followUp = Action::ContinuityRead;
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
                nativeChatContext_ = std::move(telemetryView.nativeChatContext);
                telemetrySnapshot_ = std::move(telemetryView.snapshot);
                ApplyTelemetryPresentation(*telemetrySnapshot_);
            } else if (action == Action::Refresh) {
                ApplyDisconnectedTelemetry(message);
            }
            ManagerState().Text(winrt::to_hstring(message));
        }
    }
    const auto scheduled = actionScheduler_.complete(lane);
    if (displayAllAction) ProjectDisplayAllButton().IsEnabled(true);
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
