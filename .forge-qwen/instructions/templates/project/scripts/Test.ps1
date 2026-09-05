[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Debug',[ValidateSet('x64','ARM64')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$preset="windows-msvc-$($Architecture.ToLowerInvariant())-$($Configuration.ToLowerInvariant())"
ctest --preset $preset --output-on-failure
if($LASTEXITCODE -ne 0){throw 'CTest failed.'}
