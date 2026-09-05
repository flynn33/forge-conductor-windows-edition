[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [string]$ApiBase = 'http://127.0.0.1:1234',
    [string]$ModelNamePattern = 'qwen',
    [string]$ParameterPattern = '27B',
    [double]$TargetBitsPerWeight = 4.0
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\Common.ps1"
$Repository = Get-RepoRoot -Start $Repository
$token = $env:LM_API_TOKEN
if (-not $token) {
    throw 'LM_API_TOKEN is required for unattended API mode with mcp.json plugins.'
}
$headers = @{ Authorization = ('Bearer ' + $token) }

function Get-LMModels {
    param([string]$Base, [hashtable]$Headers)
    try {
        return Invoke-RestMethod -Method Get -Uri ($Base.TrimEnd('/') + '/api/v1/models') -Headers $Headers -TimeoutSec 30
    }
    catch {
        $lms = Get-Command lms.exe -ErrorAction SilentlyContinue
        if (-not $lms) { $lms = Get-Command lms -ErrorAction SilentlyContinue }
        if ($lms) {
            & $lms.Source server start | Out-Null
            Start-Sleep -Seconds 3
            return Invoke-RestMethod -Method Get -Uri ($Base.TrimEnd('/') + '/api/v1/models') -Headers $Headers -TimeoutSec 30
        }
        throw
    }
}

$response = Get-LMModels -Base $ApiBase -Headers $headers
$candidates = @($response.models | Where-Object {
    $_.type -eq 'llm' -and
    (([string]$_.key -match $ModelNamePattern) -or ([string]$_.display_name -match $ModelNamePattern) -or ([string]$_.architecture -match $ModelNamePattern)) -and
    (([string]$_.params_string -match $ParameterPattern) -or ([string]$_.display_name -match $ParameterPattern) -or ([string]$_.key -match '27[bB]')) -and
    $_.quantization -and
    [Math]::Abs(([double]$_.quantization.bits_per_weight) - $TargetBitsPerWeight) -le 0.75
})
if ($candidates.Count -ne 1) {
    $summary = @($candidates | ForEach-Object { [ordered]@{ key=$_.key; display_name=$_.display_name; params=$_.params_string; quantization=$_.quantization } })
    $path = Join-Path $Repository '.forge-qwen\state\blockers\lmstudio-model-resolution.json'
    Write-JsonAtomic -Path $path -Value ([ordered]@{ utc=Get-UtcNow; reason='Expected exactly one matching Qwen 27B approximately 4-bit model.'; candidates=$summary })
    throw "LM Studio model selection is ambiguous or missing. Evidence: $path"
}
$model = $candidates[0]
$loaded = @($model.loaded_instances)
$contextLength = if ($loaded.Count -gt 0) { [int]$loaded[0].config.context_length } else { [int]$model.max_context_length }
if ($contextLength -lt 24576) {
    throw "The selected model context length ($contextLength) is below the package's minimum agentic target of 24576."
}

$profilePath = Join-Path $Repository '.forge-qwen\state\profiles\lmstudio.json'
$profile = Read-Json $profilePath
$capabilities = @('shell','filesystem','github','rag_v1','js_sandbox','long_term_memory','persistent_memory')
$pluginBindings = @()
foreach ($capability in $capabilities) {
    $matches = @($profile.servers | Where-Object { @($_.capability_candidates) -contains $capability })
    if ($matches.Count -ne 1) {
        $path = Join-Path $Repository ('.forge-qwen\state\blockers\lmstudio-plugin-' + $capability + '.json')
        Write-JsonAtomic -Path $path -Value ([ordered]@{ utc=Get-UtcNow; capability=$capability; reason='Expected exactly one mcp.json server-label candidate.'; matches=$matches })
        throw "Plugin resolution for $capability is ambiguous or missing. Evidence: $path"
    }
    $pluginBindings += [ordered]@{ capability=$capability; id=[string]$matches[0].plugin_id; allowed_tools=@() }
}

$configPath = Join-Path $Repository '.forge-qwen\config\runner-config.json'
$config = Read-Json $configPath
$config.api_base = $ApiBase
$config.model_id = [string]$model.key
$config.context_length = [Math]::Min($contextLength, 32768)
$config.plugin_bindings = $pluginBindings
Write-JsonAtomic -Path $configPath -Value $config
Write-JsonAtomic -Path (Join-Path $Repository '.forge-qwen\state\profiles\resolved-runner.json') -Value ([ordered]@{
    utc=Get-UtcNow
    model_key=$model.key
    display_name=$model.display_name
    architecture=$model.architecture
    params_string=$model.params_string
    quantization=$model.quantization
    loaded_instances=$model.loaded_instances
    selected_context_length=$config.context_length
    plugin_bindings=$pluginBindings
    verification_status='candidate bindings; BOOT-TOOLS-001 must run harmless probes before treating tools as verified'
})
$config | ConvertTo-Json -Depth 30
