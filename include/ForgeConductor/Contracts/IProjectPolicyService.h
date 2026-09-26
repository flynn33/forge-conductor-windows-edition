#pragma once

#include "ForgeConductor/Contracts/IProjectPolicyGate.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ForgeConductor::Contracts {

struct PolicySourceFile final {
    std::string path;
    std::string kind{"file"};
    std::uint64_t byteLength{};
    std::optional<std::string> contentHash;
    std::optional<std::string> content;
    std::string interpretation{"opaque"};
    std::optional<std::string> coverageDetail;
};

struct PolicySourceBundle final {
    std::string source;
    std::string commit;
    std::vector<PolicySourceFile> files;
};

class IPolicySourceReader {
public:
    virtual ~IPolicySourceReader() = default;
    [[nodiscard]] virtual Domain::Result<PolicySourceBundle> read(
        const std::string& source, const Domain::OperationContext&) noexcept = 0;
};

enum class ProjectPolicyAction {
    Inspect,
    Bind,
    Refresh,
    ReadDocument,
    ListFindings,
    Evaluate,
    Resolve,
    ExportLog
};

struct ProjectPolicyRequest final {
    Domain::ProjectId projectId;
    ProjectPolicyAction action{ProjectPolicyAction::Inspect};
    // Bind: local folder or remote repository URL. ReadDocument: exact path in
    // the bound immutable revision. Other actions leave source empty.
    std::string source;
    std::string expectedRevision;
    // Action-specific canonical JSON: page window, evaluation evidence,
    // resolution evidence, or export options.
    std::string detailsJson;
};

class IProjectPolicyService : public IProjectPolicyGate {
public:
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        const ProjectPolicyRequest&, const Domain::OperationContext&) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
