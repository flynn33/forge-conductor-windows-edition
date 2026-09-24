#pragma once

#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include <functional>
#include <map>
#include <mutex>

namespace ForgeConductor::Infrastructure::Windows {

// A new run reads effective settings. Existing runs and their successors retain
// their selected transport even if another project changes provider settings.
class SettingsBoundResponsesTransport final : public Contracts::IManagedResponsesTransport,
    public Contracts::INativeSessionTransport {
public:
    using Resolver = std::function<Domain::Result<LMStudioResponsesTransportConfiguration>(const Domain::OperationContext&)>;
    using LoadBindings = std::function<Domain::Result<std::string>(const Domain::OperationContext&)>;
    using SaveBindings = std::function<Domain::Result<void>(const std::string&, const Domain::OperationContext&)>;
    explicit SettingsBoundResponsesTransport(Resolver resolver, LoadBindings load = {}, SaveBindings save = {});
    ~SettingsBoundResponsesTransport() override;
    Domain::Result<Domain::ManagedProviderTurnResult> complete(const Domain::ManagedProviderTurnRequest&, const Domain::OperationContext&) noexcept override;
    Domain::Result<Domain::NativeTransportSession> createSession(const Domain::SessionCreationRequest&, const Domain::OperationContext&) noexcept override;
    Domain::Result<Domain::NativeBootstrapResponse> bootstrap(const Domain::NativeBootstrapRequest&, const Domain::OperationContext&) noexcept override;
    Domain::Result<Domain::HostSessionStatus> query(const Domain::ProviderSessionId&, const Domain::OperationContext&) noexcept override;
    void cancel(const Domain::OperationId&, const std::optional<Domain::ProviderSessionId>&) noexcept override;
    void shutdown() noexcept override;
private:
    struct Binding {
        std::string project;
        std::shared_ptr<LMStudioResponsesTransport> transport;
        LMStudioResponsesTransportConfiguration configuration;
    };
    Domain::Result<Binding> bind(const std::string& run, const std::string& project,
        const std::optional<std::string>& response, const Domain::OperationContext&);
    void finish(const Domain::OperationContext&, const Binding&, const std::optional<std::string>&);
    Resolver resolver_;
    LoadBindings load_;
    SaveBindings save_;
    bool loaded_{};
    void load(const Domain::OperationContext&);
    void save(const Domain::OperationContext&);
    std::mutex mutex_;
    bool stopped_{};
    std::map<std::string, Binding> runs_;
    std::map<std::string, Binding> responses_;
    std::map<std::string, Binding> operations_;
};
}
