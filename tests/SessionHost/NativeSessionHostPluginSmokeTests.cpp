#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"
#include "ForgeConductor/SessionHost/PluginAbi.h"
#include "Fakes/InMemoryNativeSessionLedger.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Infrastructure = ForgeConductor::Infrastructure::Windows;
namespace SessionHost = ForgeConductor::SessionHost;
namespace Fakes = ForgeConductor::Tests::Fakes;

std::size_t assertions{};

void require(const bool condition, const std::string_view expression)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

class UniqueModule final {
public:
    explicit UniqueModule(const std::filesystem::path& path)
        : value_{::LoadLibraryExW(
              path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)}
    {
        if (value_ == nullptr) {
            throw std::runtime_error{
                "LoadLibraryExW failed with " +
                std::to_string(::GetLastError())};
        }
    }

    ~UniqueModule() noexcept
    {
        if (value_ != nullptr) {
            ::FreeLibrary(value_);
        }
    }

    UniqueModule(const UniqueModule&) = delete;
    UniqueModule& operator=(const UniqueModule&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return value_; }

private:
    HMODULE value_{};
};

using GetPluginApi = bool (__stdcall*)(
    SessionHost::NativeSessionHostPluginApiV1*) noexcept;

void run(const std::filesystem::path& pluginPath)
{
    UniqueModule module{pluginPath};
    const auto raw = ::GetProcAddress(
        module.get(), "ForgeGetNativeSessionHostPluginV1");
    REQUIRE(raw != nullptr);
    const auto getApi = reinterpret_cast<GetPluginApi>(raw);

    SessionHost::NativeSessionHostPluginApiV1 api{};
    REQUIRE(getApi(&api));
    REQUIRE(api.manifest.structureSize ==
            sizeof(SessionHost::NativeSessionHostPluginManifestV1));
    REQUIRE(api.manifest.abiVersion ==
            SessionHost::NativeSessionHostPluginAbiVersion);
    REQUIRE(std::string_view{api.manifest.adapterIdentifier} ==
            "forge.native-session-host");
    REQUIRE(std::string_view{api.manifest.adapterVersion} == "1.0.0");
    REQUIRE(api.manifest.protocolVersion == 1U);
    REQUIRE(api.manifest.capabilityBits == 0xffU);
    REQUIRE(api.create != nullptr);
    REQUIRE(api.destroy != nullptr);

    Fakes::InMemoryNativeSessionLedger ledger;
    auto hasher = std::make_shared<Infrastructure::BCryptSha256Hasher>();
    auto clock = std::make_shared<Infrastructure::SystemClock>();
    Infrastructure::WindowsUuidGenerator uuidGenerator;
    Infrastructure::WindowsContinuityDocumentCodec codec{hasher, clock};
    SessionHost::LocalLogicalSessionTransport transport{hasher};
    SessionHost::NativeSessionHostPluginDependenciesV1 dependencies{};
    dependencies.ledger = &ledger;
    dependencies.transport = &transport;
    dependencies.codec = &codec;
    dependencies.uuidGenerator = &uuidGenerator;
    dependencies.clock = clock.get();

    Contracts::ISessionHostAdapter* adapter = api.create(&dependencies);
    REQUIRE(adapter != nullptr);
    const Domain::OperationContext context{
        parse<Domain::OperationId>(
            "11111111-1111-4111-8111-111111111111"),
        clock->monotonicNow() + std::chrono::seconds{30},
        {},
        parse<Domain::CorrelationId>("native-plugin-smoke")};
    const auto capabilities = take(adapter->capabilities(context));
    REQUIRE(capabilities.create);
    REQUIRE(capabilities.bootstrap);
    REQUIRE(capabilities.resume);
    REQUIRE(capabilities.idempotency);
    REQUIRE(capabilities.queryByIdempotencyKey);
    REQUIRE(capabilities.recovery);
    REQUIRE(capabilities.cancellation);

    const Domain::SessionCreationRequest request{
        parse<Domain::ContinuityOperationId>(
            "22222222-2222-4222-8222-222222222222"),
        parse<Domain::ProjectId>(
            "33333333-3333-4333-8333-333333333333"),
        parse<Domain::SessionId>(
            "44444444-4444-4444-8444-444444444444"),
        take(Domain::IdempotencyKey::create("native-plugin-smoke-key"))};
    const auto created = take(adapter->createSession(request, context));
    REQUIRE(created.projectId == request.projectId);
    REQUIRE(created.operationId == request.operationId);
    REQUIRE(created.predecessorSessionId == request.predecessorSessionId);
    REQUIRE(created.idempotencyKey == request.idempotencyKey);
    REQUIRE(created.providerSessionId.has_value());

    const auto replay = take(adapter->queryByIdempotencyKey(
        request.projectId, request.idempotencyKey, context));
    REQUIRE(replay.has_value());
    REQUIRE(replay->id == created.id);
    REQUIRE(replay->providerSessionId == created.providerSessionId);

    api.destroy(adapter);
    REQUIRE(ledger.isShutdown());

    SessionHost::NativeSessionHostPluginApiV1 wrongSize{};
    wrongSize.structureSize = 0U;
    REQUIRE(!getApi(&wrongSize));
}

} // namespace

int wmain(const int argc, wchar_t** argv)
{
    try {
        if (argc != 2) {
            std::cerr << "Expected the native session-host plugin path.\n";
            return EXIT_FAILURE;
        }
        run(std::filesystem::path{argv[1]});
        std::cout << "Native session-host plugin smoke tests passed: "
                  << assertions << " assertions.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
