#include "SessionHostCompositionRoot.h"

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"
#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"
#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Hosts::SessionHost {
namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace NativeSessionHost = ForgeConductor::SessionHost;

using Json = nlohmann::json;

constexpr std::chrono::seconds StartupDeadline{15};
constexpr std::chrono::seconds CommandDeadline{30};
constexpr std::size_t MaximumExecutablePathCharacters = 32U * 1024U;
constexpr std::size_t MaximumReportedErrorBytes = 4096U;
constexpr std::string_view LedgerFileName = "native-session-ledger.json";
constexpr std::string_view CompositionClientId =
    "forge-session-host-composition";

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void requireSuccess(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
[[nodiscard]] Domain::Result<T> propagate(Domain::Result<void> source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock,
    const std::string_view action) noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            std::string{action} + " was cancelled."));
    }
    if (context.isExpired(clock.monotonicNow())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            std::string{action} + " exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToWide(
    const std::string_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Windows path cannot be converted from UTF-8."));
        }
        const int inputLength = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0);
        if (required <= 0) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The Windows path is not valid UTF-8."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        const int written = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
            converted.data(), required);
        if (written != required) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Windows path UTF-8 conversion was incomplete."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Windows path UTF-8 conversion failed safely."));
    }
}

[[nodiscard]] Domain::Result<std::string> strictWideToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The executable path cannot be converted to UTF-8."));
        }
        const int inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The executable path is not valid UTF-16."));
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        const int written = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            converted.data(), required, nullptr, nullptr);
        if (written != required) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The executable path UTF-16 conversion was incomplete."));
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The executable path UTF-16 conversion failed safely."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText>
currentExecutablePath() noexcept
{
    try {
        std::array<wchar_t, MaximumExecutablePathCharacters> buffer{};
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U ||
            static_cast<std::size_t>(length) >= buffer.size()) {
            return Domain::Result<Domain::PathText>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "GetModuleFileNameW could not resolve the session-host executable path."));
        }
        auto utf8 = strictWideToUtf8(
            std::wstring_view{buffer.data(), static_cast<std::size_t>(length)});
        if (!utf8) {
            return Domain::Result<Domain::PathText>::failure(
                std::move(utf8).error());
        }
        return Domain::PathText::create(utf8.value());
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The session-host executable path could not be resolved safely."));
    }
}

[[nodiscard]] Domain::Result<Domain::PathText> childPath(
    const Domain::PathText& root,
    const std::string_view leaf) noexcept
{
    try {
        if (leaf.empty() || leaf.find_first_of("\\/:") !=
                                std::string_view::npos) {
            return Domain::Result<Domain::PathText>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The session-host app-data filename is invalid."));
        }
        std::string path = root.value();
        if (!path.ends_with('\\') && !path.ends_with('/')) {
            path.push_back('\\');
        }
        path.append(leaf);
        return Domain::PathText::create(path);
    } catch (...) {
        return Domain::Result<Domain::PathText>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The session-host app-data path could not be constructed safely."));
    }
}

[[nodiscard]] Domain::Result<void> ensureDataRoot(
    const Domain::PathText& root,
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    auto valid = validateContext(
        context, clock, "Session-host app-data initialization");
    if (!valid) {
        return valid;
    }
    auto wide = strictUtf8ToWide(root.value());
    if (!wide) {
        return Domain::Result<void>::failure(std::move(wide).error());
    }
    if (::CreateDirectoryW(wide.value().c_str(), nullptr) == FALSE) {
        const DWORD nativeError = ::GetLastError();
        if (nativeError != ERROR_ALREADY_EXISTS) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The session-host app-data directory could not be created (Win32 " +
                    std::to_string(nativeError) + ")."));
        }
    }
    const DWORD attributes = ::GetFileAttributesW(wide.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The session-host app-data directory could not be inspected (Win32 " +
                std::to_string(::GetLastError()) + ")."));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PathOutsideAuthority,
            "The session-host app-data root is not a regular directory."));
    }
    return validateContext(
        context, clock, "Session-host app-data initialization");
}

class ExactPathCapabilityIssuer final
    : public Contracts::IWorkspaceAuthority {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<ExactPathCapabilityIssuer>> create(
        const Domain::PathText& root,
        const Domain::PathText& primary,
        const Domain::PathText& backup,
        Contracts::IUuidGenerator& uuidGenerator,
        Contracts::IClock& clock,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(
                context, clock, "Session-host capability issuance");
            if (!valid) {
                return propagate<std::unique_ptr<ExactPathCapabilityIssuer>>(
                    std::move(valid));
            }
            auto authorityUuid = uuidGenerator.next();
            if (!authorityUuid) {
                return Domain::Result<
                    std::unique_ptr<ExactPathCapabilityIssuer>>::failure(
                    std::move(authorityUuid).error());
            }
            auto scopeUuid = uuidGenerator.next();
            if (!scopeUuid) {
                return Domain::Result<
                    std::unique_ptr<ExactPathCapabilityIssuer>>::failure(
                    std::move(scopeUuid).error());
            }
            auto clientId = Domain::ClientId::parse(CompositionClientId);
            if (!clientId) {
                return Domain::Result<
                    std::unique_ptr<ExactPathCapabilityIssuer>>::failure(
                    std::move(clientId).error());
            }
            auto authority = issueAuthority(
                Domain::AuthorityId{std::move(authorityUuid).value()},
                Domain::ProjectId{std::move(scopeUuid).value()},
                std::move(clientId).value(),
                std::vector<Domain::PathText>{root},
                Domain::FileAccess::Read,
                std::vector<Domain::FileAccess>{
                    Domain::FileAccess::Read,
                    Domain::FileAccess::Write,
                    Domain::FileAccess::Create},
                {}, false, 1U);
            if (!authority) {
                return Domain::Result<
                    std::unique_ptr<ExactPathCapabilityIssuer>>::failure(
                    std::move(authority).error());
            }
            return Domain::Result<
                std::unique_ptr<ExactPathCapabilityIssuer>>::success(
                std::unique_ptr<ExactPathCapabilityIssuer>{
                    new ExactPathCapabilityIssuer{
                        root, primary, backup, std::move(authority).value(),
                        clock}});
        } catch (...) {
            return Domain::Result<
                std::unique_ptr<ExactPathCapabilityIssuer>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The session-host exact-path capabilities could not be issued safely."));
        }
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> issue(
        const Domain::PathText& target,
        const Domain::FileAccess access,
        const Domain::OperationContext& context) noexcept
    {
        return authorize(
            authority_,
            Domain::PathAuthorizationRequest{
                target, root_, access, true},
            context);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        auto valid = validateContext(
            context, clock_, "Session-host authority resolution");
        if (!valid) {
            return propagate<Contracts::WorkspaceAuthority>(
                std::move(valid));
        }
        if (projectId != authority_.projectId()) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Unauthorized,
                    "The requested project is outside the session-host ledger authority."));
        }
        try {
            return Domain::Result<Contracts::WorkspaceAuthority>::success(
                authority_);
        } catch (...) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The session-host ledger authority could not be copied safely."));
        }
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The composition-owned exact-file authority cannot be narrowed."));
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        auto valid = validateContext(
            context, clock_, "Session-host path authorization");
        if (!valid) {
            return propagate<Contracts::AuthorizedPath>(std::move(valid));
        }
        try {
            const bool knownAuthority =
                authority.authorityId() == authority_.authorityId() &&
                authority.projectId() == authority_.projectId() &&
                authority.callerId() == authority_.callerId() &&
                authority.generation() == authority_.generation();
            const bool exactBase =
                request.basePath && *request.basePath == root_;
            const bool primaryAccess = request.requestedPath == primary_ &&
                (request.access == Domain::FileAccess::Read ||
                 request.access == Domain::FileAccess::Write ||
                 request.access == Domain::FileAccess::Create);
            const bool backupAccess = request.requestedPath == backup_ &&
                request.access == Domain::FileAccess::Read;
            if (!knownAuthority || !exactBase ||
                (!primaryAccess && !backupAccess)) {
                return Domain::Result<Contracts::AuthorizedPath>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The requested path is outside the exact session-host ledger capability."));
            }
            return issueAuthorizedPath(
                authority, request.requestedPath, root_, request.access);
        } catch (...) {
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The session-host path authorization failed safely."));
        }
    }

private:
    ExactPathCapabilityIssuer(
        Domain::PathText root,
        Domain::PathText primary,
        Domain::PathText backup,
        Contracts::WorkspaceAuthority authority,
        Contracts::IClock& clock)
        : root_{std::move(root)}, primary_{std::move(primary)},
          backup_{std::move(backup)}, authority_{std::move(authority)},
          clock_{clock}
    {
    }

    const Domain::PathText root_;
    const Domain::PathText primary_;
    const Domain::PathText backup_;
    const Contracts::WorkspaceAuthority authority_;
    Contracts::IClock& clock_;
};

[[nodiscard]] Json capabilitiesJson(
    const Domain::HostCapabilities& capabilities)
{
    return Json{
        {"bootstrap", capabilities.bootstrap},
        {"cancellation", capabilities.cancellation},
        {"create", capabilities.create},
        {"idempotency", capabilities.idempotency},
        {"query_by_idempotency_key", capabilities.queryByIdempotencyKey},
        {"recovery", capabilities.recovery},
        {"resume", capabilities.resume},
        {"usage_reporting", capabilities.usageReporting}};
}

[[nodiscard]] std::string boundedErrorText(std::string message)
{
    if (message.size() > MaximumReportedErrorBytes) {
        return "The detailed error exceeded the session-host reporting bound.";
    }
    return message;
}

void writeError(const Domain::Error& error) noexcept
{
    try {
        std::cerr << Json{
            {"error", Json{
                {"code", error.code},
                {"message", boundedErrorText(error.message)},
                {"retryable", error.retryable}}}}
                          .dump()
                  << '\n';
    } catch (...) {
        std::cerr << "Session-host command failed safely.\n";
    }
}

void writeJson(const Json& document)
{
    std::cout << document.dump() << '\n';
}

void printHelp()
{
    std::cout
        << "Forge Conductor native session host\n"
        << "Usage: ForgeConductor.SessionHost.exe [command]\n\n"
        << "Commands:\n"
        << "  --manifest        Print the bounded native-adapter manifest as JSON.\n"
        << "  --health          Validate the durable ledger and print health as JSON.\n"
        << "  --recover         Run bounded recovery without requesting orphan cancellation.\n"
        << "  --cancel-orphans  Run bounded recovery with cancelOrphans=true.\n"
        << "  --self-test       Validate x64 composition, capabilities, and ledger access.\n"
        << "  --help, -h        Print this help and exit.\n\n"
        << "This P12 executable is a bounded one-shot host surface. "
        << "Manager IPC is intentionally deferred to P16.\n";
}

} // namespace

class SessionHostCompositionRoot::Impl final {
public:
    Impl()
        : clock_{std::make_shared<InfrastructureWindows::SystemClock>()},
          hasher_{std::make_shared<
              InfrastructureWindows::BCryptSha256Hasher>()},
          uuidGenerator_{
              std::make_unique<InfrastructureWindows::WindowsUuidGenerator>()},
          codec_{std::make_unique<
              InfrastructureWindows::WindowsContinuityDocumentCodec>(
              hasher_, clock_)},
          applicationPaths_{std::make_unique<
              InfrastructureWindows::WindowsApplicationPaths>()},
          atomicFileStore_{std::make_unique<
              InfrastructureWindows::WindowsAtomicFileStore>()}
    {
        const auto startupContext = take(makeContext(
            StartupDeadline, "session-host-startup"));
        const auto requestedDataRoot = take(
            applicationPaths_->dataRoot(startupContext));
        requireSuccess(ensureDataRoot(
            requestedDataRoot, startupContext, *clock_));
        const auto dataRoot = requestedDataRoot;
        const auto primary = take(childPath(dataRoot, LedgerFileName));
        const auto backup = take(Domain::PathText::create(
            primary.value() + ".bak"));
        capabilityIssuer_ = take(ExactPathCapabilityIssuer::create(
            dataRoot, primary, backup, *uuidGenerator_, *clock_,
            startupContext));
        auto primaryRead = take(capabilityIssuer_->issue(
            primary, Domain::FileAccess::Read, startupContext));
        auto primaryWrite = take(capabilityIssuer_->issue(
            primary, Domain::FileAccess::Write, startupContext));
        auto primaryCreate = take(capabilityIssuer_->issue(
            primary, Domain::FileAccess::Create, startupContext));
        auto backupRead = take(capabilityIssuer_->issue(
            backup, Domain::FileAccess::Read, startupContext));
        ledger_ = std::make_unique<
            InfrastructureWindows::WindowsNativeSessionLedger>(
            *atomicFileStore_, *hasher_, std::move(primaryRead),
            std::move(primaryWrite), std::move(primaryCreate),
            std::move(backupRead));
        continuationQueue_ = std::make_unique<
            NativeSessionHost::BoundedLogicalContinuationQueue>();
        transport_ = std::make_unique<
            NativeSessionHost::LocalLogicalSessionTransport>(
            hasher_, *codec_, *continuationQueue_);
        auto adapterId = take(Domain::AdapterId::parse(
            NativeSessionHost::ForgeNativeSessionHostAdapter::
                AdapterIdentifier));
        adapter_ = std::make_unique<
            NativeSessionHost::ForgeNativeSessionHostAdapter>(
            std::move(adapterId), *ledger_, *transport_, *codec_,
            *uuidGenerator_, *clock_);
        executablePath_ = take(currentExecutablePath());
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] int run(
        const std::span<const std::wstring_view> arguments) noexcept
    {
        try {
            if (arguments.empty()) {
                printHelp();
                return EXIT_SUCCESS;
            }
            if (arguments.size() != 1U) {
                std::cerr
                    << "Exactly one session-host command is supported per invocation.\n";
                printHelp();
                return 2;
            }
            const auto command = arguments.front();
            if (command == L"--help" || command == L"-h" ||
                command == L"help") {
                printHelp();
                return EXIT_SUCCESS;
            }
            if (command == L"--manifest") {
                return manifest();
            }
            if (command == L"--health") {
                return health();
            }
            if (command == L"--recover") {
                return recover(false);
            }
            if (command == L"--cancel-orphans") {
                return recover(true);
            }
            if (command == L"--self-test") {
                return selfTest();
            }
            std::cerr << "Unknown session-host command.\n";
            printHelp();
            return 2;
        } catch (const std::exception& error) {
            writeError(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                boundedErrorText(error.what())));
            return EXIT_FAILURE;
        } catch (...) {
            writeError(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The session-host command failed safely."));
            return EXIT_FAILURE;
        }
    }

    void shutdown() noexcept
    {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        if (adapter_) {
            adapter_->shutdown();
        }
        adapter_.reset();
        transport_.reset();
        if (continuationQueue_) {
            continuationQueue_->shutdown();
        }
        continuationQueue_.reset();
        ledger_.reset();
        codec_.reset();
        atomicFileStore_.reset();
        capabilityIssuer_.reset();
        applicationPaths_.reset();
        uuidGenerator_.reset();
        hasher_.reset();
        clock_.reset();
    }

private:
    [[nodiscard]] Domain::Result<Domain::OperationContext> makeContext(
        const std::chrono::seconds timeout,
        const std::string_view correlationId) noexcept
    {
        try {
            if (timeout <= std::chrono::seconds::zero() ||
                timeout > std::chrono::minutes{2}) {
                return Domain::Result<Domain::OperationContext>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The session-host operation deadline is outside its bound."));
            }
            auto operationUuid = uuidGenerator_->next();
            if (!operationUuid) {
                return Domain::Result<Domain::OperationContext>::failure(
                    std::move(operationUuid).error());
            }
            auto parsedCorrelation =
                Domain::CorrelationId::parse(correlationId);
            if (!parsedCorrelation) {
                return Domain::Result<Domain::OperationContext>::failure(
                    std::move(parsedCorrelation).error());
            }
            return Domain::Result<Domain::OperationContext>::success(
                Domain::OperationContext{
                    Domain::OperationId{
                        std::move(operationUuid).value()},
                    clock_->monotonicNow() + timeout,
                    std::stop_token{},
                    std::move(parsedCorrelation).value()});
        } catch (...) {
            return Domain::Result<Domain::OperationContext>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The session-host operation context could not be created safely."));
        }
    }

    [[nodiscard]] int manifest()
    {
        auto result = NativeSessionHost::nativeSessionHostPluginManifest(
            executablePath_.value());
        if (!result) {
            writeError(result.error());
            return EXIT_FAILURE;
        }
        const auto& value = result.value();
        writeJson(Json{
            {"adapter_id", value.adapterId.value()},
            {"adapter_version", value.adapterVersion},
            {"capabilities", capabilitiesJson(value.capabilities)},
            {"executable", value.executable.value()},
            {"manager_ipc_available", false},
            {"protocol_version", value.protocolVersion}});
        return EXIT_SUCCESS;
    }

    [[nodiscard]] int health()
    {
        const auto context = take(makeContext(
            CommandDeadline, "session-host-health"));
        auto result = adapter_->health(context);
        if (!result) {
            writeError(result.error());
            return EXIT_FAILURE;
        }
        writeJson(Json{
            {"healthy", result.value().healthy},
            {"ledger_records", result.value().records},
            {"maximum_ledger_records", result.value().maximumRecords},
            {"maximum_response_bytes", result.value().maximumResponseBytes},
            {"pending_continuations", continuationQueue_->pendingCount()}});
        return result.value().healthy ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    [[nodiscard]] int recover(const bool cancelOrphans)
    {
        const auto context = take(makeContext(
            CommandDeadline,
            cancelOrphans ? "session-host-cancel-orphans"
                          : "session-host-recover"));
        auto result = adapter_->recover(
            Domain::HostRecoveryRequest{
                std::nullopt, std::nullopt, cancelOrphans},
            context);
        if (!result) {
            writeError(result.error());
            return EXIT_FAILURE;
        }
        const auto& report = result.value();
        const bool completedWithoutFailures = report.failed == 0U;
        writeJson(Json{
            {"cancel_orphans_requested", cancelOrphans},
            {"cancelled_records", report.cancelled},
            {"completed_without_failures", completedWithoutFailures},
            {"failed_records", report.failed},
            {"inspected_records", report.inspected},
            {"pending_continuations", continuationQueue_->pendingCount()},
            {"recovered_records", report.recovered}});
        return completedWithoutFailures ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    [[nodiscard]] int selfTest()
    {
        if constexpr (sizeof(void*) != 8U) {
            writeError(Domain::makeError(
                Domain::ErrorCodes::UnsupportedVersion,
                "The native session host requires a 64-bit process."));
            return EXIT_FAILURE;
        }
        const auto context = take(makeContext(
            CommandDeadline, "session-host-self-test"));
        auto capabilities = adapter_->capabilities(context);
        if (!capabilities) {
            writeError(capabilities.error());
            return EXIT_FAILURE;
        }
        auto hostHealth = adapter_->health(context);
        if (!hostHealth) {
            writeError(hostHealth.error());
            return EXIT_FAILURE;
        }
        const bool identityMatches =
            adapter_->identifier().value() ==
                NativeSessionHost::ForgeNativeSessionHostAdapter::
                    AdapterIdentifier &&
            adapter_->version() ==
                NativeSessionHost::ForgeNativeSessionHostAdapter::
                    AdapterVersion;
        const bool passed = identityMatches && hostHealth.value().healthy;
        writeJson(Json{
            {"adapter_identity_matches", identityMatches},
            {"capabilities", capabilitiesJson(capabilities.value())},
            {"ledger_records", hostHealth.value().records},
            {"manager_ipc_available", false},
            {"passed", passed},
            {"pending_continuations", continuationQueue_->pendingCount()},
            {"process_bits", sizeof(void*) * 8U}});
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    std::shared_ptr<InfrastructureWindows::SystemClock> clock_;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher_;
    std::unique_ptr<InfrastructureWindows::WindowsUuidGenerator>
        uuidGenerator_;
    std::unique_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec>
        codec_;
    std::unique_ptr<InfrastructureWindows::WindowsApplicationPaths>
        applicationPaths_;
    std::unique_ptr<InfrastructureWindows::WindowsAtomicFileStore>
        atomicFileStore_;
    std::unique_ptr<ExactPathCapabilityIssuer> capabilityIssuer_;
    std::unique_ptr<InfrastructureWindows::WindowsNativeSessionLedger>
        ledger_;
    std::unique_ptr<NativeSessionHost::BoundedLogicalContinuationQueue>
        continuationQueue_;
    std::unique_ptr<NativeSessionHost::LocalLogicalSessionTransport>
        transport_;
    std::unique_ptr<NativeSessionHost::ForgeNativeSessionHostAdapter>
        adapter_;
    std::optional<Domain::PathText> executablePath_;
    bool shutdown_{};
};

SessionHostCompositionRoot::SessionHostCompositionRoot()
    : implementation_{std::make_unique<Impl>()}
{
}

SessionHostCompositionRoot::~SessionHostCompositionRoot() noexcept = default;

int SessionHostCompositionRoot::run(
    const std::span<const std::wstring_view> arguments) noexcept
{
    return implementation_->run(arguments);
}

} // namespace ForgeConductor::Hosts::SessionHost
