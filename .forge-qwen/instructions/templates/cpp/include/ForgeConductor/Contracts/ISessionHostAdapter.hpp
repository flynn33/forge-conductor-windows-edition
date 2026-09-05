#pragma once
#include "Result.hpp"
#include <chrono>
#include <optional>
#include <string>
namespace forge {
struct HostCapabilities final { bool createSession{}; bool bootstrap{}; bool acknowledge{}; bool cancel{}; bool recover{}; };
struct SessionCreationRequest final { std::string operationId; std::string projectId; std::string predecessorSessionId; std::string idempotencyKey; std::optional<std::string> model; };
struct HostSession final { std::string logicalSessionId; std::string providerSessionId; std::optional<std::string> model; };
class ISessionHostAdapter { public: virtual ~ISessionHostAdapter()=default; virtual std::string identifier() const=0; virtual HostCapabilities capabilities() const noexcept=0; virtual Result<HostSession> createSession(SessionCreationRequest const&,std::chrono::steady_clock::time_point)=0; virtual Result<bool> bootstrap(HostSession const&,std::string const&,std::string const&,std::chrono::steady_clock::time_point)=0; virtual void cancel(std::string const&) noexcept=0; virtual void shutdown() noexcept=0;}; }
