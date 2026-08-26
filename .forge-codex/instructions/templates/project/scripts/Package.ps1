[CmdletBinding()]
param(
    [ValidateSet("x64","ARM64")][string]$Architecture = "x64",
    [string]$PfxPath = $env:FORGE_SIGNING_PFX,
    [string]$PfxPassword = $env:FORGE_SIGNING_PASSWORD
)
$ErrorActionPreference = "Stop"

# Codex must replace project paths after creating the WinUI/MSIX projects.
# This script remains the canonical noninteractive entry point.
if (-not $PfxPath) {
    throw "No signing certificate configured. Invoke the development-certificate script and pass its PFX."
}
throw "Packaging project has not yet been implemented. Complete phase P24 rather than suppressing this gate."
