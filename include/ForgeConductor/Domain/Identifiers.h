#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <compare>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Domain {

class Uuid final {
public:
    [[nodiscard]] static Result<Uuid> parse(std::string_view value);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    auto operator<=>(const Uuid&) const = default;

private:
    explicit Uuid(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

template <typename Tag>
class StrongUuid final {
public:
    explicit StrongUuid(Uuid value) : value_{std::move(value)} {}

    [[nodiscard]] static Result<StrongUuid> parse(const std::string_view value)
    {
        auto parsed = Uuid::parse(value);
        if (!parsed) {
            return Result<StrongUuid>::failure(std::move(parsed).error());
        }
        return Result<StrongUuid>::success(StrongUuid{std::move(parsed).value()});
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_.value(); }
    [[nodiscard]] const Uuid& uuid() const noexcept { return value_; }

    auto operator<=>(const StrongUuid&) const = default;

private:
    Uuid value_;
};

[[nodiscard]] Result<std::string> validateOpaqueIdentifier(
    std::string_view value,
    std::size_t maximumBytes = 128);

template <typename Tag>
class OpaqueId final {
public:
    [[nodiscard]] static Result<OpaqueId> parse(
        const std::string_view value,
        const std::size_t maximumBytes = 128)
    {
        auto validated = validateOpaqueIdentifier(value, maximumBytes);
        if (!validated) {
            return Result<OpaqueId>::failure(std::move(validated).error());
        }
        return Result<OpaqueId>::success(OpaqueId{std::move(validated).value()});
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    auto operator<=>(const OpaqueId&) const = default;

private:
    explicit OpaqueId(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

class IdempotencyKey final {
public:
    static constexpr std::size_t MaximumBytes = 256;

    [[nodiscard]] static Result<IdempotencyKey> create(std::string_view value);
    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    auto operator<=>(const IdempotencyKey&) const = default;

private:
    explicit IdempotencyKey(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

class Sha256Digest final {
public:
    [[nodiscard]] static Result<Sha256Digest> parse(std::string_view value);
    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    auto operator<=>(const Sha256Digest&) const = default;

private:
    explicit Sha256Digest(std::string value) : value_{std::move(value)} {}

    std::string value_;
};

struct AuthorityIdTag;
struct ProjectIdTag;
struct MemoryRecordIdTag;
struct SessionIdTag;
struct OperationIdTag;
struct ContinuityOperationIdTag;
struct ContinuityHandoffIdTag;
struct ClientIdTag;
struct AgentIdTag;
struct AdapterIdTag;
struct ProviderSessionIdTag;
struct EvidenceIdTag;
struct DeploymentIdTag;
struct CorrelationIdTag;
struct RequestIdTag;
struct LegacyHandoffIdTag;

using AuthorityId = StrongUuid<AuthorityIdTag>;
using ProjectId = StrongUuid<ProjectIdTag>;
using MemoryRecordId = StrongUuid<MemoryRecordIdTag>;
using SessionId = StrongUuid<SessionIdTag>;
using OperationId = StrongUuid<OperationIdTag>;
using ContinuityOperationId = StrongUuid<ContinuityOperationIdTag>;
using ContinuityHandoffId = StrongUuid<ContinuityHandoffIdTag>;
using ClientId = OpaqueId<ClientIdTag>;
using AgentId = OpaqueId<AgentIdTag>;
using AdapterId = OpaqueId<AdapterIdTag>;
using ProviderSessionId = OpaqueId<ProviderSessionIdTag>;
using EvidenceId = OpaqueId<EvidenceIdTag>;
using DeploymentId = OpaqueId<DeploymentIdTag>;
using CorrelationId = OpaqueId<CorrelationIdTag>;
using RequestId = OpaqueId<RequestIdTag>;
using LegacyHandoffId = OpaqueId<LegacyHandoffIdTag>;

} // namespace ForgeConductor::Domain
