#pragma once

#include "AuthorityContextFakeContractTests.h"
#include "NativeToolBoundaryFakeContractTests.h"
#include "PlatformStorageFakeContractTests.h"

namespace ForgeConductor::Tests {

inline void runAuthorityAdjacentPlatformFakeContractTests()
{
    runAuthorityContextFakeContractTests();
    runPlatformStorageFakeContractTests();
    runNativeToolBoundaryFakeContractTests();
}

} // namespace ForgeConductor::Tests
