#pragma once
#include "ForgeConductor/Infrastructure/Windows/WindowsAlphaManagerProfile.h"
#include "ForgeConductor/Domain/ManagerModels.h"
#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Domain/ManagerTelemetryModels.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ForgeConductor::Hosts::App {
struct ProviderSettingsView final {
    bool loaded{};
    std::string message;
    Domain::ManagerSettings settings;
};

enum class ManagedRunAction { Status, Pause, Resume, Cancel };

struct ManagedRunView final {
    bool loaded{};
    std::string message;
    std::optional<Domain::ManagedRunSnapshot> snapshot;
};

struct TelemetryView final {
    bool loaded{};
    std::string message;
    std::optional<Domain::ManagerTelemetrySnapshot> snapshot;
};

struct ProjectsView final {
    bool loaded{};
    std::string message;
    std::optional<Manager::ManagerProjectsSnapshot> snapshot;
};

struct ProjectWorkspaceView final {
    bool loaded{};
    std::string message;
    std::optional<Manager::ManagerProjectWorkspaceSnapshot> snapshot;
};

class IManagerConnection {
public:
    virtual ~IManagerConnection() = default;
    virtual std::string refresh(std::stop_token cancellation) noexcept = 0;
    virtual TelemetryView telemetry(
        std::string selectedRunId,
        std::stop_token cancellation) noexcept = 0;
    virtual std::string start(std::stop_token cancellation) noexcept = 0;
    virtual std::string control(
        Domain::ManagerControlAction action,
        std::stop_token cancellation) noexcept = 0;
    virtual ProviderSettingsView providerSettings(
        std::stop_token cancellation) noexcept = 0;
    virtual std::string saveProviderSettings(
        const Domain::ManagerSettingsPatch& patch,
        std::stop_token cancellation) noexcept = 0;
    virtual std::string testProvider(
        const Domain::ManagerSettings& settings,
        std::stop_token cancellation) noexcept = 0;
    virtual ManagedRunView startManagedRun(
        std::string projectId,
        std::string clientId,
        std::uint64_t authorityGeneration,
        std::string task,
        std::stop_token cancellation) noexcept = 0;
    virtual ManagedRunView controlManagedRun(
        std::string runId,
        ManagedRunAction action,
        std::stop_token cancellation) noexcept = 0;
    virtual ProjectsView projects(
        std::stop_token cancellation) noexcept = 0;
    virtual ProjectWorkspaceView initializeProject(
        std::string projectPath,
        std::string displayName,
        std::stop_token cancellation) noexcept = 0;
    virtual ProjectWorkspaceView projectMemory(
        std::string projectId,
        std::string query,
        std::stop_token cancellation) noexcept = 0;
    virtual ProjectWorkspaceView rememberProjectMemory(
        std::string projectId,
        std::string title,
        std::string summary,
        std::string body,
        std::vector<std::string> tags,
        std::stop_token cancellation) noexcept = 0;
};
class ManagerConnection final : public IManagerConnection {
public:
    explicit ManagerConnection(
        std::optional<std::wstring> alphaRoot = std::nullopt) noexcept;

    std::string refresh(std::stop_token cancellation) noexcept override;
    TelemetryView telemetry(
        std::string selectedRunId,
        std::stop_token cancellation) noexcept override;
    std::string start(std::stop_token cancellation) noexcept override;
    std::string control(
        Domain::ManagerControlAction action,
        std::stop_token cancellation) noexcept override;
    ProviderSettingsView providerSettings(
        std::stop_token cancellation) noexcept override;
    std::string saveProviderSettings(
        const Domain::ManagerSettingsPatch& patch,
        std::stop_token cancellation) noexcept override;
    std::string testProvider(
        const Domain::ManagerSettings& settings,
        std::stop_token cancellation) noexcept override;
    ManagedRunView startManagedRun(
        std::string projectId,
        std::string clientId,
        std::uint64_t authorityGeneration,
        std::string task,
        std::stop_token cancellation) noexcept override;
    ManagedRunView controlManagedRun(
        std::string runId,
        ManagedRunAction action,
        std::stop_token cancellation) noexcept override;
    ProjectsView projects(std::stop_token cancellation) noexcept override;
    ProjectWorkspaceView initializeProject(
        std::string projectPath,
        std::string displayName,
        std::stop_token cancellation) noexcept override;
    ProjectWorkspaceView projectMemory(
        std::string projectId,
        std::string query,
        std::stop_token cancellation) noexcept override;
    ProjectWorkspaceView rememberProjectMemory(
        std::string projectId,
        std::string title,
        std::string summary,
        std::string body,
        std::vector<std::string> tags,
        std::stop_token cancellation) noexcept override;
private:
    std::optional<Infrastructure::Windows::WindowsAlphaManagerProfile>
        alphaProfile_;
    std::string profileError_;
};
}
