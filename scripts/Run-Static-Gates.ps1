[CmdletBinding()]param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$tools=Join-Path $root '.forge-qwen\instructions\scripts'
& (Join-Path $tools 'Validate-NoPython.ps1') -Repository $root
& (Join-Path $tools 'Validate-NativeStack.ps1') -Repository $root
& (Join-Path $tools 'Validate-NoAttribution.ps1') -Repository $root
