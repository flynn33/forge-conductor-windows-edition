#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"

#include "Detail/SecureBuffer.h"
#include "Detail/UniqueBCryptHandle.h"
#include "Detail/Win32Error.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr ULONG Sha256ByteCount = 32U;
constexpr ULONG MaximumHashObjectBytes = 1024U * 1024U;

[[nodiscard]] Domain::Result<ULONG> algorithmProperty(
    const BCRYPT_ALG_HANDLE algorithm,
    const wchar_t* const property) noexcept
{
    try {
        ULONG value = 0;
        ULONG returned = 0;
        const NTSTATUS status = ::BCryptGetProperty(
            algorithm,
            property,
            reinterpret_cast<PUCHAR>(&value),
            static_cast<ULONG>(sizeof(value)),
            &returned,
            0);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<ULONG>::failure(Detail::makeNtStatusError(
                "read a BCrypt SHA-256 property", status));
        }
        if (returned != sizeof(value)) {
            return Domain::Result<ULONG>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "BCrypt returned an unexpected SHA-256 property size."));
        }
        return Domain::Result<ULONG>::success(value);
    } catch (...) {
        return Domain::Result<ULONG>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A BCrypt SHA-256 property could not be read."));
    }
}

} // namespace

Domain::Result<Domain::Sha256Digest> BCryptSha256Hasher::sha256(
    const std::span<const std::byte> bytes) noexcept
{
    try {
        BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
        NTSTATUS status = ::BCryptOpenAlgorithmProvider(
            &rawAlgorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                Detail::makeNtStatusError(
                    "open the BCrypt SHA-256 provider", status));
        }
        Detail::UniqueBCryptAlgorithmHandle algorithm{rawAlgorithm};

        auto objectLength = algorithmProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH);
        if (!objectLength) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                std::move(objectLength).error());
        }
        if (objectLength.value() == 0 ||
            objectLength.value() > MaximumHashObjectBytes) {
            return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "BCrypt reported an unsafe SHA-256 object length."));
        }

        auto hashLength = algorithmProperty(algorithm.get(), BCRYPT_HASH_LENGTH);
        if (!hashLength) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                std::move(hashLength).error());
        }
        if (hashLength.value() != Sha256ByteCount) {
            return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "BCrypt did not expose a 32-byte SHA-256 digest."));
        }

        Detail::SecureBuffer hashObject{objectLength.value()};
        BCRYPT_HASH_HANDLE rawHash = nullptr;
        status = ::BCryptCreateHash(
            algorithm.get(),
            &rawHash,
            reinterpret_cast<PUCHAR>(hashObject.data()),
            objectLength.value(),
            nullptr,
            0,
            0);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                Detail::makeNtStatusError("create a BCrypt SHA-256 hash", status));
        }
        Detail::UniqueBCryptHashHandle hash{rawHash};

        std::size_t offset = 0;
        constexpr std::size_t MaximumChunk =
            static_cast<std::size_t>((std::numeric_limits<ULONG>::max)());
        while (offset < bytes.size()) {
            const std::size_t chunkSize = (std::min)(MaximumChunk, bytes.size() - offset);
            status = ::BCryptHashData(
                hash.get(),
                reinterpret_cast<PUCHAR>(
                    const_cast<std::byte*>(bytes.data() + offset)),
                static_cast<ULONG>(chunkSize),
                0);
            if (!BCRYPT_SUCCESS(status)) {
                return Domain::Result<Domain::Sha256Digest>::failure(
                    Detail::makeNtStatusError(
                        "update a BCrypt SHA-256 hash", status));
            }
            offset += chunkSize;
        }

        Detail::SecureBuffer digestBytes{Sha256ByteCount};
        status = ::BCryptFinishHash(
            hash.get(),
            reinterpret_cast<PUCHAR>(digestBytes.data()),
            Sha256ByteCount,
            0);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                Detail::makeNtStatusError("finish a BCrypt SHA-256 hash", status));
        }

        static constexpr char Hex[] = "0123456789abcdef";
        std::string encoded(Sha256ByteCount * 2U, '0');
        for (std::size_t index = 0; index < digestBytes.size(); ++index) {
            const auto value = static_cast<unsigned char>(digestBytes.data()[index]);
            encoded[index * 2U] = Hex[(value >> 4U) & 0x0fU];
            encoded[index * 2U + 1U] = Hex[value & 0x0fU];
        }
        return Domain::Sha256Digest::parse(encoded);
    } catch (...) {
        return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The BCrypt SHA-256 operation could not allocate bounded state."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
