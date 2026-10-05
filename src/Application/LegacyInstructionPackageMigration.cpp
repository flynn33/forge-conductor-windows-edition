#include "ForgeConductor/Application/LegacyInstructionPackageMigration.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {
[[nodiscard]] Domain::Error error(const std::string_view code, const char* message)
{
    return Domain::makeError(code, message);
}

[[nodiscard]] std::span<const std::byte> byteView(const std::string& value) noexcept
{
    return std::as_bytes(std::span{value.data(), value.size()});
}
} // namespace

Domain::Result<std::optional<Domain::ProjectMemoryRecord>>
migrateLegacyInstructionPackage(
    Contracts::IProjectMemoryService& memory,
    Contracts::IHasher& hasher,
    Contracts::IClock& clock,
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto legacyPage = memory.listRecent(
            Domain::ListRecentProjectMemoryRequest{
                projectId, {"instruction_package"}, std::nullopt,
                100U, std::nullopt, true, 256U * 1024U}, context);
        if (!legacyPage) {
            return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                std::move(legacyPage).error());
        }
        for (const auto& legacyHit : legacyPage.value().records) {
            if (legacyHit.record.projectId != projectId ||
                legacyHit.record.kind != "instruction_package" ||
                legacyHit.record.isTombstone || !legacyHit.record.body) continue;
            try {
                const auto legacy = nlohmann::json::parse(*legacyHit.record.body);
                if (legacy.value("schema", std::string{}) !=
                    "forge-instruction-package-v1") continue;
                const auto revisionText = legacy.at("revision").get<std::string>();
                auto revision = Domain::Sha256Digest::parse(revisionText);
                auto path = Domain::PathText::create(
                    legacy.at("package_path").get<std::string>());
                if (!revision || !path || !legacy.at("files").is_array()) continue;
                auto packageIdentity = hasher.sha256(
                    byteView(std::string{"package-root\n"} + projectId.value() +
                        "\n" + path.value().value()));
                if (!packageIdentity) {
                    return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                        std::move(packageIdentity).error());
                }
                const auto packageId = packageIdentity.value().value();
                auto rowIdentity = hasher.sha256(
                    byteView(std::string{"package-row\n"} + projectId.value() +
                        "\n" + packageId + "\n" + revisionText));
                if (!rowIdentity) {
                    return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                        std::move(rowIdentity).error());
                }
                const auto rowId = "queue-" +
                    rowIdentity.value().value().substr(0U, 32U);
                std::vector<Domain::MemoryRecordId> legacyIds;
                for (const auto& file : legacy.at("files")) {
                    auto id = Domain::MemoryRecordId::parse(
                        file.at("record_id").get<std::string>());
                    if (!id) continue;
                    legacyIds.push_back(std::move(id).value());
                }
                std::map<std::string, Domain::ProjectMemoryRecord> byId;
                for (const auto& id : legacyIds) {
                    auto legacyRecords = memory.get(
                        Domain::GetProjectMemoryRequest{
                            projectId, {id}, true, 256U * 1024U}, context);
                    if (!legacyRecords) {
                        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                            std::move(legacyRecords).error());
                    }
                    for (auto& record : legacyRecords.value().records) {
                        if (record.projectId != projectId || record.isTombstone) continue;
                        byId.emplace(record.id.value(), std::move(record));
                    }
                }
                std::vector<Domain::ProjectMemoryWrite> entryWrites;
                std::uint64_t contentBytes{};
                std::uint64_t coverageGaps{};
                std::size_t fileIndex{};
                for (const auto& file : legacy.at("files")) {
                    const auto recordId = file.at("record_id").get<std::string>();
                    const auto found = byId.find(recordId);
                    const bool readable = found != byId.end() && found->second.body.has_value();
                    if (!readable) ++coverageGaps;
                    const auto body = readable
                        ? *found->second.body : std::string{};
                    auto contentHash = hasher.sha256(
                        byteView(body));
                    if (!contentHash) {
                        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                            std::move(contentHash).error());
                    }
                    const auto relative = file.at("path").get<std::string>();
                    nlohmann::json entry{{"schema", "forge-instruction-package-entry-v2"},
                        {"package_id", packageId}, {"queue_row_id", rowId},
                        {"revision", revisionText}, {"relative_path", relative},
                        {"kind", "file"}, {"byte_length", body.size()},
                        {"content_hash", readable ? nlohmann::json(contentHash.value().value()) : nlohmann::json(nullptr)},
                        {"interpretation", readable ? "interpreted" : "unreadable"},
                        {"coverage_detail", readable
                            ? "Migrated from the active single-manifest format."
                            : "The pinned legacy file is missing or its text is unavailable."},
                        {"derived_text", readable ? nlohmann::json(body) : nlohmann::json(nullptr)},
                        {"legacy_record_id", recordId}};
                    auto key = Domain::IdempotencyKey::create(
                        "instruction-migrated-entry:" + rowId + ":" +
                        std::to_string(fileIndex++));
                    if (!key) {
                        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                            std::move(key).error());
                    }
                    Domain::ProjectMemoryWrite write;
                    write.kind = "instruction_package_entry";
                    write.title = relative;
                    write.summary = packageId + " · migrated · " +
                        std::to_string(body.size()) + " bytes";
                    write.body = entry.dump();
                    write.tags = {"instruction-package-entry", "migrated-v1"};
                    write.importance = 1.0;
                    write.confidence = readable ? 1.0 : 0.0;
                    write.sourceKind = "manager_instruction_package_migration";
                    write.sourceReference = recordId;
                    write.idempotencyKey = std::move(key).value();
                    entryWrites.push_back(std::move(write));
                    contentBytes += body.size();
                }
                constexpr std::size_t PersistencePageEntries = 5U;
                const auto maximumBatchBytes = Domain::ProjectMemoryLimits{}.maximumBatchBytes;
                for (std::size_t begin{}; begin < entryWrites.size();) {
                    std::vector<Domain::ProjectMemoryWrite> page;
                    std::size_t pageBytes{};
                    while (begin < entryWrites.size() && page.size() < PersistencePageEntries) {
                        const auto& entry = entryWrites[begin];
                        const auto entryBytes = entry.title.size() + entry.summary.size() +
                            (entry.body ? entry.body->size() : 0U);
                        if (!page.empty() && entryBytes > maximumBatchBytes - pageBytes) break;
                        if (entryBytes > maximumBatchBytes) {
                            return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                                error(Domain::ErrorCodes::PayloadTooLarge,
                                    "A legacy instruction entry exceeds its persistence batch limit."));
                        }
                        pageBytes += entryBytes;
                        page.push_back(std::move(entryWrites[begin++]));
                    }
                    auto written = memory.rememberBatch(
                        Domain::RememberProjectMemoryBatchRequest{
                            projectId, std::move(page)}, context);
                    if (!written) {
                        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                            std::move(written).error());
                    }
                }
                nlohmann::json migrated{{"schema", "forge-instruction-package-queue-v2"},
                    {"project_id", projectId.value()}, {"queue_row_id", rowId},
                    {"package_id", packageId},
                    {"package_name", legacy.value("package_name", std::string{"Migrated instructions"})},
                    {"package_path", path.value().value()}, {"revision", revisionText},
                    {"order", 1024U}, {"state", coverageGaps == 0U ? "active" : "needs_attention"},
                    {"cursor", {{"entry", 0U}, {"byte_offset", 0U}}},
                    {"entry_count", legacy.at("files").size()},
                    {"content_bytes", contentBytes}, {"coverage_gap_count", coverageGaps},
                    {"attempts", 1U},
                    {"last_error", "Execution cursor was uncertain during migration and was reset to the first entry."},
                    {"migrated_from_record_id", legacyHit.record.id.value()}};
                auto key = Domain::IdempotencyKey::create("instruction-queue:" + rowId);
                if (!key) {
                    return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                        std::move(key).error());
                }
                Domain::ProjectMemoryWrite write;
                write.kind = "instruction_package_queue";
                write.title = legacy.value("package_name", std::string{"Migrated instructions"});
                write.summary = "Migrated active instruction manifest · " + revisionText;
                write.body = migrated.dump();
                write.tags = {"instruction-package-queue", "migrated-v1"};
                write.importance = 1.0;
                write.confidence = 1.0;
                write.sourceKind = "manager_instruction_package_migration";
                write.sourceReference = legacyHit.record.id.value();
                write.idempotencyKey = std::move(key).value();
                auto saved = memory.remember(
                    Domain::RememberProjectMemoryRequest{projectId, std::move(write)},
                    context);
                if (!saved) {
                    return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                        std::move(saved).error());
                }
                if (saved.value().disposition == Domain::MemoryWriteDisposition::Deduplicated) {
                    auto current = memory.get(Domain::GetProjectMemoryRequest{
                        projectId, {saved.value().recordId}, true, 256U * 1024U}, context);
                    if (!current) return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                        std::move(current).error());
                    for (auto& record : current.value().records) {
                        if (record.id != saved.value().recordId) continue;
                        if (record.isTombstone) break;
                        if (record.projectId != projectId ||
                            record.kind != "instruction_package_queue" || !record.body) {
                            return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                                error(Domain::ErrorCodes::IntegrityFailure,
                                    "The deduplicated instruction queue record is invalid."));
                        }
                        const auto persisted = nlohmann::json::parse(*record.body);
                        if (persisted.at("schema") != "forge-instruction-package-queue-v2" ||
                            persisted.at("project_id") != projectId.value() ||
                            persisted.at("queue_row_id") != rowId ||
                            persisted.at("revision") != revisionText) {
                            return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                                error(Domain::ErrorCodes::IntegrityFailure,
                                    "The deduplicated instruction queue identity is invalid."));
                        }
                        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::success(
                            std::move(record));
                    }
                    return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::success(std::nullopt);
                }
                const auto now = clock.utcNow();
                auto migratedRecord = Domain::ProjectMemoryRecord{
                    saved.value().recordId, projectId,
                    saved.value().recordVersion,
                    "instruction_package_queue",
                    legacy.value("package_name",
                        std::string{"Migrated instructions"}),
                    "Migrated active instruction manifest · " + revisionText,
                    migrated.dump(),
                    {"instruction-package-queue", "migrated-v1"},
                    1.0, 1.0,
                    "manager_instruction_package_migration",
                    legacyHit.record.id.value(), std::nullopt,
                    now, now, now, std::nullopt,
                    saved.value().contentHash, false,
                    saved.value().schemaVersion};
                return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::success(
                    std::move(migratedRecord));
            } catch (...) {
                return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
                    error(Domain::ErrorCodes::IntegrityFailure,
                        "The legacy instruction package failed validation."));
            }
        }
        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::success(std::nullopt);
    } catch (...) {
        return Domain::Result<std::optional<Domain::ProjectMemoryRecord>>::failure(
            error(Domain::ErrorCodes::InternalFailure,
                "The legacy instruction package could not be migrated."));
    }
}

} // namespace ForgeConductor::Application
