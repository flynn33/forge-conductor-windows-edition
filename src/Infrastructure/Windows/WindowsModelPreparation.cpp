#include "ForgeConductor/Infrastructure/Windows/WindowsModelPreparation.h"
#include "Detail/CommandLineBuilder.h"
#include "Detail/UniqueHandle.h"

#include <Windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
class ServerUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct Internet final {
    explicit Internet(HINTERNET value) : value_{value} {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    HINTERNET get() const noexcept { return value_.load(); }
    void close() noexcept { if (auto value = value_.exchange(nullptr)) WinHttpCloseHandle(value); }
    ~Internet() { close(); }
private:
    std::atomic<HINTERNET> value_{};
};

void check(const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) throw std::runtime_error{"Preparation cancelled."};
    if (context.isExpired(std::chrono::steady_clock::now()))
        throw std::runtime_error{"Model preparation timed out. Retry or select a smaller model."};
}

Json request(const Domain::ManagerSettings& settings, const wchar_t* path,
    const Json* body, const Domain::OperationContext& context)
{
    check(context);
    Internet session{WinHttpOpen(L"Forge Conductor setup", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.get()) throw std::runtime_error{"Cannot initialize local model connection."};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        context.deadline - std::chrono::steady_clock::now()).count();
    const int receiveTimeout = static_cast<int>(std::clamp<std::int64_t>(remaining, 1, body ? 120000 : 5000));
    if (!WinHttpSetTimeouts(session.get(), 2000, 2000, 5000, receiveTimeout))
        throw std::runtime_error{"Cannot set model preparation timeout."};
    const auto host = Detail::CommandLineBuilder::utf8ToUtf16(settings.localModelHost);
    if (!host) throw std::runtime_error{host.error().message};
    Internet connection{WinHttpConnect(session.get(), host.value().c_str(), settings.localModelPort, 0)};
    Internet operation{connection.get() ? WinHttpOpenRequest(connection.get(), body ? L"POST" : L"GET",
        path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        settings.localModelSecure ? WINHTTP_FLAG_SECURE : 0) : nullptr};
    const auto data = body ? body->dump() : std::string{};
    std::stop_callback cancel{context.cancellation, [&operation]() noexcept { operation.close(); }};
    if (!operation.get() || !WinHttpSendRequest(operation.get(),
            body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
            body ? static_cast<DWORD>(-1L) : 0,
            data.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(data.data()),
            static_cast<DWORD>(data.size()), static_cast<DWORD>(data.size()), 0) ||
        !WinHttpReceiveResponse(operation.get(), nullptr))
        throw ServerUnavailable{"LM Studio is not responding at the configured local address."};
    check(context);
    DWORD status{}, size{sizeof(status)};
    if (!WinHttpQueryHeaders(operation.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error{"LM Studio model management returned HTTP " + std::to_string(status) +
            ". Check server authentication or update LM Studio."};
    std::string response;
    for (;;) {
        check(context);
        std::array<char, 8192> buffer{};
        DWORD bytes{};
        if (!WinHttpReadData(operation.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes))
            throw std::runtime_error{"LM Studio model metadata could not be read."};
        if (bytes == 0) break;
        if (response.size() + bytes > 1024U * 1024U)
            throw std::runtime_error{"LM Studio model metadata exceeds the preparation limit."};
        response.append(buffer.data(), bytes);
    }
    return Json::parse(response);
}

void startServer(const Domain::ManagerSettings& settings, const Domain::OperationContext& context)
{
    check(context);
    if (settings.localModelSecure || (settings.localModelHost != "127.0.0.1" && settings.localModelHost != "localhost"))
        throw std::runtime_error{"Start the configured secure or custom LM Studio server, then retry."};
    std::array<wchar_t, 32768> profile{};
    const auto count = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
    if (!count || count >= profile.size()) throw std::runtime_error{"Cannot locate the LM Studio installation."};
    const auto executable = std::filesystem::path{profile.data()} / L".lmstudio" / L"bin" / L"lms.exe";
    if (!std::filesystem::is_regular_file(executable))
        throw std::runtime_error{"Install LM Studio and its CLI, then retry. Your registered project is saved."};
    auto arguments = Detail::CommandLineBuilder::buildCommandLine(executable.wstring(),
        {"server", "start", "--bind", "127.0.0.1", "--port", std::to_string(settings.localModelPort)});
    if (!arguments) throw std::runtime_error{arguments.error().message};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), arguments.value().data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process))
        throw std::runtime_error{"Windows could not start LM Studio. Open LM Studio and retry."};
    Detail::UniqueHandle ownedProcess{process.hProcess};
    Detail::UniqueHandle ownedThread{process.hThread};
    const auto deadline = std::min(context.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{30});
    for (;;) {
        const auto wait = WaitForSingleObject(ownedProcess.get(), 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) throw std::runtime_error{"Windows could not observe LM Studio startup. Retry preparation."};
        if (context.isCancellationRequested() || std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(ownedProcess.get(), 1);
            WaitForSingleObject(ownedProcess.get(), 5000);
            check(context);
            throw std::runtime_error{"LM Studio startup timed out. Retry preparation."};
        }
    }
    DWORD exit{};
    if (!GetExitCodeProcess(ownedProcess.get(), &exit) || exit != 0)
        throw std::runtime_error{"LM Studio could not start its server. Open LM Studio to resolve the startup error, then retry."};
}
}

Domain::Result<PreparedLocalModel> WindowsModelPreparation::prepare(
    const Domain::ManagerSettings& settings, const Domain::OperationContext& context) noexcept
{
    try {
        auto valid = Domain::validateManagerSettings(settings);
        if (!valid) return Domain::Result<PreparedLocalModel>::failure(valid.error());
        PreparedLocalModel result;
        Json inventory;
        try { inventory = request(settings, L"/api/v1/models", nullptr, context); }
        catch (const ServerUnavailable&) {
            check(context);
            startServer(settings, context);
            result.serverStarted = true;
            inventory = request(settings, L"/api/v1/models", nullptr, context);
        }
        if (!inventory.contains("models") || !inventory.at("models").is_array())
            throw std::runtime_error{"LM Studio returned no model inventory. Update LM Studio and retry."};
        const auto minimum = std::max<std::uint64_t>(settings.effectiveContextCapacity,
            static_cast<std::uint64_t>(settings.nextResponseReserve) +
            settings.handoffReserve + settings.estimationSafetyMargin + 4096U);
        Json candidate;
        std::uint64_t candidateSize = std::numeric_limits<std::uint64_t>::max();
        for (const auto& model : inventory.at("models")) {
            if (model.value("type", "") != "llm") continue;
            const auto key = model.value("key", "");
            const auto capabilities = model.value("capabilities", Json::object());
            if (!capabilities.value("trained_for_tool_use", false)) continue;
            for (const auto& instance : model.value("loaded_instances", Json::array())) {
                const auto identifier = instance.value("id", "");
                const auto capacity = instance.value("config", Json::object()).value("context_length", 0U);
                if (!identifier.empty() && capacity >= minimum &&
                    (settings.localModelName.empty() || settings.localModelName == identifier)) {
                    result.identifier = identifier;
                    result.contextCapacity = capacity;
                    return Domain::Result<PreparedLocalModel>::success(std::move(result));
                }
            }
            if (!settings.localModelName.empty() && settings.localModelName != key) continue;
            if (key.empty() || model.value("max_context_length", 0U) < minimum) continue;
            const auto bytes = model.value("size_bytes", std::uint64_t{});
            if (bytes > 0 && bytes < candidateSize) { candidate = model; candidateSize = bytes; }
        }
        if (candidate.is_null())
            throw std::runtime_error{"No downloaded tool-capable model has enough context. Download a coding model in LM Studio or change the selected model, then retry."};
        const auto capacity = std::min(settings.effectiveContextCapacity, candidate.at("max_context_length").get<std::uint32_t>());
        if (capacity < minimum)
            throw std::runtime_error{"The configured context capacity leaves too little room for work. Increase it in Provider settings and retry."};
        const Json load{{"model", candidate.at("key")}, {"context_length", capacity}};
        const auto loaded = request(settings, L"/api/v1/models/load", &load, context);
        result.identifier = loaded.value("instance_id", "");
        if (result.identifier.empty()) throw std::runtime_error{"LM Studio did not return the loaded model identity. Retry preparation."};
        inventory = request(settings, L"/api/v1/models", nullptr, context);
        for (const auto& model : inventory.at("models")) {
            if (model.value("type", "") != "llm" || model.value("key", "") != candidate.at("key").get<std::string>()) continue;
            for (const auto& instance : model.value("loaded_instances", Json::array())) {
                if (instance.value("id", "") != result.identifier) continue;
                result.contextCapacity = instance.value("config", Json::object()).value("context_length", 0U);
                if (result.contextCapacity < minimum) throw std::runtime_error{"Loaded model context is too small. Increase it in LM Studio and retry."};
                result.modelLoaded = true;
                return Domain::Result<PreparedLocalModel>::success(std::move(result));
            }
        }
        throw std::runtime_error{"The loaded model could not be verified. Retry preparation."};
    } catch (const std::exception& failure) {
        return Domain::Result<PreparedLocalModel>::failure(
            Domain::makeError(context.isCancellationRequested() ? Domain::ErrorCodes::Cancelled :
                context.isExpired(std::chrono::steady_clock::now()) ? Domain::ErrorCodes::DeadlineExceeded :
                Domain::ErrorCodes::InvalidRequest, failure.what()));
    } catch (...) {
        return Domain::Result<PreparedLocalModel>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Model preparation failed safely."));
    }
}
} // namespace ForgeConductor::Infrastructure::Windows
