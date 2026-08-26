#include "TestSupport.h"

#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/DeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "Infrastructure/Windows/Detail/SecureBuffer.h"
#include "Infrastructure/Windows/Detail/UniqueBCryptHandle.h"
#include "Infrastructure/Windows/Detail/UniqueCoTaskMemAllocation.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UniqueLocalAllocation.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::BCryptSha256Hasher;
using Infrastructure::Windows::DeadlineScheduler;
using Infrastructure::Windows::SecretRedactor;
using Infrastructure::Windows::SystemClock;
using Infrastructure::Windows::WindowsApplicationPaths;
using Infrastructure::Windows::WindowsApplicationPathsOptions;
using Infrastructure::Windows::WindowsUuidGenerator;
namespace Detail = Infrastructure::Windows::Detail;

static_assert(std::is_final_v<WindowsApplicationPaths>);
static_assert(std::is_final_v<SystemClock>);
static_assert(std::is_final_v<WindowsUuidGenerator>);
static_assert(std::is_final_v<BCryptSha256Hasher>);
static_assert(std::is_final_v<SecretRedactor>);
static_assert(std::is_final_v<DeadlineScheduler>);
static_assert(!std::is_copy_constructible_v<Detail::UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<Detail::UniqueHandle>);
static_assert(!std::is_copy_constructible_v<Detail::UniqueBCryptAlgorithmHandle>);
static_assert(std::is_nothrow_move_constructible_v<Detail::UniqueBCryptAlgorithmHandle>);
static_assert(!std::is_copy_constructible_v<Detail::SecureBuffer>);
static_assert(std::is_nothrow_move_constructible_v<Detail::SecureBuffer>);

class FixedClock final : public Contracts::IClock {
public:
    explicit FixedClock(const Domain::MonotonicTimePoint now) noexcept : now_{now} {}

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override { return {}; }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override { return now_; }

private:
    Domain::MonotonicTimePoint now_;
};

class ScopedTestDirectory final {
public:
    ScopedTestDirectory()
    {
        std::wstring buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        require(length != 0 && length < buffer.size(), "GetTempPathW failed");
        buffer.resize(length);
        path_ = std::filesystem::path{buffer} /
                (L"ForgeConductor-P06-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(::GetTickCount64()));
        require(std::filesystem::create_directories(path_),
                "could not create a unique P06 test directory");
    }

    ~ScopedTestDirectory() noexcept
    {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedEnvironmentValue final {
public:
    ScopedEnvironmentValue(const wchar_t* const name, const std::wstring& value) : name_{name}
    {
        std::wstring existing(32U * 1024U, L'\0');
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetEnvironmentVariableW(name_.c_str(), existing.data(),
                                                       static_cast<DWORD>(existing.size()));
        if (length != 0 && length < existing.size()) {
            existing.resize(length);
            previous_ = std::move(existing);
        } else {
            require(length == 0 && (::GetLastError() == ERROR_ENVVAR_NOT_FOUND ||
                                    ::GetLastError() == ERROR_SUCCESS),
                    "could not capture the prior environment value");
        }
        require(::SetEnvironmentVariableW(name_.c_str(), value.c_str()) != FALSE,
                "could not set the test environment override");
    }

    ~ScopedEnvironmentValue() noexcept
    {
        static_cast<void>(
            ::SetEnvironmentVariableW(name_.c_str(), previous_ ? previous_->c_str() : nullptr));
    }

    ScopedEnvironmentValue(const ScopedEnvironmentValue&) = delete;
    ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

[[nodiscard]] Domain::OperationContext activeContext()
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("11111111-1111-4111-8111-111111111111"),
        std::chrono::steady_clock::now() + std::chrono::minutes{5},
        {},
        parse<Domain::CorrelationId>("p06-foundation")};
}

[[nodiscard]] Domain::PathText pathText(const std::filesystem::path& value)
{
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(value.native()))));
}

void testClocksAndDeadlineGate()
{
    SystemClock systemClock;
    const auto monotonicBefore = systemClock.monotonicNow();
    const auto utc = systemClock.utcNow();
    const auto monotonicAfter = systemClock.monotonicNow();
    require(monotonicAfter >= monotonicBefore, "the system monotonic clock regressed");
    require(utc.time_since_epoch().count() > 0, "the system UTC clock returned its epoch");

    const Domain::MonotonicTimePoint now = Domain::MonotonicTimePoint{} + std::chrono::seconds{50};
    FixedClock fixed{now};
    DeadlineScheduler scheduler{fixed};
    const auto operationId = parse<Domain::OperationId>("22222222-2222-4222-8222-222222222222");
    const auto correlationId = parse<Domain::CorrelationId>("deadline-test");

    const Domain::OperationContext active{
        operationId, now + std::chrono::seconds{1}, {}, correlationId};
    require(scheduler.waitUntil(active).hasValue(),
            "the nonblocking deadline gate rejected an active context");

    const Domain::OperationContext expired{operationId, now, {}, correlationId};
    requireError(scheduler.waitUntil(expired), Domain::ErrorCodes::DeadlineExceeded,
                 "the deadline gate did not reject an expired context");

    std::stop_source cancellation;
    cancellation.request_stop();
    const Domain::OperationContext cancelledAndExpired{operationId, now, cancellation.get_token(),
                                                       correlationId};
    requireError(scheduler.waitUntil(cancelledAndExpired), Domain::ErrorCodes::Cancelled,
                 "cancellation did not take precedence over deadline expiry");

    scheduler.shutdown();
    requireError(scheduler.waitUntil(active), Domain::ErrorCodes::Cancelled,
                 "the deadline gate accepted work after shutdown");
}

void testUuidAndSha256()
{
    WindowsUuidGenerator generator;
    std::unordered_set<std::string> generated;
    for (std::size_t index = 0; index < 128U; ++index) {
        const Domain::Uuid value = take(generator.next());
        require(value.value().size() == 36U, "a generated UUID had the wrong size");
        require(value.value()[14] == '4', "a generated UUID was not version 4");
        require(std::string_view{"89ab"}.find(value.value()[19]) != std::string_view::npos,
                "a generated UUID had the wrong RFC 4122 variant");
        require(generated.insert(value.value()).second,
                "the Windows RNG generated a duplicate in a bounded sample");
    }

    BCryptSha256Hasher hasher;
    const std::span<const std::byte> empty;
    require(take(hasher.sha256(empty)).value() ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "BCrypt SHA-256 did not match the empty-string vector");

    const std::string abc{"abc"};
    require(take(hasher.sha256(std::as_bytes(std::span{abc}))).value() ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "BCrypt SHA-256 did not match the abc vector");
}

void testSecretRedaction()
{
    SecretRedactor redactor;
    const std::string input = "Authorization: Bearer bearer-token_123; "
                              "sk-1234567890abcdef; ghp_12345678901234567890; "
                              "AKIA1234567890ABCDEF; password = hunter2; keep=visible";
    const std::string output = take(redactor.redact(input));
    require(output.find("bearer-token_123") == std::string::npos,
            "authorization credentials survived redaction");
    require(output.find("sk-1234567890abcdef") == std::string::npos,
            "an API token survived redaction");
    require(output.find("ghp_12345678901234567890") == std::string::npos,
            "a GitHub token survived redaction");
    require(output.find("AKIA1234567890ABCDEF") == std::string::npos,
            "an AWS access key survived redaction");
    require(output.find("hunter2") == std::string::npos,
            "a password assignment survived redaction");
    require(output.find("keep=visible") != std::string::npos,
            "redaction changed an unrelated field");

    requireError(
        redactor.redact("-----BEGIN RSA PRIVATE KEY-----\nsecret\n-----END RSA PRIVATE KEY-----"),
        Domain::ErrorCodes::RedactionRejected, "private-key material was not rejected");

    const std::string malformedUtf8{"\xC3\x28", 2};
    requireError(redactor.redact(malformedUtf8), Domain::ErrorCodes::InvalidRequest,
                 "malformed UTF-8 crossed the redaction boundary");

    const std::string atLimit(SecretRedactor::MaximumInputBytes, 'x');
    require(take(redactor.redact(atLimit)) == atLimit,
            "the exact redaction byte limit was rejected");
    const std::string overLimit(SecretRedactor::MaximumInputBytes + 1U, 'x');
    requireError(redactor.redact(overLimit), Domain::ErrorCodes::PayloadTooLarge,
                 "redaction accepted its byte limit plus one");
}

void testUtfAndHandleOwners()
{
    const std::string utf8 = "Forge \xCE\xBB \xF0\x9F\x9B\xA0";
    const std::wstring utf16 = take(Detail::strictUtf8ToUtf16(utf8));
    require(take(Detail::strictUtf16ToUtf8(utf16)) == utf8,
            "strict UTF conversion did not round-trip valid Unicode");
    requireError(Detail::strictUtf8ToUtf16(std::string{"\xED\xA0\x80", 3}),
                 Domain::ErrorCodes::InvalidRequest,
                 "strict UTF conversion accepted a surrogate encoded in UTF-8");

    HANDLE rawEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(rawEvent != nullptr, "CreateEventW failed for the RAII owner test");
    {
        Detail::UniqueHandle first{rawEvent};
        Detail::UniqueHandle second{std::move(first)};
        require(!first && second.get() == rawEvent,
                "UniqueHandle move did not transfer sole ownership");
        second.reset(second.get());
        require(::WaitForSingleObject(rawEvent, 0) == WAIT_TIMEOUT,
                "same-handle reset closed the still-owned native handle");
    }
    require(::WaitForSingleObject(rawEvent, 0) == WAIT_FAILED &&
                ::GetLastError() == ERROR_INVALID_HANDLE,
            "UniqueHandle did not close its native handle exactly once");
}

void testApplicationPathsAndOverridePolicy()
{
    ScopedTestDirectory temporary;
    const std::filesystem::path explicitRoot = temporary.path() / L"owned";
    require(std::filesystem::create_directory(explicitRoot),
            "could not create the explicit app root");
    const Domain::PathText explicitText = pathText(explicitRoot);
    const auto context = activeContext();

    WindowsApplicationPaths paths{WindowsApplicationPathsOptions{explicitText, false}};
    require(take(paths.dataRoot(context)) == explicitText,
            "the explicit application data root changed");
    require(take(paths.configurationRoot(context)).value() == explicitText.value() + "\\config",
            "the configuration root did not use the canonical config child");
    require(take(paths.diagnosticsRoot(context)).value() == explicitText.value() + "\\logs",
            "the diagnostics root did not use the canonical logs child");
    const auto projectId = parse<Domain::ProjectId>("33333333-3333-4333-8333-333333333333");
    require(take(paths.projectRoot(projectId, context)).value() ==
                explicitText.value() + "\\projects\\" + projectId.value(),
            "the project root did not preserve the canonical project identity");

    auto expired = context;
    expired.deadline = std::chrono::steady_clock::now();
    requireError(paths.dataRoot(expired), Domain::ErrorCodes::DeadlineExceeded,
                 "application paths ignored an expired operation context");

    const std::filesystem::path overrideRoot = temporary.path() / L"override";
    const std::filesystem::path otherRoot = temporary.path() / L"explicit-wins";
    require(std::filesystem::create_directory(overrideRoot),
            "could not create the environment override root");
    require(std::filesystem::create_directory(otherRoot),
            "could not create the explicit-wins root");
    ScopedEnvironmentValue environment{L"FORGE_CONDUCTOR_HOME", overrideRoot.native()};

    WindowsApplicationPaths productionDefault;
    require(take(productionDefault.dataRoot(context)) != pathText(overrideRoot),
            "the production-default path provider consumed FORGE_CONDUCTOR_HOME");

    WindowsApplicationPaths optedIn{WindowsApplicationPathsOptions{std::nullopt, true}};
    require(take(optedIn.dataRoot(context)) == pathText(overrideRoot),
            "the injected development override was not honored");

    WindowsApplicationPaths explicitWins{WindowsApplicationPathsOptions{pathText(otherRoot), true}};
    require(take(explicitWins.dataRoot(context)) == pathText(otherRoot),
            "the environment overrode an explicitly injected app root");
}

void testPathResolverRejectsUnsafeForms()
{
    using Detail::WindowsPathResolver;

    requireError(WindowsPathResolver::resolveAppOwnedRoot("relative\\root"),
                 Domain::ErrorCodes::InvalidRequest, "a relative app-owned path was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("C:\\"),
                 Domain::ErrorCodes::InvalidRequest, "an over-broad drive root was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("\\\\server\\share\\root"),
                 Domain::ErrorCodes::PathOutsideAuthority, "a UNC app-owned path was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("\\\\?\\C:\\root"),
                 Domain::ErrorCodes::PathOutsideAuthority, "a device app-owned path was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("C:\\root\\NUL.txt"),
                 Domain::ErrorCodes::InvalidRequest,
                 "a reserved DOS device component was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("C:\\root\\file:stream"),
                 Domain::ErrorCodes::InvalidRequest, "an alternate data stream was accepted");
    requireError(WindowsPathResolver::resolveAppOwnedRoot("C:\\root\\..\\escape"),
                 Domain::ErrorCodes::InvalidRequest, "a dot-dot path was accepted");

    std::string malformed{"C:\\root\\"};
    malformed.append("\xC3\x28", 2);
    requireError(WindowsPathResolver::resolveAppOwnedRoot(malformed),
                 Domain::ErrorCodes::InvalidRequest,
                 "malformed UTF-8 was accepted as an app-owned path");

    ScopedTestDirectory temporary;
    const std::filesystem::path target = temporary.path() / L"target";
    const std::filesystem::path link = temporary.path() / L"link";
    require(std::filesystem::create_directory(target), "could not create a reparse target");
    const BOOLEAN created = ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
                                                  SYMBOLIC_LINK_FLAG_DIRECTORY |
                                                      SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (created != FALSE) {
        const auto rejected = WindowsPathResolver::resolveAppOwnedRoot(pathText(link).value());
        requireError(rejected, Domain::ErrorCodes::PathOutsideAuthority,
                     "an existing reparse point was accepted as an app-owned root");
    }
}

} // namespace

void registerFoundationWindowsTests(TestRegistry& tests)
{
    addTest(tests, "foundation.clocks_and_deadline_gate", testClocksAndDeadlineGate);
    addTest(tests, "foundation.uuid_and_sha256", testUuidAndSha256);
    addTest(tests, "foundation.secret_redaction", testSecretRedaction);
    addTest(tests, "foundation.utf_and_handle_owners", testUtfAndHandleOwners);
    addTest(tests, "foundation.application_paths_and_override_policy",
            testApplicationPathsAndOverridePolicy);
    addTest(tests, "foundation.path_resolver_rejects_unsafe_forms",
            testPathResolverRejectsUnsafeForms);
}

} // namespace ForgeConductor::Tests
