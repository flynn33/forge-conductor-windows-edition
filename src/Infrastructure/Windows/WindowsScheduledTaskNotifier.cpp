#include "ForgeConductor/Infrastructure/Windows/WindowsScheduledTaskNotifier.h"
#include "ForgeConductor/Domain/ProductIdentity.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "Detail/UtfConversion.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <appmodel.h>
#include <roapi.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(code, std::move(message))};
}
template<typename T> T take(Domain::Result<T> value) {
    if (!value) throw Failure{std::move(value).error()};
    return std::move(value).value();
}
void check(const Domain::OperationContext& context) {
    if (context.isCancellationRequested()) fail(Domain::ErrorCodes::Cancelled, "Windows notification submission was cancelled before submission.");
    if (context.isExpired(std::chrono::steady_clock::now())) fail(Domain::ErrorCodes::DeadlineExceeded, "Windows notification submission expired before submission.");
}
Json parse(std::string_view input) {
    if (input.empty() || input.size() > WindowsScheduledTaskNotifier::MaximumEventBytes)
        fail(Domain::ErrorCodes::PayloadTooLarge, "The scheduled notification event exceeds 64 KiB or is empty.");
    if (!Domain::isValidUtf8(input)) fail(Domain::ErrorCodes::InvalidRequest, "The notification event must be UTF-8 JSON.");
    std::vector<std::set<std::string>> objects;
    auto value = Json::parse(input, [&](int depth, Json::parse_event_t event, Json& member) {
        if (depth > 12) fail(Domain::ErrorCodes::LimitExceeded, "The notification event exceeds JSON depth 12.");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(member.get<std::string>()).second)
            fail(Domain::ErrorCodes::InvalidRequest, "The notification event contains duplicate properties.");
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
    if (!value.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "The notification event must be an object.");
    return value;
}
std::string boundedText(std::string value, std::size_t maximum) {
    if (!Domain::isValidUtf8(value) || value.find('\0') != std::string::npos)
        fail(Domain::ErrorCodes::InvalidRequest, "Notification text must be UTF-8 without NUL bytes.");
    auto wide = take(Detail::strictUtf8ToUtf16(value));
    for (auto& character : wide) if (character < 0x20 || character == 0xfffe || character == 0xffff) character = L' ';
    value = take(Detail::strictUtf16ToUtf8(wide));
    if (value.size() > maximum) {
        auto end = maximum - 3U;
        while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U) --end;
        value.resize(end); value.append("...");
    }
    return value;
}
std::string escape(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        switch (character) {
        case '&': result.append("&amp;"); break;
        case '<': result.append("&lt;"); break;
        case '>': result.append("&gt;"); break;
        case '"': result.append("&quot;"); break;
        case '\'': result.append("&apos;"); break;
        default: result.push_back(character); break;
        }
    }
    return result;
}
std::string label(std::string_view event) {
    if (event == "created") return "Schedule created";
    if (event == "dispatch_admitted") return "Run admitted";
    if (event == "manual_run_authorized") return "New attempt authorized";
    if (event == "cancel_requested") return "Cancellation requested";
    if (event == "needs_attention" || event == "scheduler_error") return "Needs attention";
    if (event == "interrupted") return "Previous run interrupted";
    if (event == "running") return "Run state: running";
    if (event == "paused") return "Run state: paused";
    if (event == "cancelling") return "Run state: cancelling";
    if (event == "completed") return "Run completed";
    if (event == "failed") return "Run failed";
    if (event == "cancelled") return "Run cancelled";
    fail(Domain::ErrorCodes::InvalidRequest, "The notification event is not a supported schedule transition.");
}
std::string payload(const Json& input) {
    const auto event = input.at("event").get<std::string>();
    const auto summary = label(event);
    const auto name = event == "scheduler_error" ? "Scheduled tasks" : boundedText(input.at("name").get<std::string>(), 256U);
    if (name.empty()) fail(Domain::ErrorCodes::InvalidRequest, "The notification schedule name is empty.");
    std::string runState;
    if (input.contains("latest_run") && !input.at("latest_run").is_null()) {
        runState = input.at("latest_run").at("state").get<std::string>();
        if (runState != "admitted" && runState != "running" && runState != "paused" && runState != "cancelling" &&
            runState != "missing" && runState != "completed" && runState != "failed" && runState != "cancelled")
            fail(Domain::ErrorCodes::InvalidRequest, "The notification run state is unsupported.");
    }
    if ((event == "completed" || event == "failed" || event == "cancelled") && runState != event)
        fail(Domain::ErrorCodes::InvalidRequest, "The notification transition does not match the actual run state.");
    std::string xml = "<toast duration=\"short\"><visual><binding template=\"ToastGeneric\"><text>";
    xml.append(escape(Domain::ProductName)).append("</text><text>").append(escape(name)).append(": ").append(escape(summary)).append("</text>");
    if (!runState.empty()) xml.append("<text>Actual run state: ").append(escape(runState)).append("</text>");
    xml.append("</binding></visual><audio silent=\"true\"/></toast>");
    if (xml.size() > WindowsScheduledTaskNotifier::MaximumPayloadBytes) fail(Domain::ErrorCodes::PayloadTooLarge, "The Windows notification payload exceeds 4096 bytes.");
    return xml;
}
std::string installedExecutableApplicationId(std::wstring_view executable) {
    const std::filesystem::path path{executable};
    if (!path.is_absolute()) fail(Domain::ErrorCodes::InvalidRequest, "The notification executable path must be absolute.");
    const auto directory = path.parent_path().lexically_normal().native();
    const auto fullName = path.parent_path().filename().native();
    UINT32 familyCount{};
    const auto familySize = ::PackageFamilyNameFromFullName(fullName.c_str(), &familyCount, nullptr);
    if (familySize != ERROR_INSUFFICIENT_BUFFER || familyCount < 2U || familyCount > PACKAGE_FAMILY_NAME_MAX_LENGTH + 1U)
        fail(Domain::ErrorCodes::HostCapabilityUnavailable, "The notification executable directory is not an installed package full name (native " + std::to_string(familySize) + ").");
    std::vector<wchar_t> family(familyCount);
    const auto familyResult = ::PackageFamilyNameFromFullName(fullName.c_str(), &familyCount, family.data());
    if (familyResult != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Reading the notification package family failed (native " + std::to_string(familyResult) + ").");
    UINT32 pathCount{};
    const auto pathSize = ::GetPackagePathByFullName(fullName.c_str(), &pathCount, nullptr);
    if (pathSize != ERROR_INSUFFICIENT_BUFFER || pathCount < 2U || pathCount > 32768U)
        fail(Domain::ErrorCodes::HostCapabilityUnavailable, "The notification executable's package is not registered (native " + std::to_string(pathSize) + ").");
    std::vector<wchar_t> registered(pathCount);
    const auto pathResult = ::GetPackagePathByFullName(fullName.c_str(), &pathCount, registered.data());
    if (pathResult != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Reading the notification package directory failed (native " + std::to_string(pathResult) + ").");
    const auto registeredDirectory = std::filesystem::path{registered.data()}.lexically_normal().native();
    if (directory.size() > 32767U || registeredDirectory.size() > 32767U || ::CompareStringOrdinal(directory.data(), static_cast<int>(directory.size()),
        registeredDirectory.data(), static_cast<int>(registeredDirectory.size()), TRUE) != CSTR_EQUAL)
        fail(Domain::ErrorCodes::HostCapabilityUnavailable, "The notification executable directory does not match the registered installed package path.");
    return take(Detail::strictUtf16ToUtf8(std::wstring_view{family.data(), familyCount - 1U})) + "!ForgeConductor";
}
std::string applicationId(std::string configured) {
    if (configured.empty()) {
        UINT32 count{};
        const auto initial = ::GetCurrentPackageFamilyName(&count, nullptr);
        if (initial == APPMODEL_ERROR_NO_PACKAGE) {
            std::vector<wchar_t> module(32768U);
            const auto length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            if (!length || length >= module.size()) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Reading the notification host executable path failed.");
            configured = installedExecutableApplicationId(std::wstring_view{module.data(), length});
        } else if (initial != ERROR_INSUFFICIENT_BUFFER || count < 2U || count > PACKAGE_FAMILY_NAME_MAX_LENGTH + 1U)
            fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Windows toast requires the installed Forge Conductor package identity or a host-verified installed AUMID (native " + std::to_string(initial) + ").");
        else {
            std::vector<wchar_t> family(count);
            const auto result = ::GetCurrentPackageFamilyName(&count, family.data());
            if (result != ERROR_SUCCESS) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Reading the installed notification package identity failed (native " + std::to_string(result) + ").");
            configured = take(Detail::strictUtf16ToUtf8(std::wstring_view{family.data(), count - 1U})) + "!ForgeConductor";
        }
    }
    constexpr std::string_view prefix = "ForgeConductor.Windows_", suffix = "!ForgeConductor";
    if (!configured.starts_with(prefix) || !configured.ends_with(suffix) || configured.size() > 160U ||
        configured.size() <= prefix.size() + suffix.size() || !std::all_of(configured.begin(), configured.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '!';
        })) fail(Domain::ErrorCodes::InvalidRequest, "The host notification AUMID must identify the installed ForgeConductor.Windows package's ForgeConductor application.");
    return configured;
}
class Apartment final {
public:
    Apartment() {
        auto result = ::RoInitialize(RO_INIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) result = ::RoInitialize(RO_INIT_SINGLETHREADED);
        winrt::check_hresult(result);
    }
    ~Apartment() noexcept { ::RoUninitialize(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};
std::string settingName(winrt::Windows::UI::Notifications::NotificationSetting value) {
    using Setting = winrt::Windows::UI::Notifications::NotificationSetting;
    switch (value) {
    case Setting::Enabled: return "enabled";
    case Setting::DisabledForApplication: return "disabled_for_application";
    case Setting::DisabledForUser: return "disabled_for_user";
    case Setting::DisabledByGroupPolicy: return "disabled_by_group_policy";
    case Setting::DisabledByManifest: return "disabled_by_manifest";
    }
    return "unknown";
}
std::string hresultText(winrt::hresult result) {
    std::array<char, 16U> text{};
    std::snprintf(text.data(), text.size(), "0x%08lx", static_cast<unsigned long>(static_cast<std::uint32_t>(result.value)));
    return text.data();
}
} // namespace

WindowsScheduledTaskNotifier::WindowsScheduledTaskNotifier(std::string installedApplicationId)
    : applicationId_{std::move(installedApplicationId)} {}

Domain::Result<std::string> WindowsScheduledTaskNotifier::buildPayload(std::string_view event) noexcept {
    try { return Domain::Result<std::string>::success(payload(parse(event))); }
    catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (const Json::exception&) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "The notification event contains invalid JSON fields.")); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Building the local notification failed safely.")); }
}
Domain::Result<std::string> WindowsScheduledTaskNotifier::applicationIdForInstalledExecutable(std::string_view executable) noexcept {
    try { return Domain::Result<std::string>::success(applicationId(installedExecutableApplicationId(take(Detail::strictUtf8ToUtf16(executable))))); }
    catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, "Resolving the installed notification executable failed safely.")); }
}
Domain::Result<std::string> WindowsScheduledTaskNotifier::resolveApplicationId(const Domain::OperationContext& context) const noexcept {
    try { check(context); return Domain::Result<std::string>::success(applicationId(applicationId_)); }
    catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable, "Resolving the local notification identity failed safely.")); }
}
Domain::Result<std::string> WindowsScheduledTaskNotifier::submit(std::string_view event,
    const Domain::OperationContext& context) const noexcept {
    try {
        check(context); const auto input = parse(event); const auto xmlText = payload(input);
        const auto id = take(resolveApplicationId(context)); check(context);
        const Apartment apartment;
        using namespace winrt::Windows::UI::Notifications;
        const auto notifier = ToastNotificationManager::CreateToastNotifier(take(Detail::strictUtf8ToUtf16(id)));
        const auto setting = notifier.Setting();
        if (setting != NotificationSetting::Enabled) fail(Domain::ErrorCodes::HostCapabilityUnavailable,
            "Windows toast was not submitted for " + id + ": notification_setting=" + settingName(setting) + ".");
        winrt::Windows::Data::Xml::Dom::XmlDocument xml;
        xml.LoadXml(take(Detail::strictUtf8ToUtf16(xmlText)));
        const ToastNotification toast{xml};
        check(context); notifier.Show(toast);
        // Show returns void. Successful submission does not establish banner
        // display, receipt by a human, or later asynchronous platform success.
        return Domain::Result<std::string>::success(Json{{"channel", "windows_toast"}, {"application_id", id},
            {"submission_accepted", true}, {"display_confirmed", false}, {"delivery_state", "submitted"},
            {"notification_setting", settingName(setting)}, {"event", input.at("event")},
            {"operation_id", context.operationId.value()}}.dump());
    } catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (const winrt::hresult_error& error) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
        "Windows toast submission failed (HRESULT " + hresultText(error.code()) + "). Display was not confirmed.")); }
    catch (const Json::exception&) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "The notification event contains invalid JSON fields.")); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The local Windows notification failed safely.")); }
}
} // namespace ForgeConductor::Infrastructure::Windows
