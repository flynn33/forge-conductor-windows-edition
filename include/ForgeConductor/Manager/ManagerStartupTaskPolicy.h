#pragma once

#include "ForgeConductor/Domain/ManagerStartupModels.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Manager {

enum class ManagerStartupTaskLogonType {
    Other,
    CurrentInteractiveUser
};

enum class ManagerStartupTaskRunLevel {
    Other,
    LeastPrivilege
};

enum class ManagerStartupTaskProcessTokenSidType {
    Other,
    Default
};

enum class ManagerStartupTaskTriggerKind {
    Other,
    UserLogon
};

enum class ManagerStartupTaskActionKind {
    Other,
    Execute
};

enum class ManagerStartupTaskMultipleInstances {
    Other,
    IgnoreNew
};

enum class ManagerStartupTaskCompatibility {
    Other,
    Windows10OrLater
};

struct ManagerStartupTaskOwnership final {
    std::string source;
    std::string uri;

    bool operator==(const ManagerStartupTaskOwnership&) const = default;
};

struct ManagerStartupTaskPrincipal final {
    std::string id;
    std::string displayName;
    std::string userIdentity;
    std::string groupIdentity;
    ManagerStartupTaskLogonType logonType{
        ManagerStartupTaskLogonType::Other};
    ManagerStartupTaskRunLevel runLevel{
        ManagerStartupTaskRunLevel::Other};
    ManagerStartupTaskProcessTokenSidType processTokenSidType{
        ManagerStartupTaskProcessTokenSidType::Other};
    std::vector<std::string> requiredPrivileges;

    bool operator==(const ManagerStartupTaskPrincipal&) const = default;
};

// Task Scheduler accepts calendar-based XSD durations that cannot always be
// represented as fixed seconds. Adapters preserve those bounded forms as text
// so unsupported native behavior becomes drift instead of being coerced.
class ManagerStartupTaskDuration final {
public:
    ManagerStartupTaskDuration() noexcept
        : value_{std::chrono::seconds::zero()}
    {
    }

    explicit ManagerStartupTaskDuration(
        const std::chrono::seconds value) noexcept
        : value_{value}
    {
    }

    [[nodiscard]] static ManagerStartupTaskDuration preserved(
        std::string text)
    {
        return ManagerStartupTaskDuration{PreservedTag{}, std::move(text)};
    }

    [[nodiscard]] const std::chrono::seconds* fixedSeconds() const noexcept
    {
        return std::get_if<std::chrono::seconds>(&value_);
    }

    [[nodiscard]] const std::string* preservedText() const noexcept
    {
        return std::get_if<std::string>(&value_);
    }

    bool operator==(const ManagerStartupTaskDuration&) const = default;

private:
    struct PreservedTag final {
    };

    ManagerStartupTaskDuration(PreservedTag, std::string text)
        : value_{std::move(text)}
    {
    }

    std::variant<std::chrono::seconds, std::string> value_;
};

struct ManagerStartupTaskRepetition final {
    ManagerStartupTaskDuration interval;
    ManagerStartupTaskDuration duration;
    bool stopAtDurationEnd{};

    bool operator==(const ManagerStartupTaskRepetition&) const = default;
};

struct ManagerStartupTaskTrigger final {
    ManagerStartupTaskTriggerKind kind{ManagerStartupTaskTriggerKind::Other};
    std::string id;
    std::string userIdentity;
    bool enabled{};
    std::optional<std::string> startBoundary;
    std::optional<std::string> endBoundary;
    ManagerStartupTaskDuration executionTimeLimit;
    std::optional<ManagerStartupTaskRepetition> repetition;
    ManagerStartupTaskDuration delay;

    bool operator==(const ManagerStartupTaskTrigger&) const = default;
};

struct ManagerStartupTaskAction final {
    ManagerStartupTaskActionKind kind{ManagerStartupTaskActionKind::Other};
    std::string id;
    std::optional<Domain::PathText> executable;
    std::string arguments;
    std::optional<Domain::PathText> workingDirectory;
    bool hideAppWindow{};

    bool operator==(const ManagerStartupTaskAction&) const = default;
};

struct ManagerStartupTaskSettings final {
    // Native task enablement is projected separately into the observation so
    // an exact disabled registration remains Disabled rather than Drifted.
    bool allowDemandStart{};
    ManagerStartupTaskMultipleInstances multipleInstances{
        ManagerStartupTaskMultipleInstances::Other};
    ManagerStartupTaskDuration executionTimeLimit;
    ManagerStartupTaskDuration restartInterval;
    std::uint8_t restartCount{};
    bool runOnlyIfIdle{};
    ManagerStartupTaskDuration idleDuration;
    ManagerStartupTaskDuration idleWaitTimeout;
    bool stopOnIdleEnd{};
    bool restartOnIdle{};
    bool runOnlyIfNetworkAvailable{};
    std::string networkProfileId;
    std::string networkProfileName;
    bool disallowStartIfOnBatteries{};
    bool stopIfGoingOnBatteries{};
    bool allowHardTerminate{};
    bool wakeToRun{};
    bool hidden{};
    bool startWhenAvailable{};
    ManagerStartupTaskDuration deleteExpiredTaskAfter;
    std::uint8_t priority{};
    ManagerStartupTaskCompatibility compatibility{
        ManagerStartupTaskCompatibility::Other};
    bool disallowStartOnRemoteAppSession{};
    bool useUnifiedSchedulingEngine{};
    bool maintenanceSettingsPresent{};
    bool volatileTask{};

    bool operator==(const ManagerStartupTaskSettings&) const = default;
};

// Platform adapters normalize every launch-relevant native field into this
// closed model. Exact value and collection-order equality is deliberate:
// extra triggers, actions, or policy changes are registration drift.
struct ManagerStartupTaskDefinition final {
    ManagerStartupTaskOwnership ownership;
    ManagerStartupTaskPrincipal principal;
    std::vector<ManagerStartupTaskTrigger> triggers;
    std::string actionContext;
    std::vector<ManagerStartupTaskAction> actions;
    ManagerStartupTaskSettings settings;

    bool operator==(const ManagerStartupTaskDefinition&) const = default;
};

struct ManagerStartupTaskObservation final {
    bool exists{};
    // True only after the native adapter inspected every field represented by
    // ManagerStartupTaskDefinition and rejected unsupported native behavior.
    bool launchProjectionComplete{};
    std::optional<std::string> registrationIdentity;
    std::optional<ManagerStartupTaskDefinition> definition;
    bool enabled{};
    bool running{};
    std::optional<std::int32_t> lastResult;
    std::optional<Domain::UtcTimePoint> lastRunAt;
};

class ManagerStartupTaskPolicy final {
public:
    static constexpr std::size_t MaximumTextBytes = 32U * 1024U;
    static constexpr std::size_t MaximumObservedTriggerCount = 48U;
    static constexpr std::size_t MaximumObservedActionCount = 32U;
    static constexpr std::size_t MaximumObservedPrivilegeCount = 64U;
    static constexpr std::size_t MaximumTaskPathUtf16Units = 260U;

    static constexpr std::string_view RequiredOwnershipSource =
        "ForgeConductor.Windows.ManagerStartup.v1";
    static constexpr std::string_view RequiredOwnershipUriPrefix =
        "\\ForgeConductor.Manager.v1.";
    static constexpr std::string_view RequiredPrincipalId =
        "ForgeConductor.Manager.Principal";
    static constexpr std::string_view RequiredTriggerId =
        "ForgeConductor.Logon";
    static constexpr std::string_view RequiredActionId =
        "ForgeConductor.Manager";
    static constexpr std::chrono::seconds RequiredTriggerDelay{0};
    static constexpr std::chrono::seconds RequiredExecutionTimeLimit{0};
    static constexpr std::chrono::seconds RequiredRestartInterval{60};
    static constexpr std::uint8_t RequiredRestartCount = 3U;
    static constexpr std::chrono::seconds RequiredIdleDuration{600};
    static constexpr std::chrono::seconds RequiredIdleWaitTimeout{3600};
    static constexpr std::chrono::seconds RequiredDeleteExpiredTaskAfter{0};
    static constexpr std::uint8_t RequiredPriority = 7U;

    ManagerStartupTaskPolicy() = delete;

    [[nodiscard]] static Domain::Result<void> validateExpectedDefinition(
        const ManagerStartupTaskDefinition& expected);

    [[nodiscard]] static Domain::Result<Domain::ManagerStartupStatus> classify(
        const ManagerStartupTaskDefinition& expected,
        const ManagerStartupTaskObservation& observed);
};

} // namespace ForgeConductor::Manager
