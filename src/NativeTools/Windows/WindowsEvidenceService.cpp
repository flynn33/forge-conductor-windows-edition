#include "ForgeConductor/NativeTools/Windows/WindowsEvidenceService.h"
#include "NativeFileOperations.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UniqueBCryptHandle.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {
namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
using Json = nlohmann::json;
const std::string EmptyHead(64U, '0');
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(std::string{code}, std::move(message))};
}
template<class T> T take(Domain::Result<T> value) {
    if (!value) throw Failure{std::move(value).error()};
    return std::move(value).value();
}
void take(Domain::Result<void> value) {
    if (!value) throw Failure{std::move(value).error()};
}
void check(const Domain::OperationContext& context) {
    take(InfrastructureDetail::validateOperationContext(context, std::chrono::steady_clock::now(), "capture evidence"));
}
std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
std::string hex(std::span<const unsigned char> value) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const auto item : value) { result.push_back(alphabet[item >> 4U]); result.push_back(alphabet[item & 15U]); }
    return result;
}
std::uint64_t sizeOf(const BY_HANDLE_FILE_INFORMATION& value) {
    return (static_cast<std::uint64_t>(value.nFileSizeHigh) << 32U) | value.nFileSizeLow;
}
Json digestFile(const Contracts::AuthorizedPath& path, const Domain::OperationContext& context) {
    auto opened = take(Detail::openAuthorizedObject(path, Domain::FileAccess::Read,
        InfrastructureDetail::MissingPathPolicy::Reject, context,
        FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ));
    if (opened.isDirectory()) fail(Domain::ErrorCodes::InvalidRequest, "Evidence paths must name regular files.");
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!::GetFileInformationByHandle(opened.handle.get(), &before))
        throw Failure{Detail::nativeFileError("inspect evidence file", ::GetLastError())};
    if (sizeOf(before) > WindowsEvidenceService::MaximumFileBytes)
        fail(Domain::ErrorCodes::PayloadTooLarge, "An evidence file exceeds the 64 GiB streamed capture limit.");
    BCRYPT_ALG_HANDLE rawAlgorithm{};
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        fail(Domain::ErrorCodes::InternalFailure, "Evidence SHA-256 initialization failed.");
    InfrastructureDetail::UniqueBCryptAlgorithmHandle algorithm{rawAlgorithm};
    ULONG objectSize{}, returned{};
    if (!BCRYPT_SUCCESS(::BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0)) ||
        returned != sizeof(objectSize) || objectSize == 0U || objectSize > 1024U * 1024U)
        fail(Domain::ErrorCodes::InternalFailure, "Evidence SHA-256 state is invalid.");
    std::vector<unsigned char> object(objectSize);
    BCRYPT_HASH_HANDLE rawHash{};
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm.get(), &rawHash, object.data(), objectSize, nullptr, 0, 0)))
        fail(Domain::ErrorCodes::InternalFailure, "Evidence SHA-256 state could not be created.");
    InfrastructureDetail::UniqueBCryptHashHandle hash{rawHash};
    std::array<unsigned char, 64U * 1024U> buffer{};
    std::uint64_t count{};
    for (;;) {
        check(context);
        DWORD read{};
        if (!::ReadFile(opened.handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            throw Failure{Detail::nativeFileError("read evidence file", ::GetLastError())};
        if (read == 0U) break;
        count += read;
        if (count > WindowsEvidenceService::MaximumFileBytes)
            fail(Domain::ErrorCodes::PayloadTooLarge, "An evidence file exceeds the 64 GiB streamed capture limit.");
        if (!BCRYPT_SUCCESS(::BCryptHashData(hash.get(), buffer.data(), read, 0)))
            fail(Domain::ErrorCodes::InternalFailure, "Evidence SHA-256 update failed.");
    }
    if (!::GetFileInformationByHandle(opened.handle.get(), &after))
        throw Failure{Detail::nativeFileError("recheck evidence file", ::GetLastError())};
    if (count != sizeOf(before) || sizeOf(after) != sizeOf(before) ||
        before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
        before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow ||
        ::CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0)
        fail(Domain::ErrorCodes::Conflict, "An evidence file changed while its digest was captured.");
    std::array<unsigned char, 32U> digest{};
    if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0)))
        fail(Domain::ErrorCodes::InternalFailure, "Evidence SHA-256 finalization failed.");
    return Json{{"path", path.canonicalPath().value()}, {"sha256", hex(digest)}, {"bytes", count}};
}
class CaptureLock final {
public:
    CaptureLock(std::string_view name, const Domain::OperationContext& context) {
        std::wstring native(name.begin(), name.end());
        handle_.reset(::CreateMutexW(nullptr, FALSE, native.c_str()));
        if (!handle_) throw Failure{Detail::nativeFileError("create evidence capture lock", ::GetLastError())};
        for (;;) {
            check(context);
            const DWORD waited = ::WaitForSingleObject(handle_.get(), 200U);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) { acquired_ = true; break; }
            if (waited != WAIT_TIMEOUT) throw Failure{Detail::nativeFileError("acquire evidence capture lock", ::GetLastError())};
        }
    }
    ~CaptureLock() { if (acquired_) static_cast<void>(::ReleaseMutex(handle_.get())); }
private:
    InfrastructureDetail::UniqueHandle handle_;
    bool acquired_{};
};
}

class WindowsEvidenceService::Impl final {
public:
    Impl(Contracts::IWorkspaceAuthority& authority, Contracts::IAtomicFileStore& files,
        Contracts::IHasher& hasher, Contracts::IClock& clock, Contracts::IUuidGenerator& uuids,
        Contracts::EvidenceStorageResolver storage, Domain::PathText dataRoot)
        : authority_{authority}, files_{files}, hasher_{hasher}, clock_{clock}, uuids_{uuids},
          storage_{std::move(storage)}, dataRoot_{std::move(dataRoot)} {
        if (!storage_) throw std::invalid_argument{"Evidence storage resolver is required."};
    }
    void validateAuthority(const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context, bool write) {
        check(context);
        if (authority.trustedRoots().empty()) fail(Domain::ErrorCodes::Unauthorized, "Evidence requires workspace authority.");
        static_cast<void>(take(authority_.authorize(authority, {authority.trustedRoots().front(), std::nullopt,
            write ? Domain::FileAccess::Write : Domain::FileAccess::Read, false}, context)));
    }
    std::string lockName(const Domain::ProjectId& project) {
        const auto digest = take(hasher_.sha256(bytes(dataRoot_.value())));
        return "Local\\ForgeConductor.Evidence." + digest.value() + "." + project.value();
    }
    Json load(const Contracts::EvidenceStoragePaths& paths, const Domain::ProjectId& project,
        const Domain::OperationContext& context, bool& found) {
        auto read = files_.read(paths.read, MaximumLogBytes, context);
        if (!read) {
            if (read.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{read.error()};
            found = false;
            return Json{{"schema_version", 1}, {"project_id", project.value()},
                {"head_digest", EmptyHead}, {"records", Json::array()}};
        }
        found = true;
        try {
            if (read.value().empty()) fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture log is empty; it was preserved.");
            return Json::parse(reinterpret_cast<const char*>(read.value().data()),
                reinterpret_cast<const char*>(read.value().data()) + read.value().size());
        } catch (...) { fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture log is malformed; it was preserved."); }
    }
    void verify(const Json& log, const Domain::ProjectId& project,
        const Domain::OperationContext& context, bool hashes) {
        try {
            if (!log.is_object() || log.at("schema_version") != 1 ||
                log.at("project_id") != project.value() || !log.at("records").is_array())
                fail(Domain::ErrorCodes::IntegrityFailure, "Evidence log identity is invalid.");
            std::string previous = EmptyHead;
            std::size_t sequence{};
            for (const auto& record : log.at("records")) {
                check(context);
                if (!record.is_object() || record.at("schema_version") != 1 ||
                    record.at("project_id") != project.value() || record.at("sequence") != ++sequence ||
                    record.at("previous_digest") != previous || !record.at("files").is_array() ||
                    record.at("files").empty() || record.at("files").size() > MaximumPaths)
                    fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture chain is inconsistent.");
                const auto expected = record.at("digest").get<std::string>();
                if (expected.size() != 64U || expected.find_first_not_of("0123456789abcdef") != std::string::npos)
                    fail(Domain::ErrorCodes::IntegrityFailure, "An evidence capture digest is invalid.");
                if (hashes) {
                    auto payload = record;
                    payload.erase("digest");
                    if (take(hasher_.sha256(bytes(payload.dump()))).value() != expected)
                        fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture log was altered; its bytes were preserved.");
                }
                previous = expected;
            }
            if (log.at("head_digest") != previous)
                fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture head does not match its chain.");
        } catch (Failure&) { throw; }
        catch (...) { fail(Domain::ErrorCodes::IntegrityFailure, "The evidence capture log structure is invalid; it was preserved."); }
    }
    Contracts::IWorkspaceAuthority& authority_;
    Contracts::IAtomicFileStore& files_;
    Contracts::IHasher& hasher_;
    Contracts::IClock& clock_;
    Contracts::IUuidGenerator& uuids_;
    Contracts::EvidenceStorageResolver storage_;
    Domain::PathText dataRoot_;
};

WindowsEvidenceService::WindowsEvidenceService(Contracts::IWorkspaceAuthority& authority,
    Contracts::IAtomicFileStore& files, Contracts::IHasher& hasher, Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuids, Contracts::EvidenceStorageResolver storage, Domain::PathText dataRoot)
    : implementation_{std::make_unique<Impl>(authority, files, hasher, clock, uuids, std::move(storage), std::move(dataRoot))} {}
WindowsEvidenceService::~WindowsEvidenceService() = default;

Domain::Result<std::string> WindowsEvidenceService::digest(
    const std::vector<Domain::PathText>& paths, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept {
    try {
        auto& impl = *implementation_;
        impl.validateAuthority(authority, context, true);
        if (paths.empty() || paths.size() > MaximumPaths)
            fail(Domain::ErrorCodes::LimitExceeded, "Evidence capture requires one to 64 paths.");
        std::size_t pathBytes{}, canonicalPathBytes{};
        Json entries = Json::array();
        std::vector<std::wstring> canonicalPaths;
        std::uint64_t total{};
        for (const auto& path : paths) {
            check(context);
            pathBytes += path.value().size();
            if (pathBytes > MaximumPathBytes)
                fail(Domain::ErrorCodes::PayloadTooLarge, "Evidence path names exceed the 32 KiB capture limit.");
            auto authorized = take(impl.authority_.authorize(authority, {path, std::nullopt,
                Domain::FileAccess::Read, false}, context));
            canonicalPathBytes += authorized.canonicalPath().value().size();
            if (canonicalPathBytes > MaximumPathBytes)
                fail(Domain::ErrorCodes::PayloadTooLarge, "Canonical evidence paths exceed the 32 KiB capture limit.");
            auto canonical = take(InfrastructureDetail::strictUtf8ToUtf16(authorized.canonicalPath().value()));
            if (std::any_of(canonicalPaths.begin(), canonicalPaths.end(), [&](const auto& item) { return Detail::samePath(item, canonical); }))
                fail(Domain::ErrorCodes::InvalidRequest, "Evidence capture does not accept duplicate file paths.");
            canonicalPaths.push_back(std::move(canonical));
            auto entry = digestFile(authorized, context);
            total += entry.at("bytes").get<std::uint64_t>();
            if (total > MaximumCaptureBytes)
                fail(Domain::ErrorCodes::PayloadTooLarge, "Evidence capture exceeds the 128 GiB streamed limit.");
            entries.push_back(std::move(entry));
        }
        CaptureLock lock{impl.lockName(authority.projectId()), context};
        const auto storage = take(impl.storage_(authority.projectId(), context));
        bool found{};
        auto log = impl.load(storage, authority.projectId(), context, found);
        impl.verify(log, authority.projectId(), context, true);
        const auto capture = take(impl.uuids_.next());
        Json record{{"schema_version", 1}, {"capture_id", capture.value()},
            {"project_id", authority.projectId().value()}, {"sequence", log.at("records").size() + 1U},
            {"observed_at_utc_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                impl.clock_.utcNow().time_since_epoch()).count()},
            {"previous_digest", log.at("head_digest")}, {"files", std::move(entries)}};
        record["digest"] = take(impl.hasher_.sha256(bytes(record.dump()))).value();
        log["records"].push_back(record);
        log["head_digest"] = record.at("digest");
        const auto encoded = log.dump();
        if (encoded.size() > MaximumLogBytes)
            fail(Domain::ErrorCodes::LimitExceeded, "The 8 MiB evidence log is full; existing captures were preserved.");
        take(impl.files_.replace(found ? storage.write : storage.create, bytes(encoded), false, context));
        record["ok"] = true;
        record["head_digest"] = record.at("digest");
        record["log_path"] = storage.read.canonicalPath().value();
        record["integrity"] = "verified_sha256_chain";
        record["integrity_scope"] = "Unkeyed SHA-256 chain; retain the returned digest externally to detect a complete rewrite and rehash.";
        return Domain::Result<std::string>::success(record.dump());
    } catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
        "Evidence capture failed safely.")); }
}

Domain::Result<std::string> WindowsEvidenceService::readLog(
    const Contracts::WorkspaceAuthority& authority, std::size_t offset, std::size_t limit,
    bool verify, const Domain::OperationContext& context) noexcept {
    try {
        auto& impl = *implementation_;
        impl.validateAuthority(authority, context, false);
        if (limit == 0U || limit > 16U) fail(Domain::ErrorCodes::LimitExceeded, "Evidence log pages contain one to sixteen captures.");
        CaptureLock lock{impl.lockName(authority.projectId()), context};
        const auto storage = take(impl.storage_(authority.projectId(), context));
        bool found{};
        const auto log = impl.load(storage, authority.projectId(), context, found);
        impl.verify(log, authority.projectId(), context, verify);
        const auto total = log.at("records").size();
        if (offset > total) fail(Domain::ErrorCodes::InvalidRequest, "Evidence log offset exceeds its capture count.");
        Json page{{"ok", true}, {"found", found}, {"project_id", authority.projectId().value()},
            {"log_path", storage.read.canonicalPath().value()}, {"head_digest", log.at("head_digest")},
            {"total", total}, {"offset", offset}, {"entries", Json::array()},
            {"integrity", verify ? "verified_sha256_chain" : "not_verified"},
            {"integrity_scope", "Unkeyed chain; compare a retained external capture digest to detect complete rewrite and rehash."}};
        std::size_t next = offset;
        while (next < total && page.at("entries").size() < limit) {
            page["entries"].push_back(log.at("records").at(next));
            if (page.dump().size() > MaximumPageBytes - 1024U) {
                page["entries"].erase(page["entries"].end() - 1);
                if (page.at("entries").empty()) fail(Domain::ErrorCodes::PayloadTooLarge, "One evidence capture exceeds the log page budget.");
                break;
            }
            ++next;
        }
        page["complete"] = next == total;
        page["next_offset"] = next == total ? Json(nullptr) : Json(next);
        return Domain::Result<std::string>::success(page.dump());
    } catch (Failure& error) { return Domain::Result<std::string>::failure(std::move(error.error)); }
    catch (...) { return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,
        "The evidence capture log could not be read safely.")); }
}
} // namespace ForgeConductor::NativeTools::Windows
