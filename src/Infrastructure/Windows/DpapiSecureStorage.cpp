#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"

#include "Detail/BoundedSerialExecutor.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/UniqueRegistryKey.h"
#include "Detail/UtfConversion.h"
#include "Detail/Win32Error.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <wincrypt.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace ForgeConductor::Infrastructure::Windows {
namespace {

constexpr std::array<std::byte, 5> StoredEnvelopePrefix{
    std::byte{0x46}, std::byte{0x43}, std::byte{0x44}, std::byte{0x50}, std::byte{1}};
constexpr std::array<std::byte, 5> PlainEnvelopePrefix{
    std::byte{0x46}, std::byte{0x43}, std::byte{0x53}, std::byte{0x53}, std::byte{1}};
constexpr std::size_t PlainLengthBytes = sizeof(std::uint32_t);
constexpr std::size_t PlainHeaderBytes = PlainEnvelopePrefix.size() + PlainLengthBytes;
constexpr std::size_t MaximumProtectedBlobBytes =
    DpapiSecureStorage::MaximumSecretBytes + 64U * 1024U;
constexpr std::size_t MaximumStoredBlobBytes =
    StoredEnvelopePrefix.size() + MaximumProtectedBlobBytes;
constexpr std::size_t StoredValueNameCharacters = 3U + (32U * 2U);

struct StoredEntryCatalog final {
    std::size_t entryCount{};
    bool requestedEntryExists{};
};

class UniqueAlgorithmProvider final {
public:
    UniqueAlgorithmProvider() = default;
    UniqueAlgorithmProvider(const UniqueAlgorithmProvider&) = delete;
    UniqueAlgorithmProvider& operator=(const UniqueAlgorithmProvider&) = delete;
    ~UniqueAlgorithmProvider()
    {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class LocalDataBlob final {
public:
    explicit LocalDataBlob(const bool sensitive) noexcept : sensitive_{sensitive} {}

    LocalDataBlob(const LocalDataBlob&) = delete;
    LocalDataBlob& operator=(const LocalDataBlob&) = delete;
    ~LocalDataBlob()
    {
        if (blob_.pbData != nullptr) {
            if (sensitive_ && blob_.cbData != 0U) {
                SecureZeroMemory(blob_.pbData, blob_.cbData);
            }
            LocalFree(blob_.pbData);
        }
    }

    [[nodiscard]] DATA_BLOB* put() noexcept { return &blob_; }
    [[nodiscard]] const DATA_BLOB& get() const noexcept { return blob_; }

private:
    DATA_BLOB blob_{};
    bool sensitive_{};
};

class SensitiveBytes final {
public:
    explicit SensitiveBytes(const std::size_t count) : bytes_(count) {}
    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
    ~SensitiveBytes()
    {
        if (!bytes_.empty()) {
            SecureZeroMemory(bytes_.data(), bytes_.size());
        }
    }

    [[nodiscard]] std::vector<std::byte>& get() noexcept { return bytes_; }
    [[nodiscard]] const std::vector<std::byte>& get() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

class SensitiveDigest final {
public:
    SensitiveDigest() = default;
    SensitiveDigest(const SensitiveDigest&) = delete;
    SensitiveDigest& operator=(const SensitiveDigest&) = delete;

    SensitiveDigest(SensitiveDigest&& other) noexcept : bytes_{other.bytes_}
    {
        SecureZeroMemory(other.bytes_.data(), other.bytes_.size());
    }

    SensitiveDigest& operator=(SensitiveDigest&& other) noexcept
    {
        if (this != &other) {
            SecureZeroMemory(bytes_.data(), bytes_.size());
            bytes_ = other.bytes_;
            SecureZeroMemory(other.bytes_.data(), other.bytes_.size());
        }
        return *this;
    }

    ~SensitiveDigest() { SecureZeroMemory(bytes_.data(), bytes_.size()); }

    [[nodiscard]] std::array<std::byte, 32>& get() noexcept { return bytes_; }
    [[nodiscard]] const std::array<std::byte, 32>& get() const noexcept { return bytes_; }

private:
    std::array<std::byte, 32> bytes_{};
};

[[nodiscard]] Domain::Result<void> validateStorageKey(const std::string_view key) noexcept
{
    if (key.empty()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest, "Secure-storage key must not be empty."));
    }
    if (key.size() > DpapiSecureStorage::MaximumKeyBytes) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::PayloadTooLarge, "Secure-storage key exceeds 128 UTF-8 bytes."));
    }
    for (const unsigned char character : key) {
        if (character < 0x20U || character == 0x7FU) {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                                  "Secure-storage key contains a control character."));
        }
    }
    auto wide = Detail::strictUtf8ToUtf16(key);
    if (!wide) {
        return Domain::Result<void>::failure(std::move(wide).error());
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<SensitiveDigest> sha256(const std::string_view value) noexcept
{
    UniqueAlgorithmProvider algorithm;
    auto status = BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return Domain::Result<SensitiveDigest>::failure(Detail::makeNtStatusError(
            "Open SHA-256 provider", status, Domain::ErrorCodes::InternalFailure));
    }

    SensitiveDigest digest;
    status = BCryptHash(
        algorithm.get(), nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
        static_cast<ULONG>(value.size()), reinterpret_cast<PUCHAR>(digest.get().data()),
        static_cast<ULONG>(digest.get().size()));
    if (!BCRYPT_SUCCESS(status)) {
        return Domain::Result<SensitiveDigest>::failure(Detail::makeNtStatusError(
            "Hash secure-storage key", status, Domain::ErrorCodes::InternalFailure));
    }
    return Domain::Result<SensitiveDigest>::success(std::move(digest));
}

[[nodiscard]] std::wstring valueName(const std::array<std::byte, 32>& digest)
{
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring name{L"v1_"};
    name.reserve(3U + (digest.size() * 2U));
    for (const auto value : digest) {
        const auto byte = std::to_integer<unsigned int>(value);
        name.push_back(Hex[(byte >> 4U) & 0xFU]);
        name.push_back(Hex[byte & 0xFU]);
    }
    return name;
}

void encodeUint32(const std::uint32_t value, std::byte* const destination) noexcept
{
    destination[0] = static_cast<std::byte>(value & 0xFFU);
    destination[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    destination[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    destination[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint32_t decodeUint32(const std::byte* const source) noexcept
{
    return std::to_integer<std::uint32_t>(source[0]) |
           (std::to_integer<std::uint32_t>(source[1]) << 8U) |
           (std::to_integer<std::uint32_t>(source[2]) << 16U) |
           (std::to_integer<std::uint32_t>(source[3]) << 24U);
}

[[nodiscard]] DATA_BLOB entropyBlob(const std::array<std::byte, 32>& digest) noexcept
{
    return DATA_BLOB{static_cast<DWORD>(digest.size()),
                     reinterpret_cast<BYTE*>(const_cast<std::byte*>(digest.data()))};
}

[[nodiscard]] Domain::Result<std::vector<std::byte>>
protectSecret(const std::span<const std::byte> secret,
              const std::array<std::byte, 32>& digest) noexcept
{
    try {
        SensitiveBytes plain{PlainHeaderBytes + secret.size()};
        auto& bytes = plain.get();
        std::copy(PlainEnvelopePrefix.begin(), PlainEnvelopePrefix.end(), bytes.begin());
        encodeUint32(static_cast<std::uint32_t>(secret.size()),
                     bytes.data() + PlainEnvelopePrefix.size());
        std::copy(secret.begin(), secret.end(), bytes.begin() + PlainHeaderBytes);

        DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
        auto entropy = entropyBlob(digest);
        LocalDataBlob protectedBlob{false};
        if (!CryptProtectData(&input, L"Forge Conductor secure storage v1", &entropy, nullptr,
                              nullptr, CRYPTPROTECT_UI_FORBIDDEN, protectedBlob.put())) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Detail::makeWin32Error("Protect secure-storage value", GetLastError()));
        }

        const auto& encrypted = protectedBlob.get();
        if (encrypted.pbData == nullptr || encrypted.cbData == 0U ||
            encrypted.cbData > MaximumProtectedBlobBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "DPAPI returned an invalid protected value."));
        }

        std::vector<std::byte> stored(StoredEnvelopePrefix.size() + encrypted.cbData);
        std::copy(StoredEnvelopePrefix.begin(), StoredEnvelopePrefix.end(), stored.begin());
        std::memcpy(stored.data() + StoredEnvelopePrefix.size(), encrypted.pbData,
                    encrypted.cbData);
        return Domain::Result<std::vector<std::byte>>::success(std::move(stored));
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Secure-storage protection failed."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::byte>>
unprotectSecret(const std::span<const std::byte> stored, const std::array<std::byte, 32>& digest,
                const std::size_t maximumBytes) noexcept
{
    try {
        if (stored.size() <= StoredEnvelopePrefix.size() ||
            stored.size() > MaximumStoredBlobBytes ||
            !std::equal(StoredEnvelopePrefix.begin(), StoredEnvelopePrefix.end(), stored.begin())) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Secure-storage value has an invalid envelope."));
        }

        const auto protectedSize = stored.size() - StoredEnvelopePrefix.size();
        DATA_BLOB encrypted{static_cast<DWORD>(protectedSize),
                            reinterpret_cast<BYTE*>(const_cast<std::byte*>(
                                stored.data() + StoredEnvelopePrefix.size()))};
        auto entropy = entropyBlob(digest);
        LocalDataBlob plain{true};
        if (!CryptUnprotectData(&encrypted, nullptr, &entropy, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, plain.put())) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Detail::makeWin32Error("Unprotect secure-storage value", GetLastError(),
                                       Domain::ErrorCodes::IntegrityFailure));
        }

        const auto& decrypted = plain.get();
        if (decrypted.pbData == nullptr || decrypted.cbData < PlainHeaderBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Secure-storage plaintext has an invalid envelope."));
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(decrypted.pbData);
        if (!std::equal(PlainEnvelopePrefix.begin(), PlainEnvelopePrefix.end(), bytes)) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Secure-storage plaintext version is unsupported."));
        }

        const auto secretSize = decodeUint32(bytes + PlainEnvelopePrefix.size());
        if (secretSize > DpapiSecureStorage::MaximumSecretBytes ||
            static_cast<std::size_t>(decrypted.cbData) != PlainHeaderBytes + secretSize) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Secure-storage plaintext length is invalid."));
        }
        if (secretSize > maximumBytes) {
            return Domain::Result<std::vector<std::byte>>::failure(
                Domain::makeError(Domain::ErrorCodes::PayloadTooLarge,
                                  "Secure-storage value exceeds the caller's output bound."));
        }

        return Domain::Result<std::vector<std::byte>>::success(std::vector<std::byte>{
            bytes + PlainHeaderBytes, bytes + PlainHeaderBytes + secretSize});
    } catch (...) {
        return Domain::Result<std::vector<std::byte>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Secure-storage unprotection failed."));
    }
}

[[nodiscard]] bool isStoredValueName(const std::wstring_view name) noexcept
{
    if (name.size() != StoredValueNameCharacters || !name.starts_with(L"v1_")) {
        return false;
    }
    return std::all_of(name.begin() + 3, name.end(), [](const wchar_t character) noexcept {
        return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
    });
}

[[nodiscard]] Domain::Result<StoredEntryCatalog>
enumerateStoredEntries(const HKEY registryKey, const std::wstring_view requestedName) noexcept
{
    try {
        StoredEntryCatalog catalog;
        for (std::size_t index = 0U; index <= DpapiSecureStorage::MaximumEntryCount; ++index) {
            std::array<wchar_t, StoredValueNameCharacters + 1U> name{};
            DWORD nameCharacters = static_cast<DWORD>(name.size());
            DWORD type{};
            DWORD byteCount{};
            const auto status = RegEnumValueW(registryKey, static_cast<DWORD>(index), name.data(),
                                              &nameCharacters, nullptr, &type, nullptr, &byteCount);
            if (status == ERROR_NO_MORE_ITEMS) {
                return Domain::Result<StoredEntryCatalog>::success(catalog);
            }
            if (status == ERROR_MORE_DATA) {
                return Domain::Result<StoredEntryCatalog>::failure(
                    Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                      "Secure-storage registry contains overlong entry metadata."));
            }
            if (status != ERROR_SUCCESS) {
                return Domain::Result<StoredEntryCatalog>::failure(Detail::makeWin32Error(
                    "Enumerate secure-storage entries", static_cast<DWORD>(status)));
            }

            const std::wstring_view enumeratedName{name.data(),
                                                   static_cast<std::size_t>(nameCharacters)};
            if (!isStoredValueName(enumeratedName) || type != REG_BINARY ||
                byteCount > MaximumStoredBlobBytes) {
                return Domain::Result<StoredEntryCatalog>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Secure-storage registry contains an invalid entry name, type, or size."));
            }
            ++catalog.entryCount;
            catalog.requestedEntryExists =
                catalog.requestedEntryExists || enumeratedName == requestedName;
            if (catalog.entryCount > DpapiSecureStorage::MaximumEntryCount) {
                return Domain::Result<StoredEntryCatalog>::failure(
                    Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                                      "Secure storage exceeds its maximum 128 entries."));
            }
        }
        return Domain::Result<StoredEntryCatalog>::failure(
            Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                              "Secure-storage enumeration exceeded its bounded catalog."));
    } catch (...) {
        return Domain::Result<StoredEntryCatalog>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Secure-storage registry enumeration failed."));
    }
}

[[nodiscard]] Domain::Result<std::optional<std::vector<std::byte>>>
queryStoredBlob(const HKEY registryKey, const std::wstring& name) noexcept
{
    try {
        DWORD type{};
        DWORD byteCount{};
        auto status =
            RegQueryValueExW(registryKey, name.c_str(), nullptr, &type, nullptr, &byteCount);
        if (status == ERROR_FILE_NOT_FOUND) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
        }
        if (status != ERROR_SUCCESS) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Detail::makeWin32Error("Size secure-storage value", static_cast<DWORD>(status)));
        }
        if (type != REG_BINARY || byteCount > MaximumStoredBlobBytes) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Domain::makeError(Domain::ErrorCodes::IntegrityFailure,
                                  "Secure-storage registry value has an invalid type or size."));
        }

        std::vector<std::byte> stored(byteCount);
        status = RegQueryValueExW(registryKey, name.c_str(), nullptr, &type,
                                  reinterpret_cast<BYTE*>(stored.data()), &byteCount);
        if (status == ERROR_MORE_DATA) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Domain::makeError(Domain::ErrorCodes::Conflict,
                                  "Secure-storage value changed during its bounded read.", true));
        }
        if (status != ERROR_SUCCESS) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Detail::makeWin32Error("Read secure-storage value", static_cast<DWORD>(status)));
        }
        stored.resize(byteCount);
        return Domain::Result<std::optional<std::vector<std::byte>>>::success(
            std::optional<std::vector<std::byte>>{std::move(stored)});
    } catch (...) {
        return Domain::Result<std::optional<std::vector<std::byte>>>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure, "Secure-storage registry read failed."));
    }
}

} // namespace

class DpapiSecureStorage::Impl final {
public:
    explicit Impl(std::wstring registrySubkey) : registrySubkey_{std::move(registrySubkey)}
    {
        auto valid = Detail::validateForgeRegistrySubkey(registrySubkey_);
        if (!valid) {
            throw std::invalid_argument(valid.error().message);
        }
    }

    [[nodiscard]] Domain::Result<void> put(const std::string_view key,
                                           const std::span<const std::byte> secret,
                                           const Domain::OperationContext& context) noexcept
    {
        if (secret.size() > MaximumSecretBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge, "Secure-storage value exceeds 65536 bytes."));
        }
        auto validKey = validateStorageKey(key);
        if (!validKey) {
            return validKey;
        }

        auto lease = executor_.acquire(context, "Secure-storage put");
        if (!lease) {
            return Domain::Result<void>::failure(std::move(lease).error());
        }
        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "Secure-storage put");
        if (!validContext) {
            return validContext;
        }

        auto digest = sha256(key);
        if (!digest) {
            return Domain::Result<void>::failure(std::move(digest).error());
        }
        const auto name = valueName(digest.value().get());
        auto registry = Detail::UniqueRegistryKey::createCurrentUser(
            registrySubkey_, KEY_QUERY_VALUE | KEY_SET_VALUE);
        if (!registry) {
            return Domain::Result<void>::failure(std::move(registry).error());
        }

        auto catalog = enumerateStoredEntries(registry.value().get(), name);
        if (!catalog) {
            return Domain::Result<void>::failure(std::move(catalog).error());
        }
        if (!catalog.value().requestedEntryExists &&
            catalog.value().entryCount >= MaximumEntryCount) {
            return Domain::Result<void>::failure(
                Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                                  "Secure storage already contains its maximum 128 entries."));
        }

        auto protectedValue = protectSecret(secret, digest.value().get());
        if (!protectedValue) {
            return Domain::Result<void>::failure(std::move(protectedValue).error());
        }
        validContext = Detail::validateOperationContext(context, std::chrono::steady_clock::now(),
                                                        "Secure-storage put");
        if (!validContext) {
            return validContext;
        }

        const auto& stored = protectedValue.value();
        const auto status = RegSetValueExW(registry.value().get(), name.c_str(), 0, REG_BINARY,
                                           reinterpret_cast<const BYTE*>(stored.data()),
                                           static_cast<DWORD>(stored.size()));
        if (status != ERROR_SUCCESS) {
            return Domain::Result<void>::failure(
                Detail::makeWin32Error("Commit secure-storage value", static_cast<DWORD>(status)));
        }

        // Registry publication is the linearization point. Cancellation that
        // races after this successful commit must not report a false failure.
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<std::optional<std::vector<std::byte>>>
    get(const std::string_view key, const std::size_t maximumBytes,
        const Domain::OperationContext& context) noexcept
    {
        if (maximumBytes > MaximumSecretBytes) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                                  "Secure-storage output bound exceeds 65536 bytes."));
        }
        auto validKey = validateStorageKey(key);
        if (!validKey) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(validKey).error());
        }

        auto lease = executor_.acquire(context, "Secure-storage get");
        if (!lease) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(lease).error());
        }
        auto digest = sha256(key);
        if (!digest) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(digest).error());
        }
        const auto name = valueName(digest.value().get());

        auto registry =
            Detail::UniqueRegistryKey::openCurrentUser(registrySubkey_, KEY_QUERY_VALUE);
        if (!registry) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(registry).error());
        }
        if (!registry.value()) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
        }

        auto stored = queryStoredBlob(registry.value()->get(), name);
        if (!stored) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(stored).error());
        }
        if (!stored.value()) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::success(std::nullopt);
        }

        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "Secure-storage get");
        if (!validContext) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(validContext).error());
        }
        auto secret = unprotectSecret(stored.value().value(), digest.value().get(), maximumBytes);
        if (!secret) {
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(secret).error());
        }
        validContext = Detail::validateOperationContext(context, std::chrono::steady_clock::now(),
                                                        "Secure-storage get");
        if (!validContext) {
            if (!secret.value().empty()) {
                SecureZeroMemory(secret.value().data(), secret.value().size());
            }
            return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
                std::move(validContext).error());
        }
        return Domain::Result<std::optional<std::vector<std::byte>>>::success(
            std::optional<std::vector<std::byte>>{std::move(secret).value()});
    }

    [[nodiscard]] Domain::Result<void> remove(const std::string_view key,
                                              const Domain::OperationContext& context) noexcept
    {
        auto validKey = validateStorageKey(key);
        if (!validKey) {
            return validKey;
        }
        auto lease = executor_.acquire(context, "Secure-storage remove");
        if (!lease) {
            return Domain::Result<void>::failure(std::move(lease).error());
        }
        auto digest = sha256(key);
        if (!digest) {
            return Domain::Result<void>::failure(std::move(digest).error());
        }
        auto registry = Detail::UniqueRegistryKey::openCurrentUser(registrySubkey_, KEY_SET_VALUE);
        if (!registry) {
            return Domain::Result<void>::failure(std::move(registry).error());
        }
        if (!registry.value()) {
            return Domain::Result<void>::success();
        }

        auto validContext = Detail::validateOperationContext(
            context, std::chrono::steady_clock::now(), "Secure-storage remove");
        if (!validContext) {
            return validContext;
        }
        const auto name = valueName(digest.value().get());
        const auto status = RegDeleteValueW(registry.value()->get(), name.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            return Domain::Result<void>::failure(
                Detail::makeWin32Error("Remove secure-storage value", static_cast<DWORD>(status)));
        }
        return Domain::Result<void>::success();
    }

    void shutdown() noexcept { executor_.shutdown(); }

private:
    std::wstring registrySubkey_;
    Detail::BoundedSerialExecutor executor_;
};

DpapiSecureStorage::DpapiSecureStorage(std::wstring registrySubkey)
    : implementation_{std::make_unique<Impl>(std::move(registrySubkey))}
{
}

DpapiSecureStorage::~DpapiSecureStorage() { shutdown(); }

Domain::Result<void> DpapiSecureStorage::put(const std::string_view key,
                                             const std::span<const std::byte> secret,
                                             const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->put(key, secret, context);
    } catch (...) {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Secure-storage put failed at the platform boundary."));
    }
}

Domain::Result<std::optional<std::vector<std::byte>>>
DpapiSecureStorage::get(const std::string_view key, const std::size_t maximumBytes,
                        const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->get(key, maximumBytes, context);
    } catch (...) {
        return Domain::Result<std::optional<std::vector<std::byte>>>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Secure-storage get failed at the platform boundary."));
    }
}

Domain::Result<void> DpapiSecureStorage::remove(const std::string_view key,
                                                const Domain::OperationContext& context) noexcept
{
    try {
        return implementation_->remove(key, context);
    } catch (...) {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InternalFailure,
                              "Secure-storage remove failed at the platform boundary."));
    }
}

void DpapiSecureStorage::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
