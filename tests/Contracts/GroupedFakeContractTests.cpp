#include "Contracts/GroupedFakeContractTests.h"

#include "Contracts/ApplicationServiceFakeContractTests.h"
#include "Contracts/AuthorityAdjacentPlatformFakeContractTests.h"
#include "Contracts/McpCancellationContractTests.h"
#include "Contracts/RepositoryDiagnosticsManagerFakeContractTests.h"

namespace ForgeConductor::Tests {

void runGroupedFakeContractTests()
{
    runApplicationServiceFakeContractTests();
    runMcpCancellationContractTests();
    runAuthorityAdjacentPlatformFakeContractTests();
    runRepositoryDiagnosticsManagerFakeContractTests();
}

} // namespace ForgeConductor::Tests
