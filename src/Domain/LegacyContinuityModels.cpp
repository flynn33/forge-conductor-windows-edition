#include "ForgeConductor/Domain/LegacyContinuityModels.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace ForgeConductor::Domain {
namespace {

[[nodiscard]] Result<void> invalidValue(const std::string_view message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::InvalidRequest, std::string{message}));
}

[[nodiscard]] Result<void> integrityFailure(
    const std::string_view message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::IntegrityFailure, std::string{message}));
}

[[nodiscard]] Result<void> payloadTooLarge(
    const std::string_view message) noexcept
{
    return Result<void>::failure(
        makeError(ErrorCodes::PayloadTooLarge, std::string{message}));
}

[[nodiscard]] std::size_t utf8SequenceBytes(
    const unsigned char first) noexcept
{
    if (first < 0x80U) return 1U;
    if ((first & 0xe0U) == 0xc0U) return 2U;
    if ((first & 0xf0U) == 0xe0U) return 3U;
    if ((first & 0xf8U) == 0xf0U) return 4U;
    return 0U;
}

[[nodiscard]] Result<std::size_t> utf8CharacterCount(
    const std::string_view value) noexcept
{
    try {
        if (!isValidUtf8(value)) {
            return Result<std::size_t>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Legacy continuity text must be valid UTF-8."));
        }
        std::size_t count{};
        for (std::size_t offset{}; offset < value.size();) {
            const auto bytes = utf8SequenceBytes(
                static_cast<unsigned char>(value[offset]));
            if (bytes == 0U || offset + bytes > value.size()) {
                return Result<std::size_t>::failure(makeError(
                    ErrorCodes::InvalidRequest,
                    "Legacy continuity text must be valid UTF-8."));
            }
            offset += bytes;
            ++count;
        }
        return Result<std::size_t>::success(count);
    } catch (...) {
        return Result<std::size_t>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy continuity UTF-8 text could not be counted."));
    }
}

[[nodiscard]] Result<void> validateText(
    const std::string_view value,
    const std::size_t maximumBytes,
    const std::string_view field) noexcept
{
    try {
        if (value.find('\0') != std::string_view::npos || !isValidUtf8(value)) {
            return invalidValue(
                std::string{field} + " contains U+0000 or invalid UTF-8.");
        }
        if (value.size() > maximumBytes) {
            return payloadTooLarge(
                std::string{field} + " exceeds its UTF-8 byte limit.");
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy continuity text could not be validated."));
    }
}

[[nodiscard]] Result<void> validateOptionalText(
    const std::optional<std::string>& value,
    const std::size_t maximumBytes,
    const std::string_view field) noexcept
{
    if (!value) {
        return Result<void>::success();
    }
    return validateText(*value, maximumBytes, field);
}

[[nodiscard]] Result<void> validateItems(
    const std::vector<std::string>& values,
    const std::string_view field) noexcept
{
    try {
        if (values.size() > LegacyContinuityLimits::MaximumCollectionItems) {
            return payloadTooLarge(
                std::string{field} + " exceeds its item limit.");
        }
        for (const auto& value : values) {
            auto valid = validateText(
                value, LegacyContinuityLimits::MaximumItemBytes, field);
            if (!valid) {
                return valid;
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy continuity items could not be validated."));
    }
}

[[nodiscard]] Result<void> validateOptionalItems(
    const std::optional<std::vector<std::string>>& values,
    const std::string_view field) noexcept
{
    if (!values) {
        return Result<void>::success();
    }
    return validateItems(*values, field);
}

[[nodiscard]] Result<void> validateDocuments(
    const LegacyContinuityDocuments& documents) noexcept
{
    for (const auto* document : {&documents.packetJson, &documents.payloadJson}) {
        if (!*document) {
            continue;
        }
        auto valid = validateText(
            **document,
            LegacyContinuityLimits::MaximumPacketBytes,
            "Legacy continuity JSON document");
        if (!valid) {
            return valid;
        }
    }
    if (documents.payloadJson.has_value() !=
        documents.contentSha256.has_value()) {
        return integrityFailure(
            "Canonical legacy continuity payload and SHA-256 metadata must appear together.");
    }
    return Result<void>::success();
}

[[nodiscard]] std::string truncateUtf8CharactersUnchecked(
    const std::string_view value,
    const std::size_t maximumCharacters)
{
    std::size_t offset{};
    std::size_t count{};
    while (offset < value.size() && count < maximumCharacters) {
        offset += utf8SequenceBytes(static_cast<unsigned char>(value[offset]));
        ++count;
    }
    return std::string{value.substr(0U, offset)};
}

[[nodiscard]] std::string truncateUtf8BytesUnchecked(
    const std::string_view value,
    const std::size_t maximumBytes)
{
    std::size_t offset{};
    while (offset < value.size()) {
        const auto bytes = utf8SequenceBytes(
            static_cast<unsigned char>(value[offset]));
        if (offset + bytes > maximumBytes) {
            break;
        }
        offset += bytes;
    }
    return std::string{value.substr(0U, offset)};
}

void appendLine(std::string& target, const std::string_view line)
{
    if (!target.empty()) {
        target.push_back('\n');
    }
    target.append(line);
}

[[nodiscard]] Result<void> validatePacketAsDependency(
    const LegacyHandoffPacket& packet) noexcept
{
    auto valid = validateLegacyHandoffPacket(packet);
    if (valid) {
        return valid;
    }
    return integrityFailure(
        "A legacy continuity dependency returned an invalid handoff packet.");
}

} // namespace

std::string_view wireName(const LegacyHandoffSource source) noexcept
{
    switch (source) {
    case LegacyHandoffSource::Model: return "model";
    case LegacyHandoffSource::Budget: return "budget";
    case LegacyHandoffSource::User: return "user";
    case LegacyHandoffSource::Automatic: return "auto";
    }
    return "model";
}

Result<LegacyHandoffSource> legacyHandoffSourceFromWire(
    const std::string_view value) noexcept
{
    if (value == "model") {
        return Result<LegacyHandoffSource>::success(LegacyHandoffSource::Model);
    }
    if (value == "budget") {
        return Result<LegacyHandoffSource>::success(LegacyHandoffSource::Budget);
    }
    if (value == "user") {
        return Result<LegacyHandoffSource>::success(LegacyHandoffSource::User);
    }
    if (value == "auto") {
        return Result<LegacyHandoffSource>::success(
            LegacyHandoffSource::Automatic);
    }
    return Result<LegacyHandoffSource>::failure(makeError(
        ErrorCodes::IntegrityFailure,
        "The durable legacy handoff contains an unknown source."));
}

std::size_t normalizeLegacyContinuityListLimit(
    const std::int64_t requested) noexcept
{
    if (requested < 1) {
        return 1U;
    }
    const auto maximum = static_cast<std::int64_t>(
        LegacyContinuityLimits::MaximumListLimit);
    if (requested > maximum) {
        return LegacyContinuityLimits::MaximumListLimit;
    }
    return static_cast<std::size_t>(requested);
}

Result<void> validateLegacyContinuityPatch(
    const LegacyContinuityPatch& patch) noexcept
{
    try {
        for (const auto& [value, field] : {
                 std::pair{&patch.goal, std::string_view{"Continuity goal"}},
                 {&patch.status, std::string_view{"Continuity status"}},
                 {&patch.projectSlug, std::string_view{"Continuity project slug"}},
                 {&patch.workingDirectory, std::string_view{"Continuity working directory"}},
                 {&patch.chatLabel, std::string_view{"Continuity chat label"}}}) {
            auto valid = validateOptionalText(
                *value, LegacyContinuityLimits::MaximumTextBytes, field);
            if (!valid) {
                return valid;
            }
        }
        auto valid = validateOptionalText(
            patch.narrative,
            LegacyContinuityLimits::MaximumTextBytes,
            "Continuity narrative");
        if (!valid) {
            return valid;
        }
        valid = validateOptionalText(
            patch.resumeSeed,
            LegacyContinuityLimits::MaximumResumeSeedBytes,
            "Continuity resume seed");
        if (!valid) {
            return valid;
        }
        for (const auto& [values, field] : {
                 std::pair{&patch.blockers, std::string_view{"Continuity blockers"}},
                 {&patch.nextActions, std::string_view{"Continuity next actions"}},
                 {&patch.keyFiles, std::string_view{"Continuity key files"}},
                 {&patch.decisions, std::string_view{"Continuity decisions"}}}) {
            valid = validateOptionalItems(*values, field);
            if (!valid) {
                return valid;
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy continuity patch validation failed internally."));
    }
}

Result<void> validateLegacyAgentContinuitySnapshot(
    const LegacyAgentContinuitySnapshot& snapshot) noexcept
{
    auto valid = validateText(
        snapshot.goal,
        LegacyContinuityLimits::MaximumTextBytes,
        "Agent snapshot goal");
    if (!valid) return valid;
    valid = validateOptionalText(
        snapshot.workingDirectory,
        LegacyContinuityLimits::MaximumTextBytes,
        "Agent snapshot working directory");
    if (!valid) return valid;
    valid = validateText(
        snapshot.status,
        LegacyContinuityLimits::MaximumItemBytes,
        "Agent snapshot status");
    if (!valid) return valid;
    return validateText(
        snapshot.resumeHint,
        LegacyContinuityLimits::MaximumTextBytes,
        "Agent snapshot resume hint");
}

Result<void> validateLegacyActiveBindingSnapshot(
    const LegacyActiveBindingSnapshot& binding) noexcept
{
    auto valid = validateText(
        binding.goal,
        LegacyContinuityLimits::MaximumTextBytes,
        "Active binding goal");
    if (!valid) return valid;
    return validateOptionalText(
        binding.workingDirectory,
        LegacyContinuityLimits::MaximumTextBytes,
        "Active binding working directory");
}

Result<void> validateLegacyHandoffPacket(
    const LegacyHandoffPacket& packet) noexcept
{
    try {
        if (packet.schemaVersion != LegacyContinuityLimits::SchemaVersion) {
            return Result<void>::failure(makeError(
                ErrorCodes::UnsupportedVersion,
                "Legacy continuity supports only handoff schema version 1."));
        }
        switch (packet.source) {
        case LegacyHandoffSource::Model:
        case LegacyHandoffSource::Budget:
        case LegacyHandoffSource::User:
        case LegacyHandoffSource::Automatic:
            break;
        default:
            return invalidValue(
                "Legacy handoff source is outside the supported wire values.");
        }
        if (packet.updatedAt < packet.createdAt) {
            return invalidValue(
                "Legacy handoff updatedAt cannot precede createdAt.");
        }
        for (const auto& [value, field] : {
                 std::pair{&packet.goal, std::string_view{"Continuity goal"}},
                 {&packet.status, std::string_view{"Continuity status"}},
                 {&packet.narrative, std::string_view{"Continuity narrative"}}}) {
            auto valid = validateText(
                *value, LegacyContinuityLimits::MaximumTextBytes, field);
            if (!valid) return valid;
        }
        for (const auto& [value, field] : {
                 std::pair{&packet.chatLabel, std::string_view{"Continuity chat label"}},
                 {&packet.projectSlug, std::string_view{"Continuity project slug"}},
                 {&packet.workingDirectory, std::string_view{"Continuity working directory"}}}) {
            auto valid = validateOptionalText(
                *value, LegacyContinuityLimits::MaximumTextBytes, field);
            if (!valid) return valid;
        }
        auto narrativeCharacters = utf8CharacterCount(packet.narrative);
        if (!narrativeCharacters) {
            return Result<void>::failure(std::move(narrativeCharacters).error());
        }
        if (narrativeCharacters.value() >
            LegacyContinuityLimits::MaximumNarrativeCharacters) {
            return payloadTooLarge(
                "Continuity narrative exceeds 4000 Unicode characters.");
        }
        auto valid = validateText(
            packet.resumeSeed,
            LegacyContinuityLimits::MaximumResumeSeedBytes,
            "Continuity resume seed");
        if (!valid) return valid;
        for (const auto& [values, field] : {
                 std::pair{&packet.blockers, std::string_view{"Continuity blockers"}},
                 {&packet.nextActions, std::string_view{"Continuity next actions"}},
                 {&packet.keyFiles, std::string_view{"Continuity key files"}},
                 {&packet.decisions, std::string_view{"Continuity decisions"}}}) {
            valid = validateItems(*values, field);
            if (!valid) return valid;
        }
        if (packet.agents.size() >
            LegacyContinuityLimits::MaximumAgentSnapshots) {
            return payloadTooLarge(
                "Continuity packet exceeds 128 agent snapshots.");
        }
        std::set<std::string> sessionIds;
        for (const auto& snapshot : packet.agents) {
            valid = validateLegacyAgentContinuitySnapshot(snapshot);
            if (!valid) return valid;
            if (!sessionIds.insert(snapshot.sessionId.value()).second) {
                return invalidValue(
                    "Continuity packet contains a duplicate agent session id.");
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy handoff packet validation failed internally."));
    }
}

Result<void> validateLegacyContinuityRecord(
    const LegacyContinuityRecord& record) noexcept
{
    if (record.writeSequence == 0U) {
        return integrityFailure(
            "A durable legacy handoff must have a nonzero write sequence.");
    }
    auto valid = validatePacketAsDependency(record.packet);
    if (!valid) {
        return valid;
    }
    return validateDocuments(record.documents);
}

Result<void> validateLegacyContinuityList(
    const std::vector<LegacyContinuityRecord>& records,
    const std::size_t maximumCount) noexcept
{
    try {
        if (records.size() > maximumCount) {
            return integrityFailure(
                "A legacy continuity dependency exceeded the requested row limit.");
        }
        std::set<std::string> ids;
        for (std::size_t index{}; index < records.size(); ++index) {
            auto valid = validateLegacyContinuityRecord(records[index]);
            if (!valid) return valid;
            if (!ids.insert(records[index].packet.id.value()).second) {
                return integrityFailure(
                    "A legacy continuity dependency returned a duplicate handoff id.");
            }
            if (index == 0U) continue;
            const auto& previous = records[index - 1U];
            const auto& current = records[index];
            if (previous.writeSequence < current.writeSequence ||
                (previous.writeSequence == current.writeSequence &&
                 previous.packet.id.value() > current.packet.id.value())) {
                return integrityFailure(
                    "Legacy handoffs are not in deterministic sequence/id order.");
            }
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Legacy continuity list validation failed internally."));
    }
}

Result<std::string> truncateLegacyNarrative(
    const std::string_view narrative) noexcept
{
    try {
        auto valid = validateText(
            narrative,
            LegacyContinuityLimits::MaximumTextBytes,
            "Continuity narrative");
        if (!valid) {
            return Result<std::string>::failure(std::move(valid).error());
        }
        return Result<std::string>::success(truncateUtf8CharactersUnchecked(
            narrative,
            LegacyContinuityLimits::MaximumNarrativeCharacters));
    } catch (...) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Continuity narrative could not be bounded."));
    }
}

Result<std::string> makeLegacyDefaultResumeSeed(
    const LegacyHandoffPacket& packet) noexcept
{
    try {
        auto valid = validateLegacyHandoffPacket(packet);
        if (!valid) {
            return Result<std::string>::failure(std::move(valid).error());
        }

        std::string seed;
        appendLine(
            seed,
            "Forge Continuity resume (handoff " + packet.id.value() + ").");
        appendLine(
            seed,
            "Goal: " + (packet.goal.empty() ? std::string{"(none recorded)"}
                                                   : packet.goal));
        appendLine(seed, "Status: " + packet.status);
        if (packet.workingDirectory && !packet.workingDirectory->empty()) {
            appendLine(seed, "cwd: " + *packet.workingDirectory);
        }
        if (packet.projectSlug && !packet.projectSlug->empty()) {
            appendLine(seed, "project: " + *packet.projectSlug);
        }
        if (!packet.nextActions.empty()) {
            appendLine(seed, "Next actions:");
            const auto count = std::min<std::size_t>(8U, packet.nextActions.size());
            for (std::size_t index{}; index < count; ++index) {
                appendLine(seed, "- " + packet.nextActions[index]);
            }
        }
        if (!packet.agents.empty()) {
            appendLine(seed, "Open agents:");
            const auto count = std::min<std::size_t>(8U, packet.agents.size());
            for (std::size_t index{}; index < count; ++index) {
                const auto& agent = packet.agents[index];
                appendLine(
                    seed,
                    "- " + agent.agentId.value() + " session=" +
                        agent.sessionId.value() + " status=" + agent.status +
                        " goal=" + truncateUtf8CharactersUnchecked(agent.goal, 80U));
            }
            appendLine(
                seed,
                "Use agent_run_status / agent_run_complete then continue; do not invent session state.");
        }
        if (!packet.narrative.empty()) {
            appendLine(
                seed,
                "Summary: " +
                    truncateUtf8CharactersUnchecked(packet.narrative, 500U));
        }
        std::string closing;
        appendLine(
            closing,
            "Continue this packet with handoff_id: " + packet.id.value() +
                " on later checkpoints or handoffs.");
        appendLine(
            closing,
            "Call context_get for the full structured packet, then continue the task.");
        if (seed.size() + closing.size() + 1U >
            LegacyContinuityLimits::MaximumResumeSeedBytes) {
            const auto prefixBudget =
                LegacyContinuityLimits::MaximumResumeSeedBytes -
                closing.size() - 1U;
            seed = truncateUtf8BytesUnchecked(seed, prefixBudget);
        }
        appendLine(seed, closing);
        return Result<std::string>::success(std::move(seed));
    } catch (...) {
        return Result<std::string>::failure(makeError(
            ErrorCodes::InternalFailure,
            "Default continuity resume seed could not be constructed."));
    }
}

} // namespace ForgeConductor::Domain
