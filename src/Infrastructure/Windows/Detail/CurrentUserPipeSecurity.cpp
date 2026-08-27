#include "CurrentUserPipeSecurity.h"

#include "UniqueHandle.h"
#include "Win32Error.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

constexpr std::size_t MinimumSidBytes = 8U;

[[nodiscard]] Domain::Result<void> validateSid(
    const std::span<const std::byte> sidBytes,
    const char* const description) noexcept
{
    try {
        if (sidBytes.size() < MinimumSidBytes ||
            sidBytes.size() > WindowsCurrentUserIdentity::MaximumSidBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{description} + " had an invalid byte length."));
        }

        const PSID sid = reinterpret_cast<PSID>(
            const_cast<std::byte*>(sidBytes.data()));
        if (::IsValidSid(sid) == FALSE ||
            ::GetLengthSid(sid) != static_cast<DWORD>(sidBytes.size())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                std::string{description} + " was not a canonical Windows SID."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager pipe SID could not be validated."));
    }
}

class ImpersonationScope final {
public:
    ImpersonationScope() noexcept = default;
    ~ImpersonationScope() noexcept
    {
        if (::RevertToSelf() == FALSE) {
            // Continuing could leave this thread running as an untrusted pipe
            // client. There is no safe recovery that preserves process state.
            std::terminate();
        }
    }

    ImpersonationScope(const ImpersonationScope&) = delete;
    ImpersonationScope& operator=(const ImpersonationScope&) = delete;
    ImpersonationScope(ImpersonationScope&&) = delete;
    ImpersonationScope& operator=(ImpersonationScope&&) = delete;
};

} // namespace

Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>
CurrentUserPipeSecurity::create(
    const WindowsCurrentUserIdentity& identity) noexcept
{
    try {
        const std::span<const std::byte> sidBytes = identity.sidBytes();
        auto validSid = validateSid(sidBytes, "The current-user manager pipe SID");
        if (!validSid) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                std::move(validSid).error());
        }

        const std::size_t aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) -
            sizeof(DWORD) + sidBytes.size();
        if (aclBytes > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The current-user manager pipe DACL exceeded its Windows size bound."));
        }

        auto security = std::unique_ptr<CurrentUserPipeSecurity>{
            new CurrentUserPipeSecurity{}};
        security->aclStorage_.resize(aclBytes);
        auto* const acl = reinterpret_cast<PACL>(security->aclStorage_.data());
        if (::InitializeAcl(
                acl, static_cast<DWORD>(security->aclStorage_.size()),
                ACL_REVISION) == FALSE) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                makeWin32Error(
                    "initialize the current-user manager pipe DACL",
                    ::GetLastError(), Domain::ErrorCodes::Unauthorized));
        }

        const PSID sid = reinterpret_cast<PSID>(
            const_cast<std::byte*>(sidBytes.data()));
        if (::AddAccessAllowedAceEx(
                acl, ACL_REVISION, 0U, GrantedAccess, sid) == FALSE) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                makeWin32Error(
                    "add the current-user manager pipe DACL entry",
                    ::GetLastError(), Domain::ErrorCodes::Unauthorized));
        }

        if (::InitializeSecurityDescriptor(
                &security->descriptor_, SECURITY_DESCRIPTOR_REVISION) == FALSE) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                makeWin32Error(
                    "initialize the current-user manager pipe security descriptor",
                    ::GetLastError()));
        }
        if (::SetSecurityDescriptorDacl(
                &security->descriptor_, TRUE, acl, FALSE) == FALSE) {
            return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
                makeWin32Error(
                    "apply the current-user manager pipe DACL",
                    ::GetLastError(), Domain::ErrorCodes::Unauthorized));
        }

        security->attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
        security->attributes_.lpSecurityDescriptor = &security->descriptor_;
        security->attributes_.bInheritHandle = FALSE;
        return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::success(
            std::move(security));
    } catch (...) {
        return Domain::Result<std::unique_ptr<CurrentUserPipeSecurity>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The current-user manager pipe security state could not be allocated."));
    }
}

Domain::Result<void> verifyNamedPipeClientSid(
    const HANDLE pipe,
    const std::span<const std::byte> expectedSid) noexcept
{
    try {
        if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager pipe client identity check received an invalid pipe handle."));
        }
        auto validExpectedSid = validateSid(
            expectedSid, "The expected manager pipe client SID");
        if (!validExpectedSid) {
            return validExpectedSid;
        }

        if (::ImpersonateNamedPipeClient(pipe) == FALSE) {
            return Domain::Result<void>::failure(makeWin32Error(
                "impersonate the manager named-pipe client", ::GetLastError(),
                Domain::ErrorCodes::Unauthorized));
        }
        const ImpersonationScope impersonation;

        HANDLE rawToken = nullptr;
        if (::OpenThreadToken(
                ::GetCurrentThread(), TOKEN_QUERY, TRUE, &rawToken) == FALSE) {
            return Domain::Result<void>::failure(makeWin32Error(
                "open the impersonated manager pipe client token",
                ::GetLastError(), Domain::ErrorCodes::Unauthorized));
        }
        UniqueHandle token{rawToken};

        DWORD requiredBytes = 0U;
        ::SetLastError(ERROR_SUCCESS);
        const BOOL sized = ::GetTokenInformation(
            token.get(), TokenUser, nullptr, 0U, &requiredBytes);
        const DWORD sizingError = ::GetLastError();
        if (sized != FALSE || sizingError != ERROR_INSUFFICIENT_BUFFER) {
            return Domain::Result<void>::failure(makeWin32Error(
                "size the impersonated manager pipe client identity",
                sizingError, Domain::ErrorCodes::Unauthorized));
        }
        if (requiredBytes < sizeof(TOKEN_USER) ||
            requiredBytes > WindowsCurrentUserIdentity::MaximumTokenUserInformationBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The manager pipe client token identity exceeded its bounded size."));
        }

        std::vector<std::byte> information(requiredBytes);
        DWORD returnedBytes = 0U;
        if (::GetTokenInformation(
                token.get(), TokenUser, information.data(), requiredBytes,
                &returnedBytes) == FALSE) {
            return Domain::Result<void>::failure(makeWin32Error(
                "read the impersonated manager pipe client identity",
                ::GetLastError(), Domain::ErrorCodes::Unauthorized));
        }
        if (returnedBytes < sizeof(TOKEN_USER) || returnedBytes > requiredBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The manager pipe client token identity had an inconsistent size."));
        }

        const auto* const tokenUser =
            reinterpret_cast<const TOKEN_USER*>(information.data());
        const PSID clientSid = tokenUser->User.Sid;
        const std::uintptr_t informationBegin =
            reinterpret_cast<std::uintptr_t>(information.data());
        const std::uintptr_t informationEnd = informationBegin + returnedBytes;
        const std::uintptr_t sidBegin = reinterpret_cast<std::uintptr_t>(clientSid);
        if (clientSid == nullptr || sidBegin < informationBegin ||
            sidBegin > informationEnd ||
            (informationEnd - sidBegin) < MinimumSidBytes ||
            ::IsValidSid(clientSid) == FALSE) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The manager pipe client token did not contain a valid SID."));
        }

        const DWORD clientSidLength = ::GetLengthSid(clientSid);
        if (clientSidLength < MinimumSidBytes ||
            clientSidLength > WindowsCurrentUserIdentity::MaximumSidBytes ||
            static_cast<std::uintptr_t>(clientSidLength) >
                (informationEnd - sidBegin)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The manager pipe client SID exceeded its bounded token storage."));
        }

        const std::span<const std::byte> actualSid{
            reinterpret_cast<const std::byte*>(clientSid),
            static_cast<std::size_t>(clientSidLength)};
        if (actualSid.size() != expectedSid.size() ||
            !std::ranges::equal(actualSid, expectedSid)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "The manager pipe client does not belong to the current user."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The manager pipe client identity could not be verified."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
