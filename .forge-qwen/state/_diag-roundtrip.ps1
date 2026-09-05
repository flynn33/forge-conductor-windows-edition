$ErrorActionPreference = 'Stop'
$f = 'D:\GitHub\Forge-Conductor-Windows-Edition\.forge-qwen\state\handoffs\handoff-P00-A-20260904T121711714Z.json'
$raw = Get-Content -Raw -LiteralPath $f
$h = ConvertFrom-Json $raw

$body = [ordered]@{
    schema_version=$h.schema_version; mission_id=$h.mission_id; handoff_id=$h.handoff_id; run_id=$h.run_id
    workspace_fingerprint=$h.workspace_fingerprint; target_repository=$h.target_repository
    memory_namespace=$h.memory_namespace; assignment_id=$h.assignment_id; microtask_id=$h.microtask_id
    created_utc=$h.created_utc; status=$h.status; objective=$h.objective
    verified_facts=$h.verified_facts; files_changed=$h.files_changed; commands=$h.commands
    evidence=$h.evidence; decisions=$h.decisions; blockers=$h.blockers; next_action=$h.next_action
    git_state=$h.git_state; tool_binding_revision=$h.tool_binding_revision; ledger_head=$h.ledger_head
}

$canon = $body | ConvertTo-Json -Depth 100 -Compress
$alg = [Security.Cryptography.SHA256]::Create()
try {
    $hash = ([BitConverter]::ToString($alg.ComputeHash([Text.Encoding]::UTF8.GetBytes($canon)))).Replace('-','').ToLowerInvariant()
} finally { $alg.Dispose() }

Write-Output "expected=$hash"
Write-Output "stored  =$($h.checksum)"
Write-Output "match   = $($hash -eq [string]$h.checksum)"

$rt = $body | ConvertTo-Json -Depth 100
$a = ($raw -replace '\s+','')
$b = ($rt -replace '\s+','')
Write-Output "roundtrip_lossless= $($a -eq $b)"

foreach ($k in @('schema_version','mission_id','handoff_id','run_id','workspace_fingerprint','target_repository','memory_namespace','assignment_id','microtask_id','created_utc','status','objective','verified_facts','files_changed','commands','evidence','decisions','blockers','next_action','git_state','tool_binding_revision','ledger_head')) {
    $v = $h.$k
    if ($null -eq $v) { $s1='NULL' } else { $s1 = (ConvertTo-Json -InputObject $v -Depth 50 -Compress) }
    Write-Output ("field[{0}] type={1} json={2}" -f $k, ($v.GetType().FullName), $s1)
}
