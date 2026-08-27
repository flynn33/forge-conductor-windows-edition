#pragma once

#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

// Owns exactly one dashboard bearer secret. Raw material is move-only and is
// erased when ownership moves or ends. The encoded bearer is deliberately a
// separate value so callers cannot accidentally persist this object in logs.
class DashboardBearerSecret final {
public:
    static constexpr std::size_t SizeBytes = 32U;
    using Bytes = std::array<std::byte, SizeBytes>;

    explicit DashboardBearerSecret(Bytes bytes) noexcept
        : bytes_{bytes}
    {
        erase(bytes);
    }

    ~DashboardBearerSecret() noexcept { erase(bytes_); }

    DashboardBearerSecret(const DashboardBearerSecret&) = delete;
    DashboardBearerSecret& operator=(const DashboardBearerSecret&) = delete;

    DashboardBearerSecret(DashboardBearerSecret&& other) noexcept
        : bytes_{other.bytes_}
    {
        erase(other.bytes_);
    }

    DashboardBearerSecret& operator=(DashboardBearerSecret&& other) noexcept
    {
        if (this != &other) {
            erase(bytes_);
            bytes_ = other.bytes_;
            erase(other.bytes_);
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::byte, SizeBytes> bytes() const noexcept
    {
        return bytes_;
    }

private:
    static void erase(Bytes& bytes) noexcept
    {
        volatile std::byte* destination = bytes.data();
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            destination[index] = std::byte{};
        }
    }

    Bytes bytes_{};
};

class IDashboardBearerTokenGenerator {
public:
    virtual ~IDashboardBearerTokenGenerator() = default;

    [[nodiscard]] virtual Domain::Result<DashboardBearerSecret> next() noexcept = 0;
};

class IDashboardBearerTokenStore {
public:
    virtual ~IDashboardBearerTokenStore() = default;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::Sha256Digest>> load(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::Sha256Digest> loadOrCreate(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

// Encodes the exact 256-bit secret as a lowercase 64-hex-character bearer.
// This is an encoding operation, not a hash operation. The digest domain type
// supplies the already-established fixed-width lowercase-hex invariant.
[[nodiscard]] inline Domain::Result<Domain::Sha256Digest>
encodeDashboardBearerToken(const DashboardBearerSecret& secret) noexcept
{
    constexpr std::size_t CharacterCount = DashboardBearerSecret::SizeBytes * 2U;
    class EncodedTokenBuffer final {
    public:
        ~EncodedTokenBuffer() noexcept
        {
            volatile char* destination = characters_.data();
            for (std::size_t index = 0U; index < characters_.size(); ++index) {
                destination[index] = '\0';
            }
        }

        EncodedTokenBuffer() noexcept = default;
        EncodedTokenBuffer(const EncodedTokenBuffer&) = delete;
        EncodedTokenBuffer& operator=(const EncodedTokenBuffer&) = delete;

        [[nodiscard]] std::array<char, CharacterCount>& characters() noexcept
        {
            return characters_;
        }

        [[nodiscard]] std::string_view view() const noexcept
        {
            return {characters_.data(), characters_.size()};
        }

    private:
        std::array<char, CharacterCount> characters_{};
    };

    try {
        constexpr std::array<char, 16U> Hex{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        EncodedTokenBuffer encoded;
        auto& characters = encoded.characters();
        const auto bytes = secret.bytes();
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            const auto value = std::to_integer<unsigned int>(bytes[index]);
            characters[index * 2U] = Hex[(value >> 4U) & 0x0FU];
            characters[(index * 2U) + 1U] = Hex[value & 0x0FU];
        }
        return Domain::Sha256Digest::parse(encoded.view());
    } catch (...) {
        return Domain::Result<Domain::Sha256Digest>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard bearer encoding failed."));
    }
}

// Compare every character without returning early on a mismatch. Both domain
// values are already bounded to exactly 64 lowercase hexadecimal characters.
[[nodiscard]] inline bool constantTimeDashboardBearerTokenEquals(
    const Domain::Sha256Digest& left,
    const Domain::Sha256Digest& right) noexcept
{
    const std::string& leftValue = left.value();
    const std::string& rightValue = right.value();
    std::size_t difference = leftValue.size() ^ rightValue.size();
    constexpr std::size_t TokenCharacters = DashboardBearerSecret::SizeBytes * 2U;
    for (std::size_t index = 0U; index < TokenCharacters; ++index) {
        const auto leftCharacter = index < leftValue.size()
            ? static_cast<unsigned char>(leftValue[index])
            : static_cast<unsigned char>(0U);
        const auto rightCharacter = index < rightValue.size()
            ? static_cast<unsigned char>(rightValue[index])
            : static_cast<unsigned char>(0U);
        difference |= static_cast<std::size_t>(leftCharacter ^ rightCharacter);
    }
    return difference == 0U;
}

} // namespace ForgeConductor::Contracts
