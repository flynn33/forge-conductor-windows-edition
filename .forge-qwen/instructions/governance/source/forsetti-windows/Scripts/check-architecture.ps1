# Forsetti Framework — Architecture Enforcement Script
# Copyright (c) 2026 James Daley. All Rights Reserved.
#
# Scans #include directives to enforce one-way dependency layering (R006).
# Scans for non-final classes (R005).

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$violations = @()

Write-Host "=== Architecture Enforcement ===" -ForegroundColor Cyan

# --- Rule R006: One-way dependencies ---
Write-Host "`nChecking #include layering rules..." -ForegroundColor Yellow

# ForsettiCore must NOT include ForsettiPlatform, ForsettiHostTemplate, or example modules
$coreFiles = Get-ChildItem -Path "$repoRoot\include\ForsettiCore", "$repoRoot\src\ForsettiCore" -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $coreFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($content -match '#include\s+[<"]ForsettiPlatform/') {
        $violations += "R006: $($file.FullName) includes ForsettiPlatform (forbidden for ForsettiCore)"
    }
    if ($content -match '#include\s+[<"]ForsettiHostTemplate/') {
        $violations += "R006: $($file.FullName) includes ForsettiHostTemplate (forbidden for ForsettiCore)"
    }
    if ($content -match '#include\s+[<"]ForsettiExample(Service|UI|App)Module/') {
        $violations += "R006: $($file.FullName) includes an example module (forbidden for ForsettiCore)"
    }
}

# ForsettiPlatform must NOT include ForsettiHostTemplate or example modules
$platformFiles = Get-ChildItem -Path "$repoRoot\include\ForsettiPlatform", "$repoRoot\src\ForsettiPlatform" -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $platformFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($content -match '#include\s+[<"]ForsettiHostTemplate/') {
        $violations += "R006: $($file.FullName) includes ForsettiHostTemplate (forbidden for ForsettiPlatform)"
    }
    if ($content -match '#include\s+[<"]ForsettiExample(Service|UI|App)Module/') {
        $violations += "R006: $($file.FullName) includes an example module (forbidden for ForsettiPlatform)"
    }
}

# Example modules must NOT include ForsettiPlatform, ForsettiHostTemplate, or each other
$exampleRoots = @(
    "$repoRoot\src\ForsettiExampleServiceModule",
    "$repoRoot\src\ForsettiExampleUIModule",
    "$repoRoot\src\ForsettiExampleAppModule"
)
$exampleFiles = @()
foreach ($exampleRoot in $exampleRoots) {
    if (Test-Path $exampleRoot) {
        $exampleFiles += Get-ChildItem -Path $exampleRoot -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
    }
}
foreach ($file in $exampleFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    if ($content -match '#include\s+[<"]ForsettiPlatform/') {
        $violations += "R006: $($file.FullName) includes ForsettiPlatform (forbidden for example modules)"
    }
    if ($content -match '#include\s+[<"]ForsettiHostTemplate/') {
        $violations += "R006: $($file.FullName) includes ForsettiHostTemplate (forbidden for example modules)"
    }
    if ($content -match '#include\s+[<"]Example(Service|UI|App)Module\.h[>"]' -and
        $file.DirectoryName -notmatch $Matches[1]) {
        $violations += "R006: $($file.FullName) includes another example module"
    }
}

# --- Rule R005: All classes must be final ---
Write-Host "Checking for non-final classes..." -ForegroundColor Yellow

$allSourceFiles = Get-ChildItem -Path "$repoRoot\include", "$repoRoot\src" -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $allSourceFiles) {
    $content = Get-Content $file.FullName -ErrorAction SilentlyContinue
    $lineNum = 0
    foreach ($line in $content) {
        $lineNum++
        # Match class declarations that are NOT final and NOT pure interfaces (starting with I)
        # Skip: abstract base classes (interfaces), forward declarations, template specializations
        if ($line -match '^\s*class\s+(\w+)\s*(?::\s*public)' -and $line -notmatch '\bfinal\b' -and $line -notmatch '^\s*class\s+I[A-Z]') {
            $className = $Matches[1]
            # Skip known interface/abstract classes
            if ($className -notmatch '^I[A-Z]') {
                $violations += "R005: $($file.FullName):$lineNum — class '$className' is not declared final"
            }
        }
    }
}

# --- Report ---
Write-Host ""
if ($violations.Count -eq 0) {
    Write-Host "All architecture checks passed." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Architecture violations found:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  - $v" -ForegroundColor Red
    }
    Write-Host "`n$($violations.Count) violation(s) found." -ForegroundColor Red
    exit 1
}
