#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"

#include "Detail/Win32Error.h"

#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {

Domain::Result<Domain::Uuid> WindowsUuidGenerator::next() noexcept
{
    try {
        std::array<unsigned char, 16> bytes{};
        const NTSTATUS status = ::BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            return Domain::Result<Domain::Uuid>::failure(
                Detail::makeNtStatusError("generate a UUID", status));
        }

        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

        static constexpr char Hex[] = "0123456789abcdef";
        std::string canonical(36, '-');
        std::size_t outputIndex = 0;
        for (std::size_t byteIndex = 0; byteIndex < bytes.size(); ++byteIndex) {
            if (outputIndex == 8U || outputIndex == 13U || outputIndex == 18U ||
                outputIndex == 23U) {
                ++outputIndex;
            }
            const unsigned char value = bytes[byteIndex];
            canonical[outputIndex++] = Hex[(value >> 4U) & 0x0fU];
            canonical[outputIndex++] = Hex[value & 0x0fU];
        }
        return Domain::Uuid::parse(canonical);
    } catch (...) {
        return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A UUID could not be generated from the Windows system RNG."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
