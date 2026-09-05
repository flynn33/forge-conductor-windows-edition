[CmdletBinding()]
param([string]$Repository=(Get-Location).Path,[Parameter(Mandatory)][string]$Path)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-RepoRoot -Start $Repository
$lock=Get-QwenWorkspaceLock -Repository $Repository
$full=if([IO.Path]::IsPathRooted($Path)){$Path}else{Join-Path $Repository $Path}
$handoff=Read-Json $full
if($handoff.mission_id -ne $lock.mission_id){throw 'Foreign handoff mission.'}
if($handoff.run_id -ne $lock.run_id){throw 'Foreign handoff run.'}
if($handoff.workspace_fingerprint -ne $lock.repository_fingerprint){throw 'Foreign handoff fingerprint.'}
if(-not(Test-SamePath -Left ([string]$handoff.target_repository) -Right $Repository)){throw 'Foreign handoff target repository.'}
$body=[ordered]@{
 schema_version=$handoff.schema_version;mission_id=$handoff.mission_id;handoff_id=$handoff.handoff_id;run_id=$handoff.run_id;workspace_fingerprint=$handoff.workspace_fingerprint
 target_repository=$handoff.target_repository;memory_namespace=$handoff.memory_namespace;assignment_id=$handoff.assignment_id;microtask_id=$handoff.microtask_id
 created_utc=$handoff.created_utc;status=$handoff.status;objective=$handoff.objective;verified_facts=$handoff.verified_facts;files_changed=$handoff.files_changed
 commands=$handoff.commands;evidence=$handoff.evidence;decisions=$handoff.decisions;blockers=$handoff.blockers;next_action=$handoff.next_action;git_state=$handoff.git_state
 tool_binding_revision=$handoff.tool_binding_revision;ledger_head=$handoff.ledger_head
}
$expected=Get-StringSha256 -Text ($body|ConvertTo-Json -Depth 100 -Compress)
if($expected -ne $handoff.checksum){throw 'Handoff checksum mismatch.'}
[pscustomobject]@{valid=$true;path=$full;run_id=$lock.run_id;workspace_fingerprint=$lock.repository_fingerprint;checksum=$expected}|ConvertTo-Json
