#include "LMStudioChatCheckpoint.h"
#include "Detail/OperationContextGuard.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include <wincrypt.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {
using Json = nlohmann::json;
constexpr std::string_view Prefix = "ForgeVisibleCheckpoint1\n";
constexpr std::string_view Entropy = "ForgeConductor/LMStudio/visible-chat/v1";
std::filesystem::path native(const std::string_view value) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
Domain::PathText pathText(const std::filesystem::path& value) {
    const auto text = value.lexically_normal().make_preferred().u8string();
    auto parsed = Domain::PathText::create(std::string{reinterpret_cast<const char*>(text.data()), text.size()});
    if (!parsed) throw std::runtime_error{parsed.error().message};
    return std::move(parsed).value();
}
Domain::Error invalid(const std::string& message) {
    return Domain::makeError(Domain::ErrorCodes::IntegrityFailure, message);
}
bool validEnvelope(const Json& value) {
    return value.is_object() && value.size()==5U && value.contains("schema_version") &&
        value.at("schema_version").is_number_unsigned() && value.at("schema_version")==1U &&
        value.contains("source_contract") && value.at("source_contract").is_string() &&
        value.at("source_contract").get<std::string>()==LMStudioChatCheckpoint::SourceContract &&
        value.contains("scope") && value.at("scope").is_object() && value.contains("revision") &&
        value.at("revision").is_number_unsigned() && value.at("revision").get<std::uint64_t>()!=0U &&
        value.contains("state") && value.at("state").is_object();
}
bool routeOnlyDifference(Json saved, Json current) {
    if(!saved.is_object() || !current.is_object() || !saved.contains("routing_sha256") || !current.contains("routing_sha256")) return false;
    const auto digest=[](const Json& value) {
        return value.is_string() && value.get_ref<const std::string&>().size()==64U &&
            std::all_of(value.get_ref<const std::string&>().begin(),value.get_ref<const std::string&>().end(),
                [](const char c) {return (c>='0' && c<='9') || (c>='a' && c<='f');});
    };
    if(!digest(saved.at("routing_sha256")) || !digest(current.at("routing_sha256")) || saved.at("routing_sha256")==current.at("routing_sha256")) return false;
    saved.erase("routing_sha256");current.erase("routing_sha256");return saved==current;
}
struct LocalBlob final {
    DATA_BLOB value{};
    ~LocalBlob() { if (value.pbData) {::SecureZeroMemory(value.pbData, value.cbData); ::LocalFree(value.pbData);} }
};
DATA_BLOB entropy() {
    return {static_cast<DWORD>(Entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Entropy.data()))};
}
}

LMStudioChatCheckpoint::LMStudioChatCheckpoint(const Domain::PathText& home,
    const Domain::ProjectId& project, Json scope, const Domain::OperationContext& operation)
    : project_{project}, path_{pathText(native(home.value()) / "continuity" / ("lmstudio-visible-" + project.value() + ".checkpoint"))},
      scope_(std::move(scope)), authority_{{WindowsWorkspaceAuthorityPolicy{
          Domain::AuthorityId::parse(project.value()).value(), project,
          Domain::ClientId::parse("lmstudio-visible-continuity").value(), {pathText(native(home.value()))}, Domain::FileAccess::Read,
          {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create}, {}, false, 1U}}} {
    auto valid = validateOperationContext(operation, std::chrono::steady_clock::now(), "open visible chat checkpoint");
    if (!valid) throw std::runtime_error{valid.error().message};
    auto authority = authority_.authorityFor(project_, operation);
    if (!authority) throw std::runtime_error{authority.error().message};
    const auto directory = native(home.value()) / "continuity";
    auto admitted = authority_.authorize(authority.value(), {pathText(directory), std::nullopt, Domain::FileAccess::Create, true}, operation);
    if (!admitted) throw std::runtime_error{admitted.error().message};
    std::filesystem::create_directory(directory);
    directory_.reset(::CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!directory_ || !::GetFileInformationByHandle(directory_.get(), &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        throw std::runtime_error{"Visible chat checkpoint directory identity cannot be verified."};
    // Every project on this routed home shares the same UI writer. An OS handle
    // releases ownership on worker exit; no PID or persisted lock grants it.
    const auto lockPath = directory / "lmstudio-visible-writer.lock";
    auto lockAdmitted = authority_.authorize(authority.value(), {pathText(lockPath), std::nullopt, Domain::FileAccess::Create, true}, operation);
    if (!lockAdmitted) throw std::runtime_error{lockAdmitted.error().message};
    writer_.reset(::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0U,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!writer_) throw std::runtime_error{"Another visible chat observer owns the writer, or checkpoint ownership is unavailable."};
    if (!::GetFileInformationByHandle(writer_.get(), &info) || info.nNumberOfLinks != 1U ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
        throw std::runtime_error{"Visible chat checkpoint writer identity cannot be verified."};
}

Domain::Result<Contracts::AuthorizedPath> LMStudioChatCheckpoint::authorized(
    const Domain::FileAccess access, const Domain::OperationContext& operation) noexcept {
    auto authority = authority_.authorityFor(project_, operation);
    if (!authority) return Domain::Result<Contracts::AuthorizedPath>::failure(authority.error());
    return authority_.authorize(authority.value(), {path_, std::nullopt, access, true}, operation);
}

Domain::Result<std::vector<std::byte>> LMStudioChatCheckpoint::seal(const Json& document) noexcept {
    try {
        const auto plain = document.dump();
        if (plain.size() > MaximumPlainBytes) return Domain::Result<std::vector<std::byte>>::failure(
            Domain::makeError(Domain::ErrorCodes::PayloadTooLarge, "Visible chat checkpoint exceeds its bounded storage size."));
        DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
        auto salt = entropy(); LocalBlob protectedData;
        if (!::CryptProtectData(&input, L"Forge visible chat continuity", &salt, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &protectedData.value))
            return Domain::Result<std::vector<std::byte>>::failure(invalid("Visible chat checkpoint could not be sealed for the current Windows user."));
        if (Prefix.size()+protectedData.value.cbData>MaximumStoredBytes)
            return Domain::Result<std::vector<std::byte>>::failure(invalid("Sealed visible chat checkpoint exceeds its storage bound."));
        std::vector<std::byte> stored(Prefix.size() + protectedData.value.cbData);
        std::memcpy(stored.data(), Prefix.data(), Prefix.size());
        std::memcpy(stored.data() + Prefix.size(), protectedData.value.pbData, protectedData.value.cbData);
        return Domain::Result<std::vector<std::byte>>::success(std::move(stored));
    } catch (...) {return Domain::Result<std::vector<std::byte>>::failure(invalid("Visible chat checkpoint sealing failed."));}
}

Domain::Result<Json> LMStudioChatCheckpoint::open(const std::span<const std::byte> stored) noexcept {
    try {
        if (stored.size() <= Prefix.size() || stored.size() > MaximumStoredBytes ||
            std::memcmp(stored.data(), Prefix.data(), Prefix.size()) != 0)
            return Domain::Result<Json>::failure(invalid("Visible chat checkpoint envelope is malformed or oversized."));
        DATA_BLOB input{static_cast<DWORD>(stored.size() - Prefix.size()),
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(stored.data() + Prefix.size()))};
        auto salt = entropy(); LocalBlob plain;
        if (!::CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.value) ||
            plain.value.cbData > MaximumPlainBytes)
            return Domain::Result<Json>::failure(invalid("Visible chat checkpoint integrity cannot be verified for this Windows user."));
        auto document = Json::parse(plain.value.pbData, plain.value.pbData + plain.value.cbData, nullptr, false);
        if (document.is_discarded()) return Domain::Result<Json>::failure(invalid("Visible chat checkpoint plaintext is malformed."));
        return Domain::Result<Json>::success(std::move(document));
    } catch (...) {return Domain::Result<Json>::failure(invalid("Visible chat checkpoint decoding failed."));}
}

Domain::Result<std::optional<Json>> LMStudioChatCheckpoint::load(const Domain::OperationContext& operation) noexcept {
    try {
        auto path = authorized(Domain::FileAccess::Read, operation);
        if (!path) return Domain::Result<std::optional<Json>>::failure(path.error());
        auto stored = files_.read(path.value(), MaximumStoredBytes, operation);
        if (!stored) {
            if (stored.error().code == Domain::ErrorCodes::RecordNotFound)
                return Domain::Result<std::optional<Json>>::success(std::nullopt);
            return Domain::Result<std::optional<Json>>::failure(stored.error());
        }
        auto opened = open(stored.value());
        if (!opened) return Domain::Result<std::optional<Json>>::failure(opened.error());
        const auto& value = opened.value();
        if (!value.is_object() || value.size() != 5U || !value.at("schema_version").is_number_unsigned() || value.at("schema_version") != 1U ||
            !value.at("source_contract").is_string() || value.at("source_contract").get<std::string>() != SourceContract || value.at("scope") != scope_ ||
            !value.at("revision").is_number_unsigned() || value.at("revision").get<std::uint64_t>() == 0U ||
            !value.at("state").is_object())
            return Domain::Result<std::optional<Json>>::failure(invalid("Visible chat checkpoint schema, source contract or freshly authorized scope differs."));
        revision_ = value.at("revision").get<std::uint64_t>();
        return Domain::Result<std::optional<Json>>::success(std::optional<Json>{value.at("state")});
    } catch (...) {return Domain::Result<std::optional<Json>>::failure(invalid("Visible chat checkpoint schema is incomplete."));}
}

Domain::Result<void> LMStudioChatCheckpoint::save(const Json& state, const Domain::OperationContext& operation) noexcept {
    try {
        if (revision_ == (std::numeric_limits<std::uint64_t>::max)())
            return Domain::Result<void>::failure(invalid("Visible chat checkpoint revision is exhausted."));
        auto sealed = seal(Json{{"schema_version", 1U}, {"source_contract", SourceContract},
            {"scope", scope_}, {"revision", revision_ + 1U}, {"state", state}});
        if (!sealed) return Domain::Result<void>::failure(sealed.error());
        const auto access = std::filesystem::exists(native(path_.value())) ? Domain::FileAccess::Write : Domain::FileAccess::Create;
        auto path = authorized(access, operation);
        if (!path) return Domain::Result<void>::failure(path.error());
        auto result = files_.replace(path.value(), sealed.value(), false, operation);
        if (result) ++revision_;
        return result;
    } catch (...) {return Domain::Result<void>::failure(invalid("Visible chat checkpoint publication failed."));}
}

Domain::Result<LMStudioChatCheckpoint::RouteRecoverySnapshot> LMStudioChatCheckpoint::inspectRouteRecovery(
    const Domain::OperationContext& operation, const RecoveryScope recoveryScope) noexcept {
    try {
        auto path=authorized(Domain::FileAccess::Read,operation);
        if(!path) return Domain::Result<RouteRecoverySnapshot>::failure(path.error());
        auto stored=files_.read(path.value(),MaximumStoredBytes,operation);
        if(!stored) return Domain::Result<RouteRecoverySnapshot>::failure(stored.error());
        auto opened=open(stored.value());
        if(!opened) return Domain::Result<RouteRecoverySnapshot>::failure(opened.error());
        if(!validEnvelope(opened.value()) ||
            (!routeOnlyDifference(opened.value().at("scope"),scope_) &&
             (recoveryScope!=RecoveryScope::TerminalPacket || opened.value().at("scope")!=scope_)))
            return Domain::Result<RouteRecoverySnapshot>::failure(invalid("Explicit route recovery requires the same source contract and current scope except for the route digest."));
        BCryptSha256Hasher hasher;auto hash=hasher.sha256(stored.value());
        if(!hash) return Domain::Result<RouteRecoverySnapshot>::failure(hash.error());
        return Domain::Result<RouteRecoverySnapshot>::success({std::move(opened).value(),std::move(stored).value(),hash.value().value()});
    } catch (...) {return Domain::Result<RouteRecoverySnapshot>::failure(invalid("Explicit route recovery checkpoint inspection failed."));}
}

Domain::Result<Domain::PathText> LMStudioChatCheckpoint::recoverRoute(const RouteRecoverySnapshot& snapshot,
    const Json& state,const std::function<Domain::Result<void>()>& freshAuthority,
    const Domain::OperationContext& operation, const RecoveryScope recoveryScope) noexcept {
    try {
        auto current=inspectRouteRecovery(operation,recoveryScope);
        if(!current) return Domain::Result<Domain::PathText>::failure(current.error());
        if(current.value().stored!=snapshot.stored || current.value().document!=snapshot.document || current.value().sha256!=snapshot.sha256)
            return Domain::Result<Domain::PathText>::failure(invalid("Explicit route recovery checkpoint changed after inspection."));
        const auto revision=snapshot.document.at("revision").get<std::uint64_t>();
        if(revision==(std::numeric_limits<std::uint64_t>::max)() || !state.is_object() || !freshAuthority)
            return Domain::Result<Domain::PathText>::failure(invalid("Explicit route recovery publication is invalid."));
        auto sealed=seal(Json{{"schema_version",1U},{"source_contract",SourceContract},{"scope",scope_},{"revision",revision+1U},{"state",state}});
        if(!sealed) return Domain::Result<Domain::PathText>::failure(sealed.error());
        const auto archive=pathText(native(path_.value()).parent_path()/
            ("lmstudio-visible-"+project_.value()+".route-upgrade-"+std::to_string(revision)+"-"+snapshot.sha256+".archive"));
        auto authority=authority_.authorityFor(project_,operation);
        if(!authority) return Domain::Result<Domain::PathText>::failure(authority.error());
        const auto admit=[&](Domain::FileAccess access) {return authority_.authorize(authority.value(),{archive,std::nullopt,access,true},operation);};
        auto read=admit(Domain::FileAccess::Read);
        if(!read) return Domain::Result<Domain::PathText>::failure(read.error());
        auto archived=files_.read(read.value(),MaximumStoredBytes,operation);
        if(!archived && archived.error().code==Domain::ErrorCodes::RecordNotFound) {
            auto create=admit(Domain::FileAccess::Create);
            if(!create) return Domain::Result<Domain::PathText>::failure(create.error());
            auto saved=files_.replace(create.value(),snapshot.stored,false,operation);
            if(!saved) return Domain::Result<Domain::PathText>::failure(saved.error());
            archived=files_.read(read.value(),MaximumStoredBytes,operation);
        }
        if(!archived) return Domain::Result<Domain::PathText>::failure(archived.error());
        if(archived.value()!=snapshot.stored)
            return Domain::Result<Domain::PathText>::failure(invalid("Explicit route recovery archive differs from the original encrypted checkpoint; it will not be overwritten."));
        auto fresh=freshAuthority();
        if(!fresh) return Domain::Result<Domain::PathText>::failure(fresh.error());
        current=inspectRouteRecovery(operation,recoveryScope);
        if(!current) return Domain::Result<Domain::PathText>::failure(current.error());
        if(current.value().stored!=snapshot.stored)
            return Domain::Result<Domain::PathText>::failure(invalid("Explicit route recovery checkpoint changed before publication."));
        auto write=authorized(Domain::FileAccess::Write,operation);
        if(!write) return Domain::Result<Domain::PathText>::failure(write.error());
        auto published=files_.replace(write.value(),sealed.value(),false,operation);
        if(!published) return Domain::Result<Domain::PathText>::failure(published.error());
        revision_=revision+1U;
        return Domain::Result<Domain::PathText>::success(archive);
    } catch (...) {return Domain::Result<Domain::PathText>::failure(invalid("Explicit route recovery archive or publication failed; control is deferred."));}
}
} // namespace ForgeConductor::Infrastructure::Windows::Detail
