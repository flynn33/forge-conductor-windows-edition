[CmdletBinding()]
param([string]$Repository=(Get-Location).Path)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\Common.ps1"
$Repository=Get-RepoRoot -Start $Repository
& "$PSScriptRoot\Assert-QwenWorkspace.ps1" -Repository $Repository|Out-Null
$plan=Read-Json (Join-Path $Repository '.forge-qwen\state\microtasks.json')
$byId=@{}
foreach($item in $plan.microtasks){$byId[[string]$item.id]=$item}
foreach($item in $plan.microtasks){
    if($item.status -eq 'passed'){continue}
    $ready=$true
    foreach($dependency in $item.dependencies){
        if(-not $byId.ContainsKey([string]$dependency) -or $byId[[string]$dependency].status -ne 'passed'){$ready=$false;break}
    }
    if($ready -and $item.status -ne 'blocked'){
        [pscustomobject]@{
            id=$item.id;phase_id=$item.phase_id;role=$item.role;title=$item.title;objective=$item.objective;status=$item.status
            dependencies=$item.dependencies;attempts=if($item.PSObject.Properties.Name -contains 'attempts'){$item.attempts}else{0}
            document_routes=$item.document_routes;rag_queries=$item.rag_queries;required_capabilities=$item.required_capabilities
            allowed_write_roots=$item.allowed_write_roots;max_files_changed=$item.max_files_changed;max_lines_changed=$item.max_lines_changed
            max_tool_calls=$item.max_tool_calls;max_attempts=$item.max_attempts;acceptance=$item.acceptance
        }|ConvertTo-Json -Depth 50
        return
    }
}
[pscustomobject]@{id=$null;reason='no dependency-ready unblocked microtask';all_passed=(@($plan.microtasks|Where-Object status -ne 'passed').Count -eq 0)}|ConvertTo-Json
