[CmdletBinding()]
param()
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$instructions = Join-Path $root ".forge-codex\instructions\scripts"

& (Join-Path $PSScriptRoot "Build.ps1") -Configuration Debug -Architecture x64
& (Join-Path $PSScriptRoot "Test.ps1") -Configuration Debug -Architecture x64
& (Join-Path $PSScriptRoot "Build.ps1") -Configuration Release -Architecture x64
& (Join-Path $PSScriptRoot "Test.ps1") -Configuration Release -Architecture x64
& (Join-Path $instructions "Validate-NoPython.ps1") -WorkspaceRoot $root
& (Join-Path $instructions "Validate-NativeStack.ps1") -WorkspaceRoot $root
& (Join-Path $instructions "Validate-NoAttribution.ps1") -WorkspaceRoot $root
& (Join-Path $instructions "Verify-Ledger.ps1") -WorkspaceRoot $root
