#pragma once

#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Contracts {

struct UnicodeCanonicalizationLimits final {
    static constexpr std::size_t MaximumInputBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumKeyBytes = 3U * MaximumInputBytes;
};

// An immutable NFC UTF-8 comparison key. Canonicalizer implementations and
// deterministic test fakes construct instances only after validating and
// normalizing the source text.
class NfcUtf8Key final {
public:
    [[nodiscard]] static Domain::Result<NfcUtf8Key> create(
        std::string value) noexcept
    {
        try {
            if (value.size() >
                    UnicodeCanonicalizationLimits::MaximumKeyBytes ||
                !Domain::isValidUtf8(value)) {
                return Domain::Result<NfcUtf8Key>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "A Unicode canonicalizer returned an invalid NFC key."));
            }
            return Domain::Result<NfcUtf8Key>::success(
                NfcUtf8Key{PrivateTag{}, std::move(value)});
        } catch (...) {
            return Domain::Result<NfcUtf8Key>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "A bounded NFC key could not be allocated."));
        }
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    [[nodiscard]] bool operator==(const NfcUtf8Key&) const noexcept = default;

    [[nodiscard]] std::strong_ordering operator<=>(
        const NfcUtf8Key& other) const noexcept
    {
        const std::size_t shared = (std::min)(value_.size(), other.value_.size());
        for (std::size_t index = 0; index < shared; ++index) {
            const auto left = static_cast<unsigned char>(value_[index]);
            const auto right = static_cast<unsigned char>(other.value_[index]);
            if (left < right) {
                return std::strong_ordering::less;
            }
            if (left > right) {
                return std::strong_ordering::greater;
            }
        }
        return value_.size() <=> other.value_.size();
    }

private:
    struct PrivateTag final {};

    NfcUtf8Key(PrivateTag, std::string value) noexcept
        : value_{std::move(value)}
    {
    }

    std::string value_;
};

class IUnicodeCanonicalizer {
public:
    static constexpr std::size_t MaximumInputBytes =
        UnicodeCanonicalizationLimits::MaximumInputBytes;
    static constexpr std::size_t MaximumKeyBytes =
        UnicodeCanonicalizationLimits::MaximumKeyBytes;

    virtual ~IUnicodeCanonicalizer() = default;

    [[nodiscard]] virtual Domain::Result<NfcUtf8Key> nfcKey(
        std::string_view value) const noexcept = 0;
};

} // namespace ForgeConductor::Contracts
