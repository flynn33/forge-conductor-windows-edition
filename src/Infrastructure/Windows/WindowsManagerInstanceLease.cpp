#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"

#include "Detail/UniqueHandle.h"
#include "Detail/UniqueLocalAllocation.h"
#include "Detail/Win32Error.h"

#include <Windows.h>
#include <aclapi.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::wstring_view MutexPrefix =
    // The pipe namespace is machine-wide, so the ownership primitive must be
    // machine-wide as well. The current-user SID hash and DACL preserve
    // per-user isolation across console and Remote Desktop sessions.
    L"Global\\ForgeConductor.Manager.v1.";
constexpr std::wstring_view PipePrefix =
    L"\\\\.\\pipe\\ForgeConductor.Manager.v1.";

[[nodiscard]] bool isLowerHex(const std::string_view value) noexcept
{
    return value.size() == WindowsCurrentUserIdentity::StableKeyCharacters &&
        std::ranges::all_of(value, [](const char character) noexcept {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

[[nodiscard]] bool isSafePurposeCharacter(const char character) noexcept
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_';
}

[[nodiscard]] Domain::Result<std::wstring> validatedSuffix(
    const std::string_view purposeSuffix) noexcept
{
    try {
        if (purposeSuffix.size() >
            WindowsManagerInstanceLease::MaximumPurposeSuffixCharacters) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager instance purpose suffix exceeded its character bound."));
        }
        if (!std::ranges::all_of(purposeSuffix, isSafePurposeCharacter)) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager instance purpose suffix must contain only safe ASCII characters."));
        }
        if (purposeSuffix.empty()) {
            return Domain::Result<std::wstring>::success({});
        }

        std::wstring suffix;
        suffix.reserve(purposeSuffix.size() + 1U);
        suffix.push_back(L'.');
        for (const char character : purposeSuffix) {
            suffix.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(character)));
        }
        return Domain::Result<std::wstring>::success(std::move(suffix));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager instance purpose suffix could not allocate bounded state."));
    }
}

[[nodiscard]] Domain::Result<void> validateIdentity(
    const WindowsCurrentUserIdentity& identity) noexcept
{
    try {
        if (!isLowerHex(identity.stableKey())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The manager instance identity did not contain a canonical SHA-256 key."));
        }

        const std::span<const std::byte> sidBytes = identity.sidBytes();
        if (sidBytes.size() < 8U ||
            sidBytes.size() > WindowsCurrentUserIdentity::MaximumSidBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The manager instance identity contained an invalid SID size."));
        }
        const PSID sid = reinterpret_cast<PSID>(
            const_cast<std::byte*>(sidBytes.data()));
        if (::IsValidSid(sid) == FALSE ||
            ::GetLengthSid(sid) != static_cast<DWORD>(sidBytes.size())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The manager instance identity contained invalid SID bytes."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager instance identity could not be validated."));
    }
}

} // namespace

class WindowsManagerInstanceLease::Impl final {
public:
    Impl(
        Detail::UniqueHandle handle,
        std::wstring mutexName,
        std::wstring pipeName) noexcept
        : handle_{std::move(handle)},
          mutexName_{std::move(mutexName)},
          pipeName_{std::move(pipeName)}
    {
    }

    ~Impl() noexcept = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] bool owns() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] std::wstring_view mutexName() const noexcept { return mutexName_; }
    [[nodiscard]] std::wstring_view pipeName() const noexcept { return pipeName_; }

private:
    Detail::UniqueHandle handle_;
    std::wstring mutexName_;
    std::wstring pipeName_;
};

WindowsManagerInstanceLease::WindowsManagerInstanceLease(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsManagerInstanceLease::~WindowsManagerInstanceLease() noexcept = default;

WindowsManagerInstanceLease::WindowsManagerInstanceLease(
    WindowsManagerInstanceLease&& other) noexcept = default;

WindowsManagerInstanceLease& WindowsManagerInstanceLease::operator=(
    WindowsManagerInstanceLease&& other) noexcept = default;

Domain::Result<WindowsManagerInstanceLease>
WindowsManagerInstanceLease::acquire(
    const WindowsCurrentUserIdentity& identity,
    const WindowsManagerInstanceLeaseOptions& options) noexcept
{
    try {
        auto validIdentity = validateIdentity(identity);
        if (!validIdentity) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                std::move(validIdentity).error());
        }
        auto suffix = validatedSuffix(options.purposeSuffix);
        if (!suffix) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                std::move(suffix).error());
        }

        std::wstring stableKey;
        stableKey.reserve(identity.stableKey().size());
        for (const char character : identity.stableKey()) {
            stableKey.push_back(static_cast<wchar_t>(character));
        }

        std::wstring mutexName{MutexPrefix};
        mutexName += stableKey;
        mutexName += suffix.value();
        std::wstring pipeName{PipePrefix};
        pipeName += stableKey;
        pipeName += suffix.value();
        if (mutexName.size() > MaximumMutexNameCharacters ||
            pipeName.size() > MaximumPipeNameCharacters) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The manager instance names exceeded their Windows object-name bounds."));
        }

        const std::span<const std::byte> sidBytes = identity.sidBytes();
        const PSID sid = reinterpret_cast<PSID>(
            const_cast<std::byte*>(sidBytes.data()));
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = MUTEX_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.pMultipleTrustee = nullptr;
        access.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

        PACL rawAcl = nullptr;
        const DWORD aclStatus = ::SetEntriesInAclW(1U, &access, nullptr, &rawAcl);
        if (aclStatus != ERROR_SUCCESS) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Detail::makeWin32Error(
                    "create the current-user manager mutex DACL", aclStatus,
                    Domain::ErrorCodes::Unauthorized));
        }
        Detail::UniqueLocalAllocation<ACL> acl{rawAcl};

        SECURITY_DESCRIPTOR descriptor{};
        if (::InitializeSecurityDescriptor(
                &descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Detail::makeWin32Error(
                    "initialize the current-user manager mutex security descriptor",
                    ::GetLastError()));
        }
        if (::SetSecurityDescriptorDacl(
                &descriptor, TRUE, acl.get(), FALSE) == FALSE) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Detail::makeWin32Error(
                    "apply the current-user manager mutex DACL", ::GetLastError(),
                    Domain::ErrorCodes::Unauthorized));
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = &descriptor;
        securityAttributes.bInheritHandle = FALSE;

        ::SetLastError(ERROR_SUCCESS);
        Detail::UniqueHandle handle{
            // Object existence, retained by this handle, is the process lease.
            // Avoid thread-affine Win32 mutex ownership so the move-only lease
            // can be transferred and destroyed safely by another thread.
            ::CreateMutexW(&securityAttributes, FALSE, mutexName.c_str())};
        const DWORD creationStatus = ::GetLastError();
        if (!handle) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Detail::makeWin32Error(
                    "create the current-user manager instance mutex",
                    creationStatus));
        }
        if (creationStatus == ERROR_ALREADY_EXISTS) {
            return Domain::Result<WindowsManagerInstanceLease>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::OwnershipConflict,
                    "A Forge Conductor manager already owns the current-user instance lease."));
        }

        return Domain::Result<WindowsManagerInstanceLease>::success(
            WindowsManagerInstanceLease{std::make_unique<Impl>(
                std::move(handle), std::move(mutexName), std::move(pipeName))});
    } catch (...) {
        return Domain::Result<WindowsManagerInstanceLease>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The current-user manager instance lease could not allocate bounded state."));
    }
}

bool WindowsManagerInstanceLease::owns() const noexcept
{
    return implementation_ != nullptr && implementation_->owns();
}

std::wstring_view WindowsManagerInstanceLease::mutexName() const noexcept
{
    return implementation_ != nullptr ? implementation_->mutexName() :
        std::wstring_view{};
}

std::wstring_view WindowsManagerInstanceLease::pipeName() const noexcept
{
    return implementation_ != nullptr ? implementation_->pipeName() :
        std::wstring_view{};
}

} // namespace ForgeConductor::Infrastructure::Windows
