#include "ForgeConductor/Infrastructure/Windows/SettingsBoundResponsesTransport.h"
#include <set>
#include <stdexcept>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
template<class T> Domain::Result<T> failed(const std::string& detail)
{ return Domain::Result<T>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, detail)); }
}

SettingsBoundResponsesTransport::SettingsBoundResponsesTransport(Resolver resolver)
    : resolver_{std::move(resolver)} {}
SettingsBoundResponsesTransport::~SettingsBoundResponsesTransport() { shutdown(); }

Domain::Result<SettingsBoundResponsesTransport::Binding> SettingsBoundResponsesTransport::bind(
    const std::string& run, const std::string& project, const std::optional<std::string>& response,
    const Domain::OperationContext& context)
{
    std::lock_guard lock{mutex_};
    if (stopped_) return failed<Binding>("The provider transport is stopped.");
    const auto remember = [&](const Binding& binding) {
        if (!project.empty() && binding.project != project)
            return failed<Binding>("Provider binding belongs to another project.");
        if (!run.empty()) runs_.insert_or_assign(run, binding);
        operations_.insert_or_assign(context.operationId.value(), binding);
        return Domain::Result<Binding>::success(binding);
    };
    if (const auto found = runs_.find(run); !run.empty() && found != runs_.end())
        return remember(found->second);
    if (response) {
        if (const auto found = responses_.find(*response); found != responses_.end())
            return remember(found->second);
    }
    if (runs_.size() >= 4096U || responses_.size() >= 65536U)
        return failed<Binding>("Provider binding history is full. Finish active work and restart the Manager.");
    auto configuration = resolver_(context);
    if (!configuration) return Domain::Result<Binding>::failure(configuration.error());
    return remember(Binding{project, std::make_shared<LMStudioResponsesTransport>(std::move(configuration).value())});
}

void SettingsBoundResponsesTransport::finish(const Domain::OperationContext& context,
    const Binding& binding, const std::optional<std::string>& response)
{
    std::lock_guard lock{mutex_};
    operations_.erase(context.operationId.value());
    if (response && !stopped_) responses_.insert_or_assign(*response, binding);
}

Domain::Result<Domain::ManagedProviderTurnResult> SettingsBoundResponsesTransport::complete(
    const Domain::ManagedProviderTurnRequest& request, const Domain::OperationContext& context) noexcept
{
    try {
        auto binding = bind(request.runId.value(), request.projectId.value(),
            request.previousResponseId ? std::optional<std::string>{request.previousResponseId->value()} : std::nullopt, context);
        if (!binding) return Domain::Result<Domain::ManagedProviderTurnResult>::failure(binding.error());
        auto result = binding.value().transport->complete(request, context);
        finish(context, binding.value(), result ? std::optional<std::string>{result.value().responseId.value()} : std::nullopt);
        return result;
    } catch (const std::exception& error) { return failed<Domain::ManagedProviderTurnResult>(error.what()); }
    catch (...) { return failed<Domain::ManagedProviderTurnResult>("Provider settings could not be bound to this run."); }
}

Domain::Result<Domain::NativeTransportSession> SettingsBoundResponsesTransport::createSession(
    const Domain::SessionCreationRequest& request, const Domain::OperationContext& context) noexcept
{
    try {
        auto binding = bind(request.predecessorSessionId.value(), request.projectId.value(), std::nullopt, context);
        if (!binding) return Domain::Result<Domain::NativeTransportSession>::failure(binding.error());
        auto result = binding.value().transport->createSession(request, context);
        finish(context, binding.value(), result ? std::optional<std::string>{result.value().providerSessionId.value()} : std::nullopt);
        return result;
    } catch (const std::exception& error) { return failed<Domain::NativeTransportSession>(error.what()); }
    catch (...) { return failed<Domain::NativeTransportSession>("Native session provider binding failed."); }
}

Domain::Result<Domain::NativeBootstrapResponse> SettingsBoundResponsesTransport::bootstrap(
    const Domain::NativeBootstrapRequest& request, const Domain::OperationContext& context) noexcept
{
    try {
        auto binding = bind(request.successorSessionId.value(), request.projectId.value(), request.providerSessionId.value(), context);
        if (!binding) return Domain::Result<Domain::NativeBootstrapResponse>::failure(binding.error());
        auto result = binding.value().transport->bootstrap(request, context);
        finish(context, binding.value(), result && result.value().providerResponseId
            ? std::optional<std::string>{result.value().providerResponseId->value()} : std::nullopt);
        return result;
    } catch (const std::exception& error) { return failed<Domain::NativeBootstrapResponse>(error.what()); }
    catch (...) { return failed<Domain::NativeBootstrapResponse>("Native bootstrap provider binding failed."); }
}

Domain::Result<Domain::HostSessionStatus> SettingsBoundResponsesTransport::query(
    const Domain::ProviderSessionId& response, const Domain::OperationContext& context) noexcept
{
    try {
        auto binding = bind({}, {}, response.value(), context);
        if (!binding) return Domain::Result<Domain::HostSessionStatus>::failure(binding.error());
        auto result = binding.value().transport->query(response, context);
        finish(context, binding.value(), std::nullopt);
        return result;
    } catch (const std::exception& error) { return failed<Domain::HostSessionStatus>(error.what()); }
    catch (...) { return failed<Domain::HostSessionStatus>("Provider status could not be read."); }
}

void SettingsBoundResponsesTransport::cancel(const Domain::OperationId& operation,
    const std::optional<Domain::ProviderSessionId>& response) noexcept
{
    try {
        std::shared_ptr<LMStudioResponsesTransport> transport;
        {
            std::lock_guard lock{mutex_};
            if (const auto found = operations_.find(operation.value()); found != operations_.end()) transport = found->second.transport;
            else if (response) {
                if (const auto known = responses_.find(response->value()); known != responses_.end()) transport = known->second.transport;
            }
        }
        if (transport) transport->cancel(operation, response);
    } catch (...) {}
}

void SettingsBoundResponsesTransport::shutdown() noexcept
{
    try {
        std::set<std::shared_ptr<LMStudioResponsesTransport>> transports;
        {
            std::lock_guard lock{mutex_};
            stopped_ = true;
            for (const auto& [key, value] : runs_) { (void)key; transports.insert(value.transport); }
            for (const auto& [key, value] : responses_) { (void)key; transports.insert(value.transport); }
            for (const auto& [key, value] : operations_) { (void)key; transports.insert(value.transport); }
            runs_.clear(); responses_.clear(); operations_.clear();
        }
        for (const auto& transport : transports) transport->shutdown();
    } catch (...) {}
}
}
