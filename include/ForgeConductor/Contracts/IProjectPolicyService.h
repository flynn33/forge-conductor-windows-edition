#pragma once

#include "ForgeConductor/Contracts/IProjectPolicyGate.h"
#include <string>
#include <vector>

namespace ForgeConductor::Contracts {

struct PolicySourceFile final {
    std::string path;
    std::string content;
};

struct PolicySourceBundle final {
    std::string source;
    std::string commit;
    std::vector<PolicySourceFile> files;
    std::vector<std::string> excludedFiles;
};

class IPolicySourceReader {
public:
    virtual ~IPolicySourceReader() = default;
    [[nodiscard]] virtual Domain::Result<PolicySourceBundle> read(
        const std::string& source, const Domain::OperationContext&) noexcept = 0;
};

enum class ProjectPolicyAction { Inspect, Preview, Adopt, Review, ReadDocument };

struct ProjectPolicyRequest final {
    Domain::ProjectId projectId;
    ProjectPolicyAction action{ProjectPolicyAction::Inspect};
    // Preview: local folder or public GitHub repository URL. ReadDocument: exact
    // path in the adopted snapshot. Other actions leave source empty.
    std::string source;
    std::string expectedRevision;
    // Review is an explicit user-authored review record, never model output.
    std::string reviewJson;
};

class IProjectPolicyService : public IProjectPolicyGate {
public:
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        const ProjectPolicyRequest&, const Domain::OperationContext&) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
