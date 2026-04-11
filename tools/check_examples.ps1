# Run `riz check` on every examples\*.riz (parse + diagnostics only).
# Usage: from repo root, after building riz.exe:
#   pwsh tools/check_examples.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$riz = Join-Path $Root "riz.exe"
if (-not (Test-Path $riz)) {
    $riz = Join-Path $Root "riz"
}
if (-not (Test-Path $riz)) {
    Write-Error "riz or riz.exe not found in repo root. Build first."
}

Get-ChildItem -Path (Join-Path $Root "examples") -Filter "*.riz" | Sort-Object Name | ForEach-Object {
    Write-Host "check $($_.Name)"
    & $riz check $_.FullName
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "OK: all example programs parse clean."
