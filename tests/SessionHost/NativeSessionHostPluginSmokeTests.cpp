#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"
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

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::ContinuityHandoff handoffFor(
    const Domain::SessionCreationRequest& request,
    const Domain::HostSession& successor,
    Contracts::IContinuityDocumentCodec& codec,
    const Contracts::IClock& clock,
    const Domain::OperationContext& context)
{
    Domain::ContinuityHandoff handoff{
        parse<Domain::ContinuityHandoffId>(
            "55555555-5555-4555-8555-555555555555"),
        request.operationId,
        clock.utcNow(),
        Domain::ContinuityProject{
            request.projectId,
            "Native plugin smoke project",
            take(Domain::PathText::create("D:\\native-plugin-smoke")),
            "main",
            "0123456789abcdef",
            {}},
        Domain::ContinuitySession{
            request.predecessorSessionId,
            std::nullopt,
            std::optional<std::string>{"predecessor-model"},
            std::optional<std::string>{"native-plugin-smoke"}},
        Domain::ContinuitySession{
            successor.id,
            successor.providerSessionId,
            successor.model,
            std::optional<std::string>{"native-plugin-smoke"}},
        "Resume the native plugin successor",
        {"Preserve exact plugin lifecycle bindings"},
        Domain::ContinuityCurrentWork{
            "P12",
            "native-plugin-smoke",
            "Complete the native plugin lifecycle",
            {}},
        {},
        {{std::optional<std::string>{"plugin-lifecycle"},
          "Bootstrap through the loaded DLL",
          std::optional<std::string>{"active"}}},
        {{"Use the versioned native ABI", std::nullopt}},
        Domain::ContinuityValidation{{"G11"}, {"G12"}, {}},
        {},
        {},
        {{1U,
          "Continue the loaded native session",
          "forge continue --plugin-smoke",
          "The native plugin continuation is accepted"}},
        Domain::ContinuityHostState{
            parse<Domain::AdapterId>("forge.native-session-host"),
            Domain::ContinuityState::SuccessorCreated,
            "native-plugin-smoke",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(std::string(64U, 'a')),
        true};
    return take(codec.encode(handoff, context)).handoff;
}

class UniqueModule final {
public:
    explicit UniqueModule(const std::filesystem::path& path)
        : value_{::LoadLibraryExW(
              path.c_str(),
              nullptr,
              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                  LOAD_LIBRARY_SEARCH_SYSTEM32)}
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
    SessionHost::BoundedLogicalContinuationQueue continuationQueue{4U};
    SessionHost::LocalLogicalSessionTransport transport{
        hasher, codec, continuationQueue};
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

    const auto handoff = handoffFor(
        request, created, codec, *clock, context);
    take(adapter->bootstrap(created, handoff, context));
    const auto acknowledgement = take(adapter->awaitAcknowledgement(
        created,
        handoff.handoffId,
        handoff.contentSha256,
        context));
    REQUIRE(acknowledgement.handoffId == handoff.handoffId);
    REQUIRE(acknowledgement.successorSessionId == created.id);
    REQUIRE(acknowledgement.canonicalHandoffSha256 ==
            handoff.contentSha256);
    REQUIRE(take(adapter->query(created.id, context)) ==
            Domain::HostSessionStatus::Ready);
    REQUIRE(continuationQueue.pendingCount() == 1U);
    const auto continuation = take(continuationQueue.takeNext(context));
    REQUIRE(continuation.has_value());
    REQUIRE(continuation->handoffId == handoff.handoffId);
    REQUIRE(continuation->providerSessionId == created.providerSessionId);
    REQUIRE(continuation->command == "forge continue --plugin-smoke");
    REQUIRE(continuationQueue.pendingCount() == 0U);

    const auto recovery = take(adapter->recover(
        Domain::HostRecoveryRequest{
            request.projectId, request.operationId, false},
        context));
    REQUIRE(recovery.inspected == 1U);
    REQUIRE(recovery.recovered == 1U);
    REQUIRE(recovery.failed == 0U);

    adapter->cancel(context.operationId);
    REQUIRE(take(adapter->query(created.id, Domain::OperationContext{
                parse<Domain::OperationId>(
                    "66666666-6666-4666-8666-666666666666"),
                clock->monotonicNow() + std::chrono::seconds{30},
                {},
                parse<Domain::CorrelationId>(
                    "native-plugin-smoke-cancelled-query")})) ==
            Domain::HostSessionStatus::Cancelled);

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
