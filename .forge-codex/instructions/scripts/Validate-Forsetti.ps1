[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$WorkspaceRoot = (Resolve-Path -LiteralPath $WorkspaceRoot).Path
. (Join-Path $PSScriptRoot 'Common.ps1')

function Invoke-CheckedPowerShellScript {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments
    )

    & powershell.exe -NoLogo -NoProfile -NonInteractive -File $ScriptPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

$roots = Read-JsonFile (Join-Path $WorkspaceRoot '.forge-inputs\source-roots.json')
$framework = [string]$roots.forsetti_framework
$agentic = [string]$roots.forsetti_agentic
$generator = Join-Path $WorkspaceRoot '.forge-codex\scripts\Generate-P03Baseline.ps1'
$consumerValidator = Join-Path $WorkspaceRoot 'scripts\validation\Test-G03ForsettiArchitecture.ps1'
$agenticValidator = Join-Path $agentic 'core\validator\forsetti_validate.ps1'
$frameworkManifestValidator = Join-Path $framework 'Scripts\check-manifests.ps1'

foreach ($requiredPath in @($generator, $consumerValidator, $agenticValidator, $frameworkManifestValidator)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required Forsetti validation entry point is missing: $requiredPath"
    }
}

& $generator -WorkspaceRoot $WorkspaceRoot

& $consumerValidator -WorkspaceRoot $WorkspaceRoot

$advisoryRoot = Join-Path $WorkspaceRoot '.forge-codex\state\baseline\p03-upstream-advisory'
New-Item -ItemType Directory -Force -Path $advisoryRoot | Out-Null
$contract = Join-Path $WorkspaceRoot '.forge-codex\instructions\governance\PORT_TASK_CONTRACT.json'
$context = Join-Path $WorkspaceRoot '.forge-codex\instructions\governance\forsetti-project-context.json'
$profile = Join-Path $agentic 'editions\windows\forsetti-windows-0.2.0.profile.json'
$manifest = Join-Path $WorkspaceRoot 'src\ForgeConductor.ForsettiModule\Resources\ForsettiManifests\ForgeConductorAppModule.json'

Invoke-CheckedPowerShellScript -ScriptPath $agenticValidator -Label 'Supplied contract advisory validation' -Arguments @(
    '-RepoRoot', $agentic, '-Mode', 'contract', '-ContractPath', $contract, '-Strict',
    '-OutputJson', (Join-Path $advisoryRoot 'contract.json')
)
Invoke-CheckedPowerShellScript -ScriptPath $agenticValidator -Label 'Supplied project-context advisory validation' -Arguments @(
    '-RepoRoot', $agentic, '-Mode', 'project-context', '-ProjectContextPath', $context,
    '-EditionProfilePath', $profile, '-Strict', '-OutputJson', (Join-Path $advisoryRoot 'project-context.json')
)
Invoke-CheckedPowerShellScript -ScriptPath $agenticValidator -Label 'Supplied edition-profile advisory validation' -Arguments @(
    '-RepoRoot', $agentic, '-Mode', 'edition-profile', '-EditionProfilePath', $profile, '-Strict',
    '-OutputJson', (Join-Path $advisoryRoot 'edition-profile.json')
)
Invoke-CheckedPowerShellScript -ScriptPath $agenticValidator -Label 'Supplied manifest advisory validation' -Arguments @(
    '-RepoRoot', $agentic, '-Mode', 'manifest', '-EditionProfilePath', $profile, '-ManifestPath', $manifest,
    '-Strict', '-OutputJson', (Join-Path $advisoryRoot 'manifest.json')
)
Invoke-CheckedPowerShellScript -ScriptPath $frameworkManifestValidator -Label 'Supplied framework manifest advisory validation' -Arguments @(
    '-RepoRoot', $WorkspaceRoot
)

Write-Host 'Forsetti consumer validation passed. Supplied advisory modes passed; known upstream build/test defects remain recorded in P03-010.'
