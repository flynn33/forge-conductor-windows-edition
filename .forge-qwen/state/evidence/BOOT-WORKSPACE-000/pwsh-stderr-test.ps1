$ErrorActionPreference = 'Stop'
cmd /c type D:\nonexistent-file-xyz 2>$null | Out-Null
Write-Output PWSH_STDERR_OK
