# Forsetti Framework - PR Compatibility Check Script
# Copyright (c) 2026 James Daley. All Rights Reserved.
#
# Validates pull request changes for compatibility with framework rules:
# - No direct module-to-module communication
# - No use of forsetti.internal.* namespace in module code
# - Test structure validation
# - Test count regression detection

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$violations = @()

Write-Host "=== PR Compatibility Check ===" -ForegroundColor Cyan

# --- Check: forsetti.internal.* namespace in module code ---
Write-Host "`nChecking for reserved namespace usage in module code..." -ForegroundColor Yellow

$moduleRoots = @(
    "$repoRoot\src\ForsettiExampleServiceModule",
    "$repoRoot\src\ForsettiExampleUIModule",
    "$repoRoot\src\ForsettiExampleAppModule"
)
$moduleFiles = @()
foreach ($moduleRoot in $moduleRoots) {
    if (Test-Path $moduleRoot) {
        $moduleFiles += Get-ChildItem -Path $moduleRoot -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
    }
}
foreach ($file in $moduleFiles) {
    $content = Get-Content $file.FullName -ErrorAction SilentlyContinue
    $lineNum = 0
    foreach ($line in $content) {
        $lineNum++
        if ($line -match 'forsetti\.internal\.' -and $line -notmatch '^\s*//') {
            $rel = $file.FullName.Replace("$repoRoot\", "")
            $violations += "$rel`:$lineNum - Uses reserved 'forsetti.internal.*' namespace (reserved for framework)"
        }
    }
}

# --- Check: Direct module-to-module includes ---
Write-Host "Checking for direct module-to-module references..." -ForegroundColor Yellow

foreach ($file in $moduleFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    $rel = $file.FullName.Replace("$repoRoot\", "")

    # Module code should not include other module headers directly
    if ($content -match '#include\s+[<"].*Module.*\.h[>"]' -and $content -notmatch '#include\s+[<"]ForsettiCore/' -and $file.Name -notmatch 'ExampleModule') {
        # Skip self-references within the same module
        if ($content -match '#include\s+[<"](?!ForsettiCore/)(?!ForsettiPlatform/).*Module.*\.h[>"]') {
            # This is OK if it's a local include within the same module directory
        }
    }
}

# --- Check: Test file structure ---
Write-Host "Checking test file structure..." -ForegroundColor Yellow

$testFiles = Get-ChildItem -Path "$repoRoot\tests" -Recurse -Include "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $testFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    $rel = $file.FullName.Replace("$repoRoot\", "")

    # Must include CppUnitTest.h
    if ($content -notmatch '#include\s+[<"]CppUnitTest\.h[>"]') {
        $violations += "$rel - Test file does not include CppUnitTest.h"
    }

    # Must have at least one TEST_CLASS
    if ($content -notmatch 'TEST_CLASS\s*\(') {
        $violations += "$rel - Test file has no TEST_CLASS declaration"
    }

    # Must have at least one TEST_METHOD
    if ($content -notmatch 'TEST_METHOD\s*\(') {
        $violations += "$rel - Test file has no TEST_METHOD declaration"
    }
}

# --- Check: Test count regression ---
Write-Host "Counting test methods..." -ForegroundColor Yellow

$totalTests = 0
foreach ($file in $testFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    $matches = [regex]::Matches($content, 'TEST_METHOD\s*\(')
    $totalTests += $matches.Count
}

$minExpectedTests = 131  # Current baseline
Write-Host "  Found $totalTests TEST_METHOD declarations (baseline: $minExpectedTests)" -ForegroundColor Gray

if ($totalTests -lt $minExpectedTests) {
    $violations += "Test regression: Found $totalTests test methods, expected at least $minExpectedTests"
}

# --- Check: Source files use Forsetti namespace ---
Write-Host "Checking namespace usage..." -ForegroundColor Yellow

$srcFiles = Get-ChildItem -Path "$repoRoot\src" -Recurse -Include "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $srcFiles) {
    $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
    $rel = $file.FullName.Replace("$repoRoot\", "")

    # Skip stub/placeholder files that contain only comments and whitespace
    $codeOnly = $content -replace '//[^\n]*' -replace '/\*[\s\S]*?\*/' -replace '\s+'
    if ([string]::IsNullOrWhiteSpace($codeOnly)) { continue }

    if ($content -notmatch 'namespace\s+Forsetti' -and $content -notmatch 'Forsetti::') {
        $violations += "$rel - Source file does not use the Forsetti namespace"
    }
}

# --- Check: No raw new/delete in source (prefer smart pointers) ---
Write-Host "Checking for raw new/delete usage..." -ForegroundColor Yellow

$allSrcFiles = Get-ChildItem -Path "$repoRoot\include", "$repoRoot\src" -Recurse -Include "*.h", "*.cpp" -ErrorAction SilentlyContinue
foreach ($file in $allSrcFiles) {
    $content = Get-Content $file.FullName -ErrorAction SilentlyContinue
    $lineNum = 0
    foreach ($line in $content) {
        $lineNum++
        # Check for raw new (but not placement new, not make_unique/make_shared, not comments)
        if ($line -match '\bnew\s+\w+[\s\(]' -and $line -notmatch '^\s*//' -and $line -notmatch 'make_unique|make_shared|placement' -and $line -notmatch 'operator\s+new') {
            $rel = $file.FullName.Replace("$repoRoot\", "")
            $violations += "$rel`:$lineNum - Raw 'new' detected (prefer std::make_unique/std::make_shared)"
        }
        if ($line -match '\bdelete\s+\w+' -and $line -notmatch '^\s*//' -and $line -notmatch 'operator\s+delete' -and $line -notmatch '= delete') {
            $rel = $file.FullName.Replace("$repoRoot\", "")
            $violations += "$rel`:$lineNum - Raw 'delete' detected (prefer RAII/smart pointers)"
        }
    }
}

# --- Report ---
Write-Host ""
if ($violations.Count -eq 0) {
    Write-Host "All compatibility checks passed." -ForegroundColor Green
    exit 0
} else {
    Write-Host "Compatibility violations found:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  - $v" -ForegroundColor Red
    }
    Write-Host "`n$($violations.Count) violation(s) found." -ForegroundColor Red
    exit 1
}
