#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/DeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioConfigurationCodec.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WinHttpLocalModelSessionTransport.h"

#include <memory>

namespace ForgeConductor::Infrastructure::Windows {

[[nodiscard]] std::unique_ptr<Contracts::IAgentCompletionReportInspector>
createWindowsAgentCompletionReportInspector(Contracts::IClock& clock);

} // namespace ForgeConductor::Infrastructure::Windows
