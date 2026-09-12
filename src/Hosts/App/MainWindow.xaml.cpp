#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"

#include <algorithm>
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
    L"Software\\Forge Conductor\\Windows Alpha";
constexpr wchar_t SelectedPageValue[] = L"SelectedPage";
constexpr wchar_t SelectedProjectValue[] = L"SelectedProjectId";

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
}

MainWindow::MainWindow() {}

MainWindow::MainWindow(
    std::shared_ptr<::ForgeConductor::Hosts::App::IManagerConnection> connection)
    : connection_{std::move(connection)}
{
}

void MainWindow::WindowClosed(Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::WindowEventArgs const&)
{
    if (telemetryTimer_) telemetryTimer_.Stop();
    cancellation_.request_stop();
}

void MainWindow::WindowContentLoaded(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    if (telemetryUiInitialized_) return;
    telemetryUiInitialized_ = true;
    if (const auto savedProject = loadSavedText(SelectedProjectValue)) {
        selectedProjectId_ = winrt::to_string(*savedProject);
        RunProjectId().Text(*savedProject);
    }
    if (const auto saved = loadSavedText(SelectedPageValue)) {
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
    telemetryTimer_.Interval(std::chrono::seconds{2});
    telemetryTimer_.Tick([weak](auto const&, auto const&) {
        if (const auto self = weak.get()) self->RunAction(Action::Refresh);
    });
    RunAction(Action::Refresh);
    telemetryTimer_.Start();
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

void MainWindow::ProjectSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    if (rebuildingProjects_) return;
    const auto index = ProjectSelector().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= projects_.size()) return;
    selectedProjectId_ = projects_[static_cast<std::size_t>(index)].id.value();
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(SelectedProjectValue, selected);
    RunProjectId().Text(selected);
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
    storeSavedText(SelectedPageValue, tag);
    const bool provider = tag == L"Provider";
    const bool autonomy = tag == L"Autonomy" || tag == L"Continuity";
    const bool rig = tag == L"Rig";
    const bool projects = tag == L"Projects";
    ProviderPanel().Visibility(provider ? Visibility::Visible : Visibility::Collapsed);
    AutonomyPanel().Visibility(autonomy ? Visibility::Visible : Visibility::Collapsed);
    RigPanel().Visibility(rig ? Visibility::Visible : Visibility::Collapsed);
    ProjectsPanel().Visibility(projects ? Visibility::Visible : Visibility::Collapsed);
    GenericPanel().Visibility(
        !provider && !rig && !autonomy && !projects
            ? Visibility::Visible : Visibility::Collapsed);
    if (provider) {
        PageDescription().Text(L"Configure and test the Manager-owned LM Studio Responses endpoint.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else if (autonomy) {
        PageDescription().Text(L"Start, attach, pause, resume, and stop Manager-owned work while observing retained context.");
    } else if (tag == L"Projects") {
        PageDescription().Text(L"Register authorized folders, select exact project identities, and read or write persistent project memory.");
        RunAction(Action::ProjectList);
    } else if (tag == L"Tools" || tag == L"LM Studio MCP") {
        PageDescription().Text(L"Inspect the Manager-owned native and MCP tool catalog.");
    } else if (tag == L"Events & Evidence" || tag == L"Feed") {
        PageDescription().Text(L"Inspect recent operational activity and measured tool durations.");
    } else if (tag == L"Runtimes") {
        PageDescription().Text(L"Inspect the native telemetry runtime, process resources, threads, repositories, and databases.");
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

void MainWindow::ApplyTelemetryPresentation(
    const ::ForgeConductor::Domain::ManagerTelemetrySnapshot& snapshot)
{
    const auto presentation =
        ::ForgeConductor::Hosts::App::makeTelemetryPresentation(snapshot);
    applyMetric(CpuValue(), CpuState(), CpuGauge(), presentation.cpu);
    applyMetric(RamValue(), RamState(), RamGauge(), presentation.ram);
    applyMetric(GpuValue(), GpuState(), GpuGauge(), presentation.gpu);
    applyMetric(
        ContextValue(), ContextState(), ContextGauge(), presentation.context);
    ManagerHealth().Text(winrt::to_hstring(presentation.managerStatus));
    ProviderHealth().Text(winrt::to_hstring(presentation.providerStatus));
    StoreHealth().Text(winrt::to_hstring(presentation.storeStatus));
    ContinuityHealth().Text(winrt::to_hstring(presentation.continuityStatus));

    const auto width = std::max(320.0, HistoryCanvas().ActualWidth());
    constexpr double Height = 132.0;
    CpuHistoryLine().Points(chartPoints(presentation.cpuHistory, width, Height));
    RamHistoryLine().Points(chartPoints(presentation.ramHistory, width, Height));
    if (presentation.cpuHistory.empty()) {
        HistoryEquivalentText().Text(L"No measured CPU/RAM history samples.");
    } else {
        HistoryEquivalentText().Text(winrt::to_hstring(
            std::to_string(presentation.cpuHistory.size()) +
            " measured samples · latest CPU " + presentation.cpu.value +
            " · latest RAM " + presentation.ram.value));
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
    if (selectedIndex < 0 && !projects_.empty()) selectedIndex = 0;
    ProjectSelector().SelectedIndex(selectedIndex);
    rebuildingProjects_ = false;

    if (selectedIndex >= 0) {
        selectedProjectId_ = projects_[static_cast<std::size_t>(selectedIndex)].id.value();
        const auto selected = winrt::to_hstring(selectedProjectId_);
        storeSavedText(SelectedProjectValue, selected);
        RunProjectId().Text(selected);
    } else {
        selectedProjectId_.clear();
        ProjectIdentity().Text(L"No registered project is selected.");
        ProjectFolders().Text(L"Register an authorized folder to begin.");
        ProjectPersistence().Text(L"No project memory store is active.");
        ProjectMemoryRecords().Children().Clear();
    }
}

void MainWindow::ApplyProjectWorkspace(
    const ::ForgeConductor::Manager::ManagerProjectWorkspaceSnapshot& snapshot)
{
    selectedProjectId_ = snapshot.project.id.value();
    const auto selected = winrt::to_hstring(selectedProjectId_);
    storeSavedText(SelectedProjectValue, selected);
    RunProjectId().Text(selected);

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
    if (telemetrySnapshot_) {
        HistoryEquivalentText().Text(
            L"Last measured CPU/RAM history is stale because the Manager is disconnected.");
        LatencyEquivalentText().Text(
            L"Last measured latency history is stale because the Manager is disconnected.");
    }
}

winrt::fire_and_forget MainWindow::RunAction(const Action action)
{
    auto lifetime = get_strong();
    if (busy_ || !connection_) co_return;

    std::optional<::ForgeConductor::Domain::ManagerSettings> submitted;
    const bool runAction = action == Action::RunStart ||
        action == Action::RunStatus || action == Action::RunPause ||
        action == Action::RunResume || action == Action::RunCancel;
    const bool projectAction = action == Action::ProjectList ||
        action == Action::ProjectRegister || action == Action::ProjectLoad ||
        action == Action::ProjectRemember;
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
            runProject = winrt::to_string(RunProjectId().Text());
            runClient = winrt::to_string(RunClientId().Text());
            runTask = winrt::to_string(RunTask().Text());
            try {
                runGeneration = numberValue(
                    RunAuthorityGeneration(), "Authority generation");
            } catch (const std::exception& exception) {
                RunState().Text(winrt::to_hstring(exception.what()));
                co_return;
            }
        } else if (runId.empty()) {
            RunState().Text(L"Enter a run ID to attach or control a Manager-owned run.");
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

    busy_ = true;
    winrt::apartment_context ui;
    if (runAction) {
        RunState().Text(L"Contacting the Manager…");
    } else if (projectAction) {
        ProjectState().Text(L"Contacting the Manager…");
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
        } else if (action == Action::ProviderLoad || action == Action::ProviderSave ||
            action == Action::ProviderTest) {
            if (action == Action::ProviderLoad && loaded.loaded) {
                providerSettings_ = loaded.settings;
                ApplyProviderForm(*providerSettings_);
            } else if (action == Action::ProviderSave && submitted && !failed) {
                providerSettings_ = submitted;
            }
            ProviderState().Text(winrt::to_hstring(message));
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
    busy_ = false;
    if (followUp) RunAction(*followUp);
}
}
