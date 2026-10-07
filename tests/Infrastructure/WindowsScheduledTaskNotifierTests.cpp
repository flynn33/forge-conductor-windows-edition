#include "TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsScheduledTaskNotifier.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using Notifier = Infrastructure::Windows::WindowsScheduledTaskNotifier;
Domain::OperationContext context() { return TestContext{}.active(); }
Json completed(std::string name = "Owned project check") {
    return Json{{"name", name}, {"event", "completed"}, {"latest_run", {{"state", "completed"}, {"output", "private result never placed in toast"}}}};
}
void payloadEscapesBoundsAndReportsActualState() {
    const auto xml = take(Notifier::buildPayload(completed("A <check> & \"quote\" ' Ω").dump()));
    require(xml.find("A &lt;check&gt; &amp; &quot;quote&quot; &apos; Ω") != std::string::npos &&
        xml.find("Run completed") != std::string::npos && xml.find("Actual run state: completed") != std::string::npos,
        "The notification did not escape input or retain actual state.");
    require(xml.find("private result") == std::string::npos && xml.find("<actions") == std::string::npos && xml.find("<image") == std::string::npos,
        "The notification exposed output or introduced executable/network content.");
    const auto truncated = take(Notifier::buildPayload(completed(std::string(20000U, '&')).dump()));
    require(truncated.size() <= Notifier::MaximumPayloadBytes && truncated.find("...") != std::string::npos,
        "A long schedule name exceeded its toast bound.");
    const auto sanitized = take(Notifier::buildPayload(completed(std::string{"line\n\x01"} + "next").dump()));
    require(sanitized.find("line  next") != std::string::npos, "XML-forbidden/line controls were not normalized.");
    const auto error = take(Notifier::buildPayload(Json{{"event", "scheduler_error"}, {"error", {{"message", "private detail"}}}}.dump()));
    require(error.find("Scheduled tasks: Needs attention") != std::string::npos && error.find("private detail") == std::string::npos,
        "A scheduler error lacked a truthful bounded notification.");
}
void invalidPayloadAndNoImplicitSubmission() {
    auto mismatch = completed(); mismatch["latest_run"]["state"] = "failed";
    requireError(Notifier::buildPayload(mismatch.dump()), Domain::ErrorCodes::InvalidRequest, "A mismatched completion state was accepted.");
    requireError(Notifier::buildPayload("{\"event\":\"created\",\"event\":\"completed\",\"name\":\"check\"}"), Domain::ErrorCodes::InvalidRequest,
        "Duplicate event keys were accepted.");
    requireError(Notifier::buildPayload("[]"), Domain::ErrorCodes::InvalidRequest, "A nonobject notification was accepted.");
    auto unknown = completed(); unknown["event"] = "approved";
    requireError(Notifier::buildPayload(unknown.dump()), Domain::ErrorCodes::InvalidRequest, "An invented approval transition was accepted.");
    auto invalid = completed(std::string(1U, '\0'));
    requireError(Notifier::buildPayload(invalid.dump()), Domain::ErrorCodes::InvalidRequest, "NUL text was accepted.");
    requireError(Notifier::buildPayload(std::string(Notifier::MaximumEventBytes + 1U, ' ')), Domain::ErrorCodes::PayloadTooLarge,
        "An oversized notification event was accepted.");
    Notifier notifier{"unrelated-app!recipient"};
    requireError(notifier.submit(completed().dump(), context()), Domain::ErrorCodes::InvalidRequest,
        "A host-supplied unrelated recipient was accepted.");
    requireError(Notifier::applicationIdForInstalledExecutable("relative\\ForgeConductor.Manager.exe"), Domain::ErrorCodes::InvalidRequest,
        "A relative executable path was used as a package identity.");
    requireError(Notifier::applicationIdForInstalledExecutable("C:\\unregistered-notification-package\\ForgeConductor.Manager.exe"), Domain::ErrorCodes::HostCapabilityUnavailable,
        "An unregistered executable directory was used as a package identity.");
    auto cancelled = context(); std::stop_source stop; stop.request_stop(); cancelled.cancellation = stop.get_token();
    requireError(notifier.submit(completed().dump(), cancelled), Domain::ErrorCodes::Cancelled, "Cancelled input attempted a notification.");
    auto expired = context(); expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
    requireError(notifier.submit(completed().dump(), expired), Domain::ErrorCodes::DeadlineExceeded, "Expired input attempted a notification.");
}
} // namespace
} // namespace ForgeConductor::Tests

int main(int argc, char** argv) {
    using namespace ForgeConductor::Tests;
    try {
        payloadEscapesBoundsAndReportsActualState(); std::cout << "PASS toast.payload_bounds_escaping_actual_state\n";
        invalidPayloadAndNoImplicitSubmission(); std::cout << "PASS toast.validation_cancel_no_side_effect\n";
        if (argc == 3 && std::string_view{argv[1]} == "--probe-installed") {
            std::cout << "PROBE " << take(Notifier::applicationIdForInstalledExecutable(argv[2])) << '\n';
        } else if (argc == 3 && std::string_view{argv[1]} == "--live-toast") {
            const ForgeConductor::Infrastructure::Windows::WindowsScheduledTaskNotifier notifier{argv[2]};
            const auto receipt = take(notifier.submit(completed("Forge Conductor owned notifier check").dump(), context()));
            const auto result = nlohmann::json::parse(receipt);
            require(result.at("submission_accepted") == true && result.at("display_confirmed") == false,
                "The live notifier did not distinguish submission from display.");
            std::cout << "LIVE " << receipt << '\n';
        } else if (argc != 1) throw TestFailure{"Use --probe-installed <installed-executable> or --live-toast <verified-installed-Forge-Conductor-AUMID>."};
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
