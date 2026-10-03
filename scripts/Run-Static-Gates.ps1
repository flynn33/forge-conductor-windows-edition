[CmdletBinding()]param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$tools=Join-Path $root 'scripts\quality'
& (Join-Path $tools 'Validate-NoPython.ps1') -Repository $root
& (Join-Path $tools 'Validate-NativeStack.ps1') -Repository $root
& (Join-Path $tools 'Validate-NoAttribution.ps1') -Repository $root
