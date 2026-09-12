#include "ManagerConnection.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace ForgeConductor::Hosts::App {
namespace W = Infrastructure::Windows;
namespace {
struct ProcessHandles final {
    PROCESS_INFORMATION value{};
    ~ProcessHandles() { if (value.hThread) CloseHandle(value.hThread); if (value.hProcess) CloseHandle(value.hProcess); }
};

[[nodiscard]] Domain::OperationContext operationContext(
    const std::shared_ptr<W::SystemClock>& clock,
    const std::stop_token cancellation,
    const std::chrono::seconds timeout = std::chrono::seconds{5})
{
    W::WindowsUuidGenerator ids;
    auto id = ids.next();
    if (!id) throw std::runtime_error{id.error().message};
    return Domain::OperationContext{
        Domain::OperationId{id.value()},
        clock->monotonicNow() + timeout,
        cancellation,
        Domain::CorrelationId::parse(id.value().value()).value()};
}

[[nodiscard]] Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>
connectManager(
    const std::optional<W::WindowsAlphaManagerProfile>& alphaProfile,
    const Domain::OperationContext& context,
    const std::shared_ptr<W::SystemClock>& clock)
{
    auto identity = W::WindowsCurrentUserIdentity::load();
    if (!identity) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            identity.error());
    }
    W::WindowsManagerInstanceLeaseOptions leaseOptions;
    if (alphaProfile) leaseOptions.purposeSuffix = alphaProfile->purposeSuffix();
    auto names = W::WindowsManagerInstanceLease::namesFor(
        identity.value(), leaseOptions);
    if (!names) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            names.error());
    }
    const std::wstring registrySubkey = alphaProfile
        ? std::wstring{alphaProfile->secureStorageRegistrySubkey()}
        : std::wstring{W::DpapiSecureStorage::DefaultRegistrySubkey};
    W::DpapiSecureStorage secure{registrySubkey};
    W::WindowsManagerAuthenticationTokenGenerator generator;
    W::WindowsManagerAuthenticationTokenStore tokens{secure, generator};
    auto nonce = tokens.load(context);
    if (!nonce) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            nonce.error());
    }
    if (!nonce.value()) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::SessionNotFound,
                "Manager is not initialized. Select Start manager."));
    }
    return W::WindowsManagerNamedPipeClient::create(
        clock, std::wstring{names.value().pipeName()}, *nonce.value());
}
}
ManagerConnection::ManagerConnection(
    std::optional<std::wstring> alphaRoot) noexcept
{
    if (!alphaRoot) return;
    auto created = W::WindowsAlphaManagerProfile::create(*alphaRoot);
    if (!created) {
        profileError_ = "Invalid isolated Alpha root: " +
            created.error().message;
        return;
    }
    alphaProfile_.emplace(std::move(created).value());
}
std::string ManagerConnection::refresh(std::stop_token cancellation) noexcept {
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        const auto result = client->status(context);
        client->shutdown();
        if (!result) return "Disconnected: " + result.error().message;
        const auto& status = result.value();
        const std::string profile = alphaProfile_
            ? "Isolated Alpha\nData: " + alphaProfile_->dataRoot().value()
            : "Production\nData: %LOCALAPPDATA%\\Forge Conductor";
        return "Connected to manager PID " + std::to_string(status.processId) +
            "\nProfile: " + profile +
            "\nVersion: " + status.version +
            "\nService: " + (status.serviceActive ? "active" : "inactive") +
            "\nDashboard: " + (status.httpListening ? "listening" : "not listening") +
            (status.lastError ? "\nLast error: " + *status.lastError : "");
    } catch (const std::exception& error) { return error.what(); }
      catch (...) { return "Could not read manager status."; }
}

ProviderSettingsView ManagerConnection::providerSettings(
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, {}};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, {}};
        auto client = std::move(created).value();
        auto result = client->settings(context);
        client->shutdown();
        if (!result) return {false, result.error().message, {}};
        return {true, "Provider settings loaded from the manager.",
            std::move(result).value()};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    } catch (...) {
        return {false, "Could not load provider settings.", {}};
    }
}

std::string ManagerConnection::control(
    const Domain::ManagerControlAction action,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        auto result = client->control({action}, context);
        client->shutdown();
        if (!result) return "Manager control failed: " + result.error().message;
        const auto& status = result.value();
        const char* state = status.serviceActive ? "active" : "inactive";
        return "Manager service is " + std::string{state} +
            ". Process PID " + std::to_string(status.processId) + ".";
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Could not control the Manager service.";
    }
}

std::string ManagerConnection::saveProviderSettings(
    const Domain::ManagerSettingsPatch& patch,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        auto result = client->updateSettings(patch, true, context);
        client->shutdown();
        if (!result) return "Provider settings were not saved: " + result.error().message;
        return "Provider settings saved for " +
            result.value().settings.localModelHost + ":" +
            std::to_string(result.value().settings.localModelPort) +
            ". Restart the Manager before starting new model sessions.";
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Could not save provider settings.";
    }
}

std::string ManagerConnection::testProvider(
    const Domain::ManagerSettings& settings,
    const std::stop_token cancellation) noexcept
{
    try {
        auto valid = Domain::validateManagerSettings(settings);
        if (!valid) return "Connection test blocked: " + valid.error().message;
        W::LMStudioResponsesTransportConfiguration configuration;
        configuration.loopbackHost = settings.localModelHost;
        configuration.port = settings.localModelPort;
        configuration.secure = settings.localModelSecure;
        if (!settings.localModelName.empty()) {
            configuration.model = settings.localModelName;
        }
        auto transport = std::make_unique<W::LMStudioResponsesTransport>(
            std::move(configuration));
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        W::WindowsUuidGenerator ids;
        const auto next = [&ids]() {
            auto value = ids.next();
            if (!value) throw std::runtime_error{value.error().message};
            return value.value().value();
        };
        Domain::SessionCreationRequest request{
            Domain::ContinuityOperationId::parse(next()).value(),
            Domain::ProjectId::parse(next()).value(),
            Domain::SessionId::parse(next()).value(),
            Domain::IdempotencyKey::create(next()).value()};
        auto probed = transport->createSession(request, context);
        transport->shutdown();
        if (!probed) return "LM Studio connection failed: " + probed.error().message;
        return "LM Studio model discovery succeeded. Loaded model: " +
            probed.value().model.value_or("unknown") + ".";
    } catch (const std::exception& error) {
        return "LM Studio connection failed: " + std::string{error.what()};
    } catch (...) {
        return "LM Studio connection failed safely.";
    }
}
std::string ManagerConnection::start(std::stop_token cancellation) noexcept {
    try {
        if (!profileError_.empty()) return profileError_;
        if (cancellation.stop_requested()) return "Cancelled.";
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return "Cannot resolve the installed application directory.";
        const auto executable = std::filesystem::path{path.data()}.parent_path() / L"ForgeConductor.Manager.exe";
        std::wstring arguments = L"\"" + executable.wstring() + L"\"";
        if (alphaProfile_) {
            arguments.append(L" --alpha-root \"");
            arguments.append(alphaProfile_->nativeDataRoot());
            arguments.push_back(L'\"');
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        ProcessHandles process;
        if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process.value)) {
            return "Manager could not start. Windows error " + std::to_string(GetLastError()) +
                ". Verify the manager executable is installed beside the app.";
        }
        // The manager is independently owned. Closing this connection never terminates it.
        return alphaProfile_
            ? "Isolated Alpha manager start requested for " +
                alphaProfile_->dataRoot().value() +
                ". Select Refresh to attach and read its status."
            : "Manager start requested. Select Refresh to attach and read its status.";
    } catch (const std::exception& error) { return error.what(); }
      catch (...) { return "Could not start manager."; }
}
}
