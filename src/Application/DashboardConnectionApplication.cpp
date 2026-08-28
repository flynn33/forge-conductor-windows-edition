#include "ForgeConductor/Application/DashboardConnectionApplication.h"

#include "ForgeConductor/Dashboard/DashboardApplicationErrorMapper.h"
#include "ForgeConductor/Dashboard/DashboardApplicationJsonCodec.h"
#include "ForgeConductor/Dashboard/DashboardManagerJsonCodec.h"
#include "ForgeConductor/Dashboard/DashboardRequestPlanner.h"
#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequestDecoder.h"
#include "ForgeConductor/Dashboard/DashboardTelemetryJsonCodec.h"
#include "ForgeConductor/Domain/Error.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

constexpr std::string_view JsonContentType =
    "application/json; charset=utf-8";
constexpr std::string_view HtmlContentType = "text/html; charset=utf-8";
constexpr std::string_view TextContentType = "text/plain; charset=utf-8";
constexpr std::string_view NotFoundBody = "Not Found";
constexpr std::string_view MissingSessionIdBody =
    R"({"ok":false,"message":"session_id required"})";
constexpr std::string_view PingBody =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>Forge Telemetry OK</title></head>"
    "<body style=\"background:#02040a;color:#e8fbff;"
    "font-family:system-ui;padding:2rem\">"
    "<h1 style=\"color:#18f0ff\">Forge Telemetry is reachable</h1>"
    "<p>Integrated Windows host &middot; continuous native collectors</p>"
    "<p><a style=\"color:#18f0ff\" href=\"/\">Open dashboard</a> "
    "&middot; <a style=\"color:#18f0ff\" href=\"/api/health\">"
    "/api/health</a> &middot; <a style=\"color:#18f0ff\" "
    "href=\"/api/stream\">/api/stream (SSE realtime)</a> &middot; "
    "<a style=\"color:#18f0ff\" href=\"/api/live\">/api/live</a> "
    "&middot; <a style=\"color:#18f0ff\" href=\"/control\">"
    "Manager controls</a></p></body></html>";

using PreparedResult =
    Domain::Result<Dashboard::DashboardPreparedExchange>;

[[nodiscard]] Domain::Error internalError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::move(message));
}

[[nodiscard]] PreparedResult mappedErrorResponse(
    const Domain::Error& error,
    const Dashboard::DashboardApplicationErrorOrigin origin,
    const bool head) noexcept
{
    const auto rejection =
        Dashboard::DashboardApplicationErrorMapper::map(error, origin);
    return head
        ? Dashboard::DashboardResponseComposer::headRejection(rejection)
        : Dashboard::DashboardResponseComposer::rejection(rejection);
}

[[nodiscard]] PreparedResult checkedComposition(
    PreparedResult response,
    const bool head) noexcept
{
    if (response) {
        return response;
    }
    return mappedErrorResponse(
        response.error(),
        Dashboard::DashboardApplicationErrorOrigin::ResponseEncoding,
        head);
}

[[nodiscard]] PreparedResult jsonResponse(
    Domain::Result<std::string> encoded,
    const bool head,
    const Dashboard::DashboardPostDeliveryAction action =
        Dashboard::DashboardPostDeliveryAction::None) noexcept
{
    if (!encoded) {
        return mappedErrorResponse(
            encoded.error(),
            Dashboard::DashboardApplicationErrorOrigin::ResponseEncoding,
            head);
    }

    auto body = std::move(encoded).value();
    if (head) {
        return checkedComposition(
            Dashboard::DashboardResponseComposer::head(
                200U, std::string{JsonContentType}, body.size()),
            true);
    }
    return checkedComposition(
        Dashboard::DashboardResponseComposer::completeText(
            200U,
            std::string{JsonContentType},
            std::move(body),
            {},
            action),
        false);
}

[[nodiscard]] PreparedResult jsonByteResponse(
    Domain::Result<std::vector<std::byte>> encoded,
    const bool head) noexcept
{
    if (!encoded) {
        return mappedErrorResponse(
            encoded.error(),
            Dashboard::DashboardApplicationErrorOrigin::ResponseEncoding,
            head);
    }

    auto body = std::move(encoded).value();
    if (head) {
        return checkedComposition(
            Dashboard::DashboardResponseComposer::head(
                200U, std::string{JsonContentType}, body.size()),
            true);
    }
    return checkedComposition(
        Dashboard::DashboardResponseComposer::complete(
            200U, std::string{JsonContentType}, std::move(body)),
        false);
}

[[nodiscard]] PreparedResult textResponse(
    const std::uint16_t status,
    const std::string_view contentType,
    const std::string_view body,
    const bool head) noexcept
{
    if (head) {
        return checkedComposition(
            Dashboard::DashboardResponseComposer::head(
                status, std::string{contentType}, body.size()),
            true);
    }
    return checkedComposition(
        Dashboard::DashboardResponseComposer::completeText(
            status, std::string{contentType}, std::string{body}),
        false);
}

[[nodiscard]] Dashboard::DashboardShellAssetId shellAssetId(
    const Dashboard::DashboardRouteId route) noexcept
{
    switch (route) {
    case Dashboard::DashboardRouteId::ShellRoot:
        return Dashboard::DashboardShellAssetId::Root;
    case Dashboard::DashboardRouteId::ShellIndex:
        return Dashboard::DashboardShellAssetId::Index;
    case Dashboard::DashboardRouteId::ShellControl:
        return Dashboard::DashboardShellAssetId::Control;
    case Dashboard::DashboardRouteId::ShellManager:
        return Dashboard::DashboardShellAssetId::Manager;
    default:
        return static_cast<Dashboard::DashboardShellAssetId>(0xffU);
    }
}

[[nodiscard]] PreparedResult assetResponse(
    const Dashboard::DashboardStaticAssetHandle& asset,
    const bool head) noexcept
{
    if (asset == nullptr) {
        return mappedErrorResponse(
            internalError("The dashboard asset store returned a null asset."),
            Dashboard::DashboardApplicationErrorOrigin::Dependency,
            head);
    }

    if (head) {
        return checkedComposition(
            Dashboard::DashboardResponseComposer::head(
                200U,
                asset->mimeType(),
                asset->bytes().size()),
            true);
    }
    try {
        std::vector<std::byte> body{
            asset->bytes().begin(), asset->bytes().end()};
        return checkedComposition(
            Dashboard::DashboardResponseComposer::complete(
                200U,
                asset->mimeType(),
                std::move(body)),
            false);
    } catch (...) {
        return mappedErrorResponse(
            internalError("The dashboard asset response could not be copied."),
            Dashboard::DashboardApplicationErrorOrigin::ResponseEncoding,
            false);
    }
}

[[nodiscard]] PreparedResult dependencyFailure(
    const Domain::Error& error,
    const bool head = false) noexcept
{
    return mappedErrorResponse(
        error,
        Dashboard::DashboardApplicationErrorOrigin::Dependency,
        head);
}

[[nodiscard]] std::span<const std::byte> bodySpan(
    const Dashboard::DashboardHttpRequest& request) noexcept
{
    return std::span<const std::byte>{
        request.body().data(), request.body().size()};
}

} // namespace

Domain::Result<Dashboard::DashboardPreparedExchange>
DashboardConnectionApplication::prepare(
    Dashboard::DashboardHttpRequest request,
    const bool operationalServiceActive,
    Domain::OperationContext context) noexcept
{
    const bool head = request.method() == "HEAD";
    try {
        const auto plan = Dashboard::DashboardRequestPlanner::plan(
            policy_, request, operationalServiceActive, budgets_);
        if (plan.kind() == Dashboard::DashboardRequestPlanKind::Rejected) {
            if (plan.rejection() == nullptr) {
                return dependencyFailure(
                    internalError(
                        "The dashboard request planner returned no rejection."),
                    head);
            }
            return head
                ? Dashboard::DashboardResponseComposer::headRejection(
                      *plan.rejection())
                : Dashboard::DashboardResponseComposer::rejection(
                      *plan.rejection());
        }

        const auto* route = plan.route();
        if (route == nullptr) {
            return dependencyFailure(
                internalError("The dashboard request plan has no route."),
                head);
        }

        if (plan.kind() ==
            Dashboard::DashboardRequestPlanKind::StaticResource) {
            if (plan.staticResource() == nullptr) {
                return dependencyFailure(
                    internalError(
                        "The static dashboard plan has no resource path."),
                    head);
            }
            auto asset = assetStore_.findStaticAsset(*plan.staticResource());
            if (!asset) {
                if (asset.error().code == Domain::ErrorCodes::RecordNotFound) {
                    return textResponse(
                        404U, TextContentType, NotFoundBody, head);
                }
                return dependencyFailure(asset.error(), head);
            }
            return assetResponse(asset.value(), head);
        }

        if (plan.kind() ==
            Dashboard::DashboardRequestPlanKind::ServerSentEvents) {
            if (plan.streamRate() == nullptr) {
                return dependencyFailure(
                    internalError(
                        "The dashboard stream plan has no delivery rate."),
                    head);
            }
            auto subscription =
                telemetrySource_.subscribe(*plan.streamRate(), context);
            if (!subscription) {
                return mappedErrorResponse(
                    subscription.error(),
                    Dashboard::DashboardApplicationErrorOrigin::
                        SseSubscription,
                    head);
            }
            auto prepared = Dashboard::DashboardResponseComposer::
                serverSentEvents(std::move(subscription).value());
            if (!prepared) {
                return mappedErrorResponse(
                    prepared.error(),
                    Dashboard::DashboardApplicationErrorOrigin::
                        ResponseEncoding,
                    head);
            }
            return prepared;
        }

        switch (route->id()) {
        case Dashboard::DashboardRouteId::ShellRoot:
        case Dashboard::DashboardRouteId::ShellIndex:
        case Dashboard::DashboardRouteId::ShellControl:
        case Dashboard::DashboardRouteId::ShellManager: {
            auto asset = assetStore_.findShellAsset(shellAssetId(route->id()));
            if (!asset) {
                return dependencyFailure(
                    internalError(
                        "A required dashboard shell asset is unavailable."),
                    head);
            }
            return assetResponse(asset.value(), head);
        }
        case Dashboard::DashboardRouteId::TelemetryHealth: {
            auto health = telemetrySource_.health(context);
            if (!health) return dependencyFailure(health.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeHealth(
                    health.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::TelemetryLive:
        case Dashboard::DashboardRouteId::TelemetryFrame:
        case Dashboard::DashboardRouteId::TelemetrySnapshot:
        case Dashboard::DashboardRouteId::TelemetrySystem:
        case Dashboard::DashboardRouteId::TelemetryForge: {
            auto observation = telemetrySource_.latest(context);
            if (!observation) {
                return dependencyFailure(observation.error(), head);
            }
            if (observation.value().snapshot == nullptr) {
                return dependencyFailure(
                    internalError(
                        "The telemetry source returned no current snapshot."),
                    head);
            }
            const auto& value = observation.value();
            const auto maximumBytes = Dashboard::DashboardResponseComposer::
                MaximumResponseBodyBytes;
            if (route->id() == Dashboard::DashboardRouteId::TelemetryLive) {
                return jsonResponse(
                    Dashboard::DashboardTelemetryJsonCodec::encodeLiveFrame(
                        *value.snapshot,
                        value.measuredSampleHz,
                        maximumBytes),
                    head);
            }
            if (route->id() == Dashboard::DashboardRouteId::TelemetryFrame) {
                return jsonResponse(
                    Dashboard::DashboardTelemetryJsonCodec::encodeFrame(
                        *value.snapshot,
                        value.measuredSampleHz,
                        maximumBytes),
                    head);
            }
            if (route->id() ==
                Dashboard::DashboardRouteId::TelemetrySnapshot) {
                return jsonResponse(
                    Dashboard::DashboardTelemetryJsonCodec::encodeSnapshot(
                        *value.snapshot,
                        value.measuredSampleHz,
                        maximumBytes),
                    head);
            }
            if (route->id() == Dashboard::DashboardRouteId::TelemetrySystem) {
                return jsonResponse(
                    Dashboard::DashboardTelemetryJsonCodec::encodeSystem(
                        value.snapshot->system, maximumBytes),
                    head);
            }
            return jsonResponse(
                Dashboard::DashboardTelemetryJsonCodec::encodeForge(
                    value.snapshot->forge, maximumBytes),
                head);
        }
        case Dashboard::DashboardRouteId::TelemetryPing:
            return textResponse(200U, HtmlContentType, PingBody, head);
        case Dashboard::DashboardRouteId::Status: {
            auto operational = operationalService_.status(context);
            if (!operational) {
                return dependencyFailure(operational.error(), head);
            }
            auto telemetry = telemetrySource_.health(context);
            if (!telemetry) {
                return dependencyFailure(telemetry.error(), head);
            }
            auto manager = managerClient_.status(context);
            if (!manager) return dependencyFailure(manager.error(), head);

            Dashboard::DashboardApplicationStatus status{
                identity_,
                std::move(operational).value(),
                std::move(telemetry).value(),
                std::move(manager).value(),
                dashboardHost_,
                dashboardPort_,
                operationalServiceActive};
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeStatus(
                    status,
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalDoctor: {
            auto result = operationalService_.doctor(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeDoctor(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalAgents: {
            auto result = operationalService_.agents(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeAgents(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalSessions: {
            auto result = operationalService_.sessions(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeSessions(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalAudit: {
            auto result = operationalService_.audit(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeAudit(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalDiagnostics: {
            auto result = operationalService_.diagnosticLines(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeDiagnostics(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalSessionsPrune: {
            auto result = operationalService_.pruneSessions(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::
                    encodePruneAcknowledgement(
                        Dashboard::DashboardResponseComposer::
                            MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::OperationalSessionsClose: {
            auto decoded =
                Dashboard::DashboardSessionCloseRequestDecoder::decode(
                    bodySpan(request));
            if (!decoded) {
                if (decoded.error().code ==
                        Domain::ErrorCodes::InvalidRequest &&
                    decoded.error().message == "session_id required") {
                    return textResponse(
                        400U,
                        JsonContentType,
                        MissingSessionIdBody,
                        head);
                }
                return mappedErrorResponse(
                    decoded.error(),
                    Dashboard::DashboardApplicationErrorOrigin::RequestBody,
                    head);
            }
            auto result = operationalService_.closeSession(
                decoded.value(), context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::encodeClosedSession(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::ManagerStatus: {
            auto result = managerClient_.status(context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonByteResponse(
                Dashboard::DashboardManagerJsonCodec::encodeStatus(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::ManagerSettings: {
            if (request.method() == "GET") {
                auto result = managerClient_.settings(context);
                if (!result) return dependencyFailure(result.error(), head);
                return jsonByteResponse(
                    Dashboard::DashboardManagerJsonCodec::encodeSettings(
                        result.value(),
                        Dashboard::DashboardResponseComposer::
                            MaximumResponseBodyBytes),
                    head);
            }
            auto mutation =
                Dashboard::DashboardManagerJsonCodec::decodeSettingsMutation(
                    bodySpan(request));
            if (!mutation) {
                return mappedErrorResponse(
                    mutation.error(),
                    Dashboard::DashboardApplicationErrorOrigin::RequestBody,
                    head);
            }
            auto result = managerClient_.updateSettings(
                mutation.value().patch(),
                mutation.value().apply(),
                context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonByteResponse(
                Dashboard::DashboardManagerJsonCodec::
                    encodeSettingsUpdateOutcome(
                        result.value(),
                        Dashboard::DashboardResponseComposer::
                            MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::ManagerStart:
        case Dashboard::DashboardRouteId::ManagerStop:
        case Dashboard::DashboardRouteId::ManagerRestart: {
            Domain::ManagerControlAction action{
                Domain::ManagerControlAction::Start};
            if (route->id() == Dashboard::DashboardRouteId::ManagerStop) {
                action = Domain::ManagerControlAction::Stop;
            } else if (
                route->id() == Dashboard::DashboardRouteId::ManagerRestart) {
                action = Domain::ManagerControlAction::Restart;
            }
            auto result = managerClient_.control(
                Domain::ManagerControlRequest{action}, context);
            if (!result) return dependencyFailure(result.error(), head);
            return jsonByteResponse(
                Dashboard::DashboardManagerJsonCodec::encodeStatus(
                    result.value(),
                    Dashboard::DashboardResponseComposer::
                        MaximumResponseBodyBytes),
                head);
        }
        case Dashboard::DashboardRouteId::ManagerShutdown:
            return jsonResponse(
                Dashboard::DashboardApplicationJsonCodec::
                    encodeShutdownAcknowledgement(
                        Dashboard::DashboardResponseComposer::
                            MaximumResponseBodyBytes),
                head,
                Dashboard::DashboardPostDeliveryAction::
                    RequestManagerShutdown);
        case Dashboard::DashboardRouteId::Unknown:
        case Dashboard::DashboardRouteId::StaticResource:
        case Dashboard::DashboardRouteId::TelemetryStream:
        case Dashboard::DashboardRouteId::UnavailableToolsCall:
            return dependencyFailure(
                internalError(
                    "The dashboard planner dispatched an invalid route."),
                head);
        }
        return dependencyFailure(
            internalError("The dashboard route value is invalid."), head);
    } catch (...) {
        return dependencyFailure(
            internalError("The dashboard request failed safely."), head);
    }
}

Domain::Result<void> DashboardConnectionApplication::executePostDelivery(
    const Dashboard::DashboardPostDeliveryAction action,
    Domain::OperationContext context) noexcept
{
    try {
        switch (action) {
        case Dashboard::DashboardPostDeliveryAction::None:
            return Domain::Result<void>::success();
        case Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown:
            return managerClient_.requestShutdown(context);
        }
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard post-delivery action is invalid."));
    } catch (...) {
        return Domain::Result<void>::failure(internalError(
            "The dashboard post-delivery action failed safely."));
    }
}

} // namespace ForgeConductor::Application
