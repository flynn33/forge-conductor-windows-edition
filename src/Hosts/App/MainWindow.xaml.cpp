#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"
#include "TelemetryPresentation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Storage.h>

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
    Closed([this](auto const&, auto const&) { cancellation_.request_stop(); });
    RootNavigation().Loaded([this](auto const&, auto const&) {
        try {
            const auto saved = Windows::Storage::ApplicationData::Current()
                .LocalSettings().Values().TryLookup(L"selected_page");
            const auto selected = saved.try_as<Windows::Foundation::IPropertyValue>();
            if (!selected || selected.Type() !=
                    Windows::Foundation::PropertyType::String) return;
            const auto tag = selected.GetString();
            for (const auto& candidate : RootNavigation().MenuItems()) {
                const auto item = candidate.try_as<
                    Microsoft::UI::Xaml::Controls::NavigationViewItem>();
                if (item && unbox_value_or<hstring>(item.Tag(), L"") == tag) {
                    RootNavigation().SelectedItem(item);
                    break;
                }
            }
        } catch (...) {
        }
    });
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

void MainWindow::NavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args)
{
    const auto item = args.SelectedItem().try_as<
        Microsoft::UI::Xaml::Controls::NavigationViewItem>();
    if (!item) return;
    const auto tag = unbox_value_or<hstring>(item.Tag(), L"Rig");
    PageTitle().Text(tag);
    try {
        Windows::Storage::ApplicationData::Current().LocalSettings()
            .Values().Insert(L"selected_page", box_value(tag));
    } catch (...) {
    }
    const bool provider = tag == L"Provider";
    const bool autonomy = tag == L"Autonomy" || tag == L"Continuity";
    const bool rig = tag == L"Rig";
    ProviderPanel().Visibility(provider ? Visibility::Visible : Visibility::Collapsed);
    AutonomyPanel().Visibility(autonomy ? Visibility::Visible : Visibility::Collapsed);
    RigPanel().Visibility(rig ? Visibility::Visible : Visibility::Collapsed);
    GenericPanel().Visibility(!provider && !rig && !autonomy ? Visibility::Visible : Visibility::Collapsed);
    if (provider) {
        PageDescription().Text(L"Configure and test the Manager-owned LM Studio Responses endpoint.");
        if (!providerSettings_) RunAction(Action::ProviderLoad);
    } else if (rig) {
        PageDescription().Text(L"Read and control the current native Manager runtime.");
    } else if (autonomy) {
        PageDescription().Text(L"Start, attach, pause, resume, and stop Manager-owned work while observing retained context.");
    } else if (tag == L"Projects") {
        PageDescription().Text(L"Inspect registered project identities and their current Manager session scope.");
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

winrt::fire_and_forget MainWindow::RunAction(const Action action)
{
    auto lifetime = get_strong();
    if (busy_ || !connection_) co_return;

    std::optional<::ForgeConductor::Domain::ManagerSettings> submitted;
    const bool runAction = action == Action::RunStart ||
        action == Action::RunStatus || action == Action::RunPause ||
        action == Action::RunResume || action == Action::RunCancel;
    std::string runProject;
    std::string runClient;
    std::string runTask;
    std::string runId;
    std::uint64_t runGeneration{};
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

    busy_ = true;
    winrt::apartment_context ui;
    if (runAction) {
        RunState().Text(L"Contacting the Manager…");
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
        if (runAction) {
            if (runView.snapshot) {
                RunId().Text(winrt::to_hstring(
                    runView.snapshot->record.runId.value()));
            }
            RunState().Text(winrt::to_hstring(message));
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
            }
            ManagerState().Text(winrt::to_hstring(message));
            GenericState().Text(winrt::to_hstring(message));
        }
    }
    busy_ = false;
}
}
