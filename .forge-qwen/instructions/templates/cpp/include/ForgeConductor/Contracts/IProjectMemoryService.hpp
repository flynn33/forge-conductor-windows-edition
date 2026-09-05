#pragma once
#include "Result.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <vector>
namespace forge {
struct ProjectDescriptor final { std::string id; std::string displayName; std::optional<std::string> repositoryIdentity; std::vector<std::wstring> aliases; };
struct MemoryWrite final { std::string kind; std::string title; std::string summary; std::optional<std::string> body; std::vector<std::string> tags; std::optional<std::string> sessionId; double importance{}; double confidence{}; std::optional<std::string> idempotencyKey; };
class IProjectMemoryService { public: virtual ~IProjectMemoryService()=default; virtual Result<ProjectDescriptor> initialize(std::wstring const&,std::optional<std::string> const&,std::optional<std::string> const&,std::chrono::steady_clock::time_point)=0; virtual Result<std::string> remember(std::string const&,MemoryWrite const&,std::chrono::steady_clock::time_point)=0; virtual Result<bool> resetProjectMemory(std::string const&,std::string const&)=0; virtual void shutdown() noexcept=0;}; }
