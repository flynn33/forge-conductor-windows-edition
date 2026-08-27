#pragma once

#include "ForgeConductor/Contracts/ISecureStorage.h"
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

// Owns exactly one manager authentication secret. Ownership is move-only so
// raw nonce material cannot be copied accidentally. Destruction and moves
// erase the relinquished storage through volatile writes.
class ManagerAuthenticationSecret final {
public:
    static constexpr std::size_t SizeBytes = 32U;
    using Bytes = std::array<std::byte, SizeBytes>;

    explicit ManagerAuthenticationSecret(Bytes bytes) noexcept
        : bytes_{bytes}
    {
        erase(bytes);
    }

    ~ManagerAuthenticationSecret() noexcept { erase(bytes_); }

    ManagerAuthenticationSecret(const ManagerAuthenticationSecret&) = delete;
    ManagerAuthenticationSecret& operator=(const ManagerAuthenticationSecret&) = delete;

    ManagerAuthenticationSecret(ManagerAuthenticationSecret&& other) noexcept
        : bytes_{other.bytes_}
    {
        erase(other.bytes_);
    }

    ManagerAuthenticationSecret& operator=(ManagerAuthenticationSecret&& other) noexcept
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

class IManagerAuthenticationTokenGenerator {
public:
    virtual ~IManagerAuthenticationTokenGenerator() = default;

    [[nodiscard]] virtual Domain::Result<ManagerAuthenticationSecret> next() noexcept = 0;
};

class IManagerAuthenticationTokenStore {
public:
    virtual ~IManagerAuthenticationTokenStore() = default;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::Sha256Digest>> load(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::Sha256Digest> loadOrCreate(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

// Encodes the exact nonce bytes as the lowercase 64-hex-character wire token.
// This is an encoding operation, not a hash operation. Keeping the neutral
// helper inline prevents contract consumers from acquiring a Windows
// infrastructure link dependency.
[[nodiscard]] inline Domain::Result<Domain::Sha256Digest>
encodeManagerAuthenticationToken(const ManagerAuthenticationSecret& secret) noexcept
{
    constexpr std::size_t CharacterCount = ManagerAuthenticationSecret::SizeBytes * 2U;
    class EncodedTokenBuffer final {
    public:
        EncodedTokenBuffer() noexcept = default;

        ~EncodedTokenBuffer() noexcept
        {
            volatile char* destination = characters_.data();
            for (std::size_t index = 0U; index < characters_.size(); ++index) {
                destination[index] = '\0';
            }
        }

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
            "Manager authentication token encoding failed."));
    }
}

// Compares every token character and does not return early on a mismatch. The
// backing strings remain bound by reference so this noexcept path neither
// allocates nor copies token material.
[[nodiscard]] inline bool constantTimeManagerAuthenticationTokenEquals(
    const Domain::Sha256Digest& left,
    const Domain::Sha256Digest& right) noexcept
{
    const std::string& leftValue = left.value();
    const std::string& rightValue = right.value();
    std::size_t difference = leftValue.size() ^ rightValue.size();
    constexpr std::size_t TokenCharacters = ManagerAuthenticationSecret::SizeBytes * 2U;
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
