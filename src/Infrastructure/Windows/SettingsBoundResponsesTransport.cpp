#include "ForgeConductor/Infrastructure/Windows/SettingsBoundResponsesTransport.h"
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
template<class T> Domain::Result<T> failed(const std::string& detail)
{ return Domain::Result<T>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, detail)); }
}

SettingsBoundResponsesTransport::SettingsBoundResponsesTransport(Resolver resolver, LoadBindings load, SaveBindings save)
    : resolver_{std::move(resolver)}, load_{std::move(load)}, save_{std::move(save)} {}
SettingsBoundResponsesTransport::~SettingsBoundResponsesTransport() { shutdown(); }

void SettingsBoundResponsesTransport::load(const Domain::OperationContext& context)
{
    if (loaded_) return;
    if (load_) {
        auto content = load_(context);
        if (!content) throw std::runtime_error{content.error().message};
        if (!content.value().empty()) {
            const auto document = nlohmann::json::parse(content.value());
            if (document.at("schema") != 1 || document.at("runs").size() > 4096 || document.at("responses").size() > 65536)
                throw std::runtime_error{"Persisted provider bindings are invalid."};
            std::map<std::string, Binding> runs, responses;
            const auto decode = [](const auto& records, auto& target) {
                for (const auto& [key, item] : records.items()) {
                    LMStudioResponsesTransportConfiguration config;
                    config.loopbackHost = item.at("host").template get<std::string>();
                    const auto port = item.at("port").template get<unsigned>();
                    if ((config.loopbackHost != "127.0.0.1" && config.loopbackHost != "localhost" && config.loopbackHost != "::1") || port == 0 || port > 65535)
                        throw std::runtime_error{"Persisted provider endpoint is invalid."};
                    config.port = static_cast<std::uint16_t>(port);
                    config.secure = item.at("secure").template get<bool>();
                    config.basePath = item.at("base_path").template get<std::string>();
                    if (!item.at("model").is_null()) config.model = item.at("model").template get<std::string>();
                    target.emplace(key, Binding{item.at("project").template get<std::string>(),
                        std::make_shared<LMStudioResponsesTransport>(config), config});
                }
            };
            decode(document.at("runs"), runs);
            decode(document.at("responses"), responses);
            runs_ = std::move(runs); responses_ = std::move(responses);
        }
    }
    loaded_ = true;
}

void SettingsBoundResponsesTransport::save(const Domain::OperationContext& context)
{
    if (!save_) return;
    nlohmann::json document{{"schema", 1}, {"runs", nlohmann::json::object()}, {"responses", nlohmann::json::object()}};
    const auto encode = [](const auto& bindings, auto& target) {
        for (const auto& [key, binding] : bindings) {
            const auto& config = binding.configuration;
            if (config.bearerToken) throw std::runtime_error{"Credential-bearing provider bindings require a protected credential store."};
            target[key] = {{"project", binding.project}, {"host", config.loopbackHost}, {"port", config.port},
                {"secure", config.secure}, {"base_path", config.basePath},
                {"model", config.model ? nlohmann::json(*config.model) : nlohmann::json(nullptr)}};
        }
    };
    encode(runs_, document["runs"]); encode(responses_, document["responses"]);
    auto stored = save_(document.dump(), context);
    if (!stored) throw std::runtime_error{stored.error().message};
}

Domain::Result<SettingsBoundResponsesTransport::Binding> SettingsBoundResponsesTransport::bind(
    const std::string& run, const std::string& project, const std::optional<std::string>& response,
    const Domain::OperationContext& context)
{
    std::lock_guard lock{mutex_};
    if (stopped_) return failed<Binding>("The provider transport is stopped.");
    load(context);
    const auto remember = [&](const Binding& binding) {
        if (!project.empty() && binding.project != project)
            return failed<Binding>("Provider binding belongs to another project.");
        if (!run.empty() && !runs_.contains(run)) {
            if (runs_.size() >= 4096U) return failed<Binding>("Provider run binding history is full.");
            runs_.insert_or_assign(run, binding);
            try { save(context); } catch (...) { runs_.erase(run); throw; }
        }
        operations_.insert_or_assign(context.operationId.value(), binding);
        return Domain::Result<Binding>::success(binding);
    };
    if (const auto found = runs_.find(run); !run.empty() && found != runs_.end())
        return remember(found->second);
    if (response) {
        if (const auto found = responses_.find(*response); found != responses_.end())
            return remember(found->second);
        if (save_) return failed<Binding>("This provider response has no saved connection binding. Start a new run; its history cannot safely be moved to the current model.");
    }
    if (runs_.size() >= 4096U || responses_.size() >= 65536U)
        return failed<Binding>("Provider binding history is full. Finish active work and restart the Manager.");
    auto configuration = resolver_(context);
    if (!configuration) return Domain::Result<Binding>::failure(configuration.error());
    if (save_ && !configuration.value().model)
        return failed<Binding>("Prepare the project or select a model in Provider settings before starting a durable run.");
    return remember(Binding{project, std::make_shared<LMStudioResponsesTransport>(configuration.value()), configuration.value()});
}

void SettingsBoundResponsesTransport::finish(const Domain::OperationContext& context,
    const Binding& binding, const std::optional<std::string>& response)
{
    std::lock_guard lock{mutex_};
    operations_.erase(context.operationId.value());
    if (response && !stopped_) {
        if (!responses_.contains(*response) && responses_.size() >= 65536U)
            throw std::runtime_error{"Provider response binding history is full."};
        responses_.insert_or_assign(*response, binding);
        save(context);
    }
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
