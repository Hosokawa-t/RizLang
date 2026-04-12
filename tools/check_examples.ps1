# Run `riz check --strict` on every examples\*.riz (errors and static warnings fail the check).
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

Get-ChildItem -Path (Join-Path $Root "examples") -Filter "*.riz" -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($Root.Length).TrimStart([char[]]@('\', '/'))
    Write-Host "check $rel"
    & $riz check --strict $_.FullName
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "OK: all example programs pass riz check."
