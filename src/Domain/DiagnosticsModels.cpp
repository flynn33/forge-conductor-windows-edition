#include "ForgeConductor/Domain/DiagnosticsModels.h"

namespace ForgeConductor::Domain {

AuditOutcome auditOutcomeFor(const std::string_view status) noexcept
{
    if (status == "ok") return AuditOutcome::Success;
    if (status == "error") return AuditOutcome::OperationalError;
    if (status == "denied") return AuditOutcome::PolicyDenied;
    if (status == "warn") return AuditOutcome::MaintenanceWarning;
    return AuditOutcome::Other;
}

AuditOutcomeCounts summarizeAuditOutcomes(const std::vector<std::string>& statuses) noexcept
{
    AuditOutcomeCounts counts;
    for (const auto& status : statuses) {
        switch (auditOutcomeFor(status)) {
        case AuditOutcome::Success: break;
        case AuditOutcome::OperationalError: ++counts.errorCount; break;
        case AuditOutcome::PolicyDenied: ++counts.deniedCount; break;
        case AuditOutcome::MaintenanceWarning: ++counts.warningCount; break;
        case AuditOutcome::Other: ++counts.otherCount; break;
        }
    }
    return counts;
}

Result<void> validateDiagnosticEnvelope(const DiagnosticEnvelope& envelope)
{
    if (envelope.event.empty() || envelope.role.empty()) {
        return Result<void>::failure(makeError(
            ErrorCodes::InvalidRequest,
            "Diagnostic event and role must be nonempty."));
    }
    if (envelope.event.size() > MaximumDiagnosticEventBytes ||
        envelope.role.size() > MaximumDiagnosticRoleBytes) {
        return Result<void>::failure(makeError(
            ErrorCodes::PayloadTooLarge,
            "Diagnostic event or role exceeds its UTF-8 byte limit."));
    }
    if (envelope.fields.size() > MaximumDiagnosticFieldCount) {
        return Result<void>::failure(makeError(
            ErrorCodes::LimitExceeded,
            "Diagnostic field count exceeds 64."));
    }
    for (const auto& field : envelope.fields) {
        if (field.name.empty()) {
            return Result<void>::failure(makeError(
                ErrorCodes::InvalidRequest,
                "Diagnostic field names must be nonempty."));
        }
        if (field.name.size() > MaximumDiagnosticFieldNameBytes ||
            field.value.size() > MaximumDiagnosticFieldValueBytes) {
            return Result<void>::failure(makeError(
                ErrorCodes::PayloadTooLarge,
                "Diagnostic field exceeds its bounded key/value size."));
        }
    }
    return Result<void>::success();
}

} // namespace ForgeConductor::Domain
