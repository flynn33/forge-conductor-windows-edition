#include "ForgeConductor/Infrastructure/Windows/WindowsReviewerRunStore.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "Detail/OperationContextGuard.h"
#include "Detail/UniqueHandle.h"
#include "Detail/UtfConversion.h"
#include "Detail/WindowsPathResolver.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
struct Failure final { Domain::Error error; };
[[noreturn]] void reject(std::string_view code, std::string message) { throw Failure{Domain::makeError(code, std::move(message))}; }
template<class T> T take(Domain::Result<T> result) { if (!result) throw Failure{result.error()}; return std::move(result).value(); }
void take(Domain::Result<void> result) { if (!result) throw Failure{result.error()}; }
void check(const Domain::OperationContext& context) {
    take(Detail::validateOperationContext(context, std::chrono::steady_clock::now(), "persist reviewer receipt"));
}
std::span<const std::byte> bytes(std::string_view value) { return {reinterpret_cast<const std::byte*>(value.data()), value.size()}; }
std::int64_t nanoseconds(Domain::UtcTimePoint value) { return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count(); }
std::string string(const Json& value, std::size_t maximum = WindowsReviewerRunStore::MaximumRecordBytes) {
    if (!value.is_string()) reject(Domain::ErrorCodes::IntegrityFailure, "A reviewer receipt text field has the wrong type.");
    auto text = value.get<std::string>();
    if (text.size() > maximum || text.find('\0') != std::string::npos || !Domain::isValidUtf8(text))
        reject(Domain::ErrorCodes::IntegrityFailure, "A reviewer receipt text field is invalid or excessive.");
    return text;
}
std::uint64_t unsignedValue(const Json& value) {
    if (!value.is_number_unsigned()) reject(Domain::ErrorCodes::IntegrityFailure, "A reviewer receipt count has the wrong type.");
    return value.get<std::uint64_t>();
}
Domain::UtcTimePoint time(const Json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())))
        reject(Domain::ErrorCodes::IntegrityFailure, "A reviewer receipt timestamp is invalid.");
    return Domain::UtcTimePoint{std::chrono::duration_cast<Domain::UtcTimePoint::duration>(std::chrono::nanoseconds{value.get<std::int64_t>()})};
}
bool terminal(Domain::ManagedRunState state) {
    return state == Domain::ManagedRunState::Completed || state == Domain::ManagedRunState::Failed || state == Domain::ManagedRunState::Cancelled;
}
Json encodeRecord(const Domain::ManagedRunRecord& record) {
    if (!record.readOnlyTools) reject(Domain::ErrorCodes::Unauthorized, "Only independently started read-only reviewer records may use this store.");
    if (record.task.empty() || record.task.size() > Domain::MaximumManagedRunTaskBytes || record.authorityGeneration == 0U ||
        (record.outputText && record.outputText->size() > Domain::MaximumManagedRunOutputBytes) ||
        record.pendingFunctionCalls.size() > 64U || record.nativeTaskChecks.size() > 8U ||
        static_cast<std::uint32_t>(record.state) > static_cast<std::uint32_t>(Domain::ManagedRunState::Paused))
        reject(Domain::ErrorCodes::LimitExceeded, "Reviewer receipt fields exceed their logical bounds.");
    Json value{{"run_id", record.runId.value()}, {"project_id", record.projectId.value()}, {"client_id", record.clientId.value()},
        {"task", record.task}, {"authority_generation", record.authorityGeneration}, {"state", static_cast<std::uint32_t>(record.state)},
        {"provider_response_id", record.providerResponseId ? Json(record.providerResponseId->value()) : Json(nullptr)},
        {"input_tokens", record.inputTokens}, {"output_tokens", record.outputTokens},
        {"retained_context_tokens", record.retainedContextTokens ? Json(*record.retainedContextTokens) : Json(nullptr)},
        {"output_text", record.outputText ? Json(*record.outputText) : Json(nullptr)}, {"output_truncated", record.outputTruncated},
        {"last_error", nullptr}, {"pending_function_calls", Json::array()}, {"native_task_checks", Json::array()},
        {"created_at_utc_ns", nanoseconds(record.createdAt)}, {"updated_at_utc_ns", nanoseconds(record.updatedAt)},
        {"allow_tools", record.allowTools}, {"read_only_tools", record.readOnlyTools}};
    if (record.lastError) value["last_error"] = Json{{"code", record.lastError->code}, {"message", record.lastError->message},
        {"retryable", record.lastError->retryable}, {"evidence_id", record.lastError->evidenceId ? Json(*record.lastError->evidenceId) : Json(nullptr)}};
    for (const auto& call : record.pendingFunctionCalls)
        value["pending_function_calls"].push_back(Json{{"call_id", call.callId}, {"name", call.name}, {"arguments", call.canonicalArguments}});
    for (const auto& result : record.nativeTaskChecks)
        value["native_task_checks"].push_back(Json{{"command_digest", result.commandDigest.value()}, {"stdout_digest", result.stdoutDigest.value()},
            {"stderr_digest", result.stderrDigest.value()}, {"exit_code", result.exitCode}, {"passed", result.passed},
            {"timed_out", result.timedOut}, {"cancelled", result.cancelled}, {"termination_confirmed", result.terminationConfirmed},
            {"elapsed_ms", result.elapsedMilliseconds}, {"checked_at_utc_ns", nanoseconds(result.checkedAt)}});
    return value;
}
Domain::ManagedRunRecord decodeRecord(const Json& value) {
    if (!value.is_object() || !value.at("read_only_tools").is_boolean() || !value.at("read_only_tools").get<bool>())
        reject(Domain::ErrorCodes::IntegrityFailure, "The stored receipt is not a read-only reviewer.");
    Domain::ManagedRunRecord record{take(Domain::SessionId::parse(string(value.at("run_id"), 36U))),
        take(Domain::ProjectId::parse(string(value.at("project_id"), 36U))),
        take(Domain::ClientId::parse(string(value.at("client_id"), 128U))), string(value.at("task"), Domain::MaximumManagedRunTaskBytes),
        unsignedValue(value.at("authority_generation"))};
    const auto state = unsignedValue(value.at("state"));
    if (state > static_cast<std::uint64_t>(Domain::ManagedRunState::Paused)) reject(Domain::ErrorCodes::IntegrityFailure, "Reviewer state is invalid.");
    record.state = static_cast<Domain::ManagedRunState>(state);
    if (!value.at("provider_response_id").is_null()) record.providerResponseId = take(Domain::ProviderSessionId::parse(string(value.at("provider_response_id"), 512U), 512U));
    record.inputTokens = unsignedValue(value.at("input_tokens"));
    record.outputTokens = unsignedValue(value.at("output_tokens"));
    if (!value.at("retained_context_tokens").is_null()) record.retainedContextTokens = unsignedValue(value.at("retained_context_tokens"));
    if (!value.at("output_text").is_null()) record.outputText = string(value.at("output_text"), Domain::MaximumManagedRunOutputBytes);
    record.outputTruncated = value.at("output_truncated").get<bool>();
    if (!value.at("last_error").is_null()) {
        const auto& error = value.at("last_error");
        record.lastError = Domain::makeError(string(error.at("code"), 128U), string(error.at("message")), error.at("retryable").get<bool>());
        if (!error.at("evidence_id").is_null()) record.lastError->evidenceId = string(error.at("evidence_id"));
    }
    const auto& pending = value.at("pending_function_calls");
    const auto& checks = value.at("native_task_checks");
    if (!pending.is_array() || pending.size() > 64U || !checks.is_array() || checks.size() > 8U)
        reject(Domain::ErrorCodes::IntegrityFailure, "Reviewer pending calls or native checks exceed their bound.");
    for (const auto& call : pending) record.pendingFunctionCalls.push_back({string(call.at("call_id"), 512U),
        string(call.at("name"), 128U), string(call.at("arguments"))});
    for (const auto& result : checks) record.nativeTaskChecks.push_back({take(Domain::Sha256Digest::parse(string(result.at("command_digest"), 64U))),
        take(Domain::Sha256Digest::parse(string(result.at("stdout_digest"), 64U))), take(Domain::Sha256Digest::parse(string(result.at("stderr_digest"), 64U))),
        result.at("exit_code").get<int>(), result.at("passed").get<bool>(), result.at("timed_out").get<bool>(),
        result.at("cancelled").get<bool>(), result.at("termination_confirmed").get<bool>(), unsignedValue(result.at("elapsed_ms")), time(result.at("checked_at_utc_ns"))});
    record.createdAt = time(value.at("created_at_utc_ns")); record.updatedAt = time(value.at("updated_at_utc_ns"));
    record.allowTools = value.at("allow_tools").get<bool>(); record.readOnlyTools = true;
    static_cast<void>(encodeRecord(record));
    return record;
}
class StoreLock final {
public:
    StoreLock(std::string_view name, const Domain::OperationContext& context) {
        const std::wstring native{name.begin(), name.end()};
        handle_.reset(::CreateMutexW(nullptr, FALSE, native.c_str()));
        if (!handle_) reject(Domain::ErrorCodes::InternalFailure, "Reviewer receipt mutex could not be created.");
        for (;;) {
            check(context);
            const DWORD result = ::WaitForSingleObject(handle_.get(), 200U);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { held_ = true; break; }
            if (result != WAIT_TIMEOUT) reject(Domain::ErrorCodes::InternalFailure, "Reviewer receipt mutex wait failed.");
        }
    }
    ~StoreLock() { if (held_) static_cast<void>(::ReleaseMutex(handle_.get())); }
private:
    Detail::UniqueHandle handle_;
    bool held_{};
};
std::size_t countRecords(const Contracts::AuthorizedPath& directory, const Domain::OperationContext& context) {
    auto anchored = Detail::WindowsPathResolver::resolveAnchoredAuthorizedPath(directory, Domain::FileAccess::Read,
        Detail::MissingPathPolicy::Reject, Detail::AnchorSharePolicy::DenyConcurrentWrite);
    if (!anchored) {
        if (anchored.error().code == Domain::ErrorCodes::RecordNotFound) return 0U;
        throw Failure{anchored.error()};
    }
    Detail::UniqueHandle handle{::CreateFileW(anchored.value().canonicalPath().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle) reject(Domain::ErrorCodes::Unauthorized, "The private reviewer directory could not be safely opened.");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!::GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        reject(Domain::ErrorCodes::Unauthorized, "The private reviewer directory is not a regular directory.");
    std::size_t records{}, visited{};
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64U * 1024U> storage{};
    bool first = true;
    for (;;) {
        check(context);
        if (!::GetFileInformationByHandleEx(handle.get(), first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                storage.data(), static_cast<DWORD>(storage.size()))) {
            if (::GetLastError() == ERROR_NO_MORE_FILES) break;
            reject(Domain::ErrorCodes::InternalFailure, "Private reviewer receipt enumeration failed.");
        }
        first = false;
        std::size_t offset{};
        for (;;) {
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(storage.data() + offset);
            if (++visited > 1024U || entry->FileNameLength % sizeof(wchar_t) != 0U ||
                entry->FileNameLength > storage.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
                reject(Domain::ErrorCodes::LimitExceeded, "The private reviewer directory inventory exceeds its bound.");
            const std::wstring_view name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
            if (name.size() == 41U && name.ends_with(L".json")) {
                const auto identifier = take(Detail::strictUtf16ToUtf8(name.substr(0U, 36U)));
                if (Domain::SessionId::parse(identifier)) {
                    if ((entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
                        reject(Domain::ErrorCodes::IntegrityFailure, "A reviewer receipt path is not a regular file.");
                    ++records;
                }
            }
            if (entry->NextEntryOffset == 0U) break;
            if (entry->NextEntryOffset < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) || entry->NextEntryOffset > storage.size() - offset - sizeof(FILE_ID_BOTH_DIR_INFO))
                reject(Domain::ErrorCodes::IntegrityFailure, "Reviewer directory enumeration returned an invalid entry.");
            offset += entry->NextEntryOffset;
        }
    }
    take(anchored.value().revalidateDirectoryAnchors());
    return records;
}
} // namespace

class WindowsReviewerRunStore::Impl final {
public:
    Impl(Contracts::IAtomicFileStore& files, Contracts::IHasher& hasher, Contracts::IClock& clock,
        Contracts::AuthorizedPath directory, ReviewerRunStorageResolver resolver)
        : files_{files}, hasher_{hasher}, clock_{clock}, directory_{std::move(directory)}, resolver_{std::move(resolver)} {
        if (!resolver_) throw std::invalid_argument{"Reviewer storage resolver is required."};
    }
    std::string lockName() { return "Local\\ForgeConductor.Reviewer." + take(hasher_.sha256(bytes(directory_.canonicalPath().value()))).value(); }
    ReviewerRunStoragePaths paths(const Domain::SessionId& id, const Domain::OperationContext& context) {
        auto result = take(resolver_(id, context));
        auto expected = take(Detail::strictUtf8ToUtf16(directory_.canonicalPath().value()));
        while (expected.size() > 3U && expected.back() == L'\\') expected.pop_back();
        const auto identifier = id.value();
        expected += L"\\" + std::wstring{identifier.begin(), identifier.end()} + L".json";
        for (const auto* path : {&result.read, &result.write, &result.create}) {
            const auto actual = take(Detail::strictUtf8ToUtf16(path->canonicalPath().value()));
            if (::CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) != CSTR_EQUAL)
                reject(Domain::ErrorCodes::Unauthorized, "Reviewer receipt paths must be UUID files directly below the private store directory.");
        }
        return result;
    }
    std::optional<Domain::ManagedRunRecord> read(const Domain::SessionId& id, const ReviewerRunStoragePaths& paths,
        const Domain::OperationContext& context) {
        auto stored = files_.read(paths.read, MaximumRecordBytes, context);
        if (!stored) { if (stored.error().code == Domain::ErrorCodes::RecordNotFound) return std::nullopt; throw Failure{stored.error()}; }
        try {
            if (stored.value().empty()) reject(Domain::ErrorCodes::IntegrityFailure, "The reviewer receipt is empty.");
            const auto envelope = Json::parse(reinterpret_cast<const char*>(stored.value().data()),
                reinterpret_cast<const char*>(stored.value().data()) + stored.value().size());
            if (!envelope.is_object() || envelope.size() != 4U || envelope.at("schema_version") != 1U ||
                envelope.at("kind") != "forge_readonly_reviewer") reject(Domain::ErrorCodes::IntegrityFailure, "The receipt kind or schema is not a supported reviewer.");
            const auto encoded = envelope.at("record").dump();
            const auto seal = take(Domain::Sha256Digest::parse(string(envelope.at("sha256"), 64U)));
            if (take(hasher_.sha256(bytes(encoded))) != seal) reject(Domain::ErrorCodes::IntegrityFailure, "The reviewer receipt was altered; it was preserved.");
            auto record = decodeRecord(envelope.at("record"));
            if (record.runId != id) reject(Domain::ErrorCodes::IntegrityFailure, "The reviewer receipt run identity does not match its filename.");
            record.evidenceSeal = seal;
            record.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::Verified;
            return record;
        } catch (Failure& failure) {
            if (failure.error.code == Domain::ErrorCodes::Cancelled || failure.error.code == Domain::ErrorCodes::DeadlineExceeded) throw;
            reject(Domain::ErrorCodes::IntegrityFailure, "The reviewer receipt is invalid or altered; it was preserved.");
        } catch (...) { reject(Domain::ErrorCodes::IntegrityFailure, "The reviewer receipt is malformed; it was preserved."); }
    }
    void write(const Domain::ManagedRunRecord& record, const ReviewerRunStoragePaths& paths, bool exists,
        const Domain::OperationContext& context) {
        const auto value = encodeRecord(record);
        // Decode validation prevents invalid UTF-8/NULs and typed-field drift
        // from ever becoming a readable persisted reviewer receipt.
        static_cast<void>(decodeRecord(value));
        Json envelope{{"schema_version", 1U}, {"kind", "forge_readonly_reviewer"}, {"record", value},
            {"sha256", take(hasher_.sha256(bytes(value.dump()))).value()}};
        const auto encoded = envelope.dump();
        if (encoded.size() > MaximumRecordBytes) reject(Domain::ErrorCodes::PayloadTooLarge, "Reviewer receipt exceeds its 4 MiB encoded bound.");
        check(context);
        take(files_.replace(exists ? paths.write : paths.create, bytes(encoded), false, context));
    }
    Contracts::IAtomicFileStore& files_;
    Contracts::IHasher& hasher_;
    Contracts::IClock& clock_;
    Contracts::AuthorizedPath directory_;
    ReviewerRunStorageResolver resolver_;
};
WindowsReviewerRunStore::WindowsReviewerRunStore(Contracts::IAtomicFileStore& files, Contracts::IHasher& hasher,
    Contracts::IClock& clock, Contracts::AuthorizedPath directory, ReviewerRunStorageResolver resolver)
    : implementation_{std::make_unique<Impl>(files, hasher, clock, std::move(directory), std::move(resolver))} {}
WindowsReviewerRunStore::~WindowsReviewerRunStore() = default;
Domain::Result<std::optional<Domain::ManagedRunRecord>> WindowsReviewerRunStore::load(
    const Domain::SessionId& id, const Domain::OperationContext& context) noexcept {
    try {
        check(context); auto& impl = *implementation_; StoreLock lock{impl.lockName(), context};
        auto paths = impl.paths(id, context); auto record = impl.read(id, paths, context);
        if (record && (!terminal(record->state) || (record->state == Domain::ManagedRunState::Completed && !record->pendingFunctionCalls.empty()))) {
            const auto previousError = record->lastError;
            record->state = Domain::ManagedRunState::Failed;
            record->lastError = Domain::makeError(Domain::ErrorCodes::Conflict,
                "Reviewer owner stopped before a final verified result. Outcome is unknown; pending calls are retained as evidence and will not be replayed." +
                (previousError ? " Previous diagnostic: " + previousError->code + ": " + previousError->message : std::string{}), false, previousError ? previousError->evidenceId : std::nullopt);
            record->updatedAt = impl.clock_.utcNow();
            impl.write(*record, paths, true, context);
            record = impl.read(id, paths, context);
        }
        return Domain::Result<std::optional<Domain::ManagedRunRecord>>::success(std::move(record));
    } catch (Failure& failure) { return Domain::Result<std::optional<Domain::ManagedRunRecord>>::failure(std::move(failure.error)); }
    catch (...) { return Domain::Result<std::optional<Domain::ManagedRunRecord>>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Reviewer receipt load failed safely.")); }
}
Domain::Result<void> WindowsReviewerRunStore::save(const Domain::ManagedRunRecord& record,
    const Domain::OperationContext& context) noexcept {
    try {
        check(context); static_cast<void>(encodeRecord(record)); auto& impl = *implementation_; StoreLock lock{impl.lockName(), context};
        const auto paths = impl.paths(record.runId, context); const auto existing = impl.read(record.runId, paths, context);
        if (existing && (existing->projectId != record.projectId || existing->clientId != record.clientId || existing->task != record.task ||
                existing->authorityGeneration != record.authorityGeneration || existing->allowTools != record.allowTools))
            reject(Domain::ErrorCodes::OwnershipConflict, "Reviewer run identity is already bound to another request.");
        if (existing && terminal(existing->state) && !terminal(record.state))
            reject(Domain::ErrorCodes::Conflict, "A terminal reviewer receipt cannot be reopened or replayed.");
        if (!existing && countRecords(impl.directory_, context) >= MaximumRetainedRecords)
            reject(Domain::ErrorCodes::LimitExceeded, "Reviewer receipt capacity is sixteen records. The owner must archive/delete retained receipts before starting another reviewer.");
        impl.write(record, paths, existing.has_value(), context);
        return Domain::Result<void>::success();
    } catch (Failure& failure) { return Domain::Result<void>::failure(std::move(failure.error)); }
    catch (...) { return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "Reviewer receipt save failed safely.")); }
}
} // namespace ForgeConductor::Infrastructure::Windows
