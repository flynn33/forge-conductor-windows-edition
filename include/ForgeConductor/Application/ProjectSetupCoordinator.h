#pragma once

#include <exception>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {

enum class SetupStage { Manager, Project, Plugins, Provider, Verification };
enum class SetupState { Pending, Running, Ready, NeedsAction, Cancelled };

struct SetupCheck final {
    SetupStage stage;
    SetupState state{SetupState::Pending};
    std::string detail;
};

struct SetupOperationResult final {
    bool ready{};
    std::string detail;
};

struct ProjectSetupSnapshot final {
    std::string folder;
    std::string projectId;
    std::string projectName;
    std::string model;
    std::vector<SetupCheck> checks;
    bool ready{};
};

// Implementations use authenticated Manager operations for durable project and
// settings mutations. No setup result authorizes a run: the Manager revalidates
// project, provider and governance at execution time.
class IProjectSetupOperations {
public:
    virtual ~IProjectSetupOperations() = default;
    virtual SetupOperationResult ensureManager(std::stop_token) = 0;
    virtual SetupOperationResult ensureProject(ProjectSetupSnapshot&, std::stop_token) = 0;
    virtual SetupOperationResult ensurePlugins(std::stop_token) = 0;
    virtual SetupOperationResult ensureProvider(ProjectSetupSnapshot&, std::stop_token) = 0;
    virtual SetupOperationResult verifyProvider(ProjectSetupSnapshot&, std::stop_token) = 0;
};

class ProjectSetupCoordinator final {
public:
    using Observer = std::function<void(const ProjectSetupSnapshot&)>;

    explicit ProjectSetupCoordinator(IProjectSetupOperations& operations)
        : operations_{operations} {}

    [[nodiscard]] ProjectSetupSnapshot prepare(
        std::string folder, std::stop_token cancellation,
        const Observer& observer = {}, bool installPlugins = true)
    {
        ProjectSetupSnapshot result;
        result.folder = std::move(folder);
        result.checks = {{SetupStage::Manager}, {SetupStage::Project}};
        if (installPlugins) result.checks.push_back({SetupStage::Plugins});
        result.checks.push_back({SetupStage::Provider});
        result.checks.push_back({SetupStage::Verification});
        for (auto& check : result.checks) {
            if (cancellation.stop_requested()) {
                check.state = SetupState::Cancelled;
                check.detail = "Preparation stopped. Retry safely to reuse completed setup.";
                if (observer) observer(result);
                return result;
            }
            check.state = SetupState::Running;
            if (observer) observer(result);
            SetupOperationResult operation;
            try {
                switch (check.stage) {
                case SetupStage::Manager:
                    operation = operations_.ensureManager(cancellation); break;
                case SetupStage::Project:
                    operation = operations_.ensureProject(result, cancellation); break;
                case SetupStage::Plugins:
                    operation = operations_.ensurePlugins(cancellation); break;
                case SetupStage::Provider:
                    operation = operations_.ensureProvider(result, cancellation); break;
                case SetupStage::Verification:
                    operation = operations_.verifyProvider(result, cancellation); break;
                }
            } catch (const std::exception& failure) {
                operation = {false, failure.what()};
            } catch (...) {
                operation = {false, "Preparation failed. Retry or open diagnostics for details."};
            }
            check.state = cancellation.stop_requested() ? SetupState::Cancelled
                : operation.ready ? SetupState::Ready : SetupState::NeedsAction;
            check.detail = std::move(operation.detail);
            // A backend success without authoritative identities is incomplete.
            if (check.state == SetupState::Ready &&
                ((check.stage == SetupStage::Project && result.projectId.empty()) ||
                 (check.stage == SetupStage::Provider && result.model.empty()))) {
                check.state = SetupState::NeedsAction;
                check.detail = "Preparation returned no verified identity. Retry preparation.";
            }
            if (observer) observer(result);
            if (check.state != SetupState::Ready) return result;
        }
        result.ready = true;
        if (observer) observer(result);
        return result;
    }

private:
    IProjectSetupOperations& operations_;
};

} // namespace ForgeConductor::Application
