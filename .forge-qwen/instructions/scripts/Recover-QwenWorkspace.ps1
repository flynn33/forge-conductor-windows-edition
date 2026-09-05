[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkspaceRoot)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$WorkspaceRoot=Get-NormalizedFullPath -Path $WorkspaceRoot
Push-Location $WorkspaceRoot
try{
    $assert=& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $WorkspaceRoot -RequireCurrentDirectory|ConvertFrom-Json
    $assignment=& "$PSScriptRoot\Prepare-QwenAssignment.ps1" -Repository $WorkspaceRoot|ConvertFrom-Json
    Add-QwenLedgerEvent -Repository $WorkspaceRoot -Role 'architect' -Microtask ([string]$assignment.microtask.id) -Action 'wrong_workspace_recovery' -Data @{fingerprint=$assert.workspace_fingerprint;note='Foreign continuity/memory ignored; no foreign repository modified.'}|Out-Null
    [ordered]@{recovered=$true;workspace=$WorkspaceRoot;run_id=$assert.run_id;fingerprint=$assert.workspace_fingerprint;active_microtask=$assignment.microtask.id;next_message=(Join-Path $WorkspaceRoot '.forge-qwen\NEXT_MESSAGE.txt')}|ConvertTo-Json -Depth 30
}
finally{Pop-Location}
