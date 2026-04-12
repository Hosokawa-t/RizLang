#Requires -Version 5.1
<#
.SYNOPSIS
    Build riz-v{VERSION}-windows-x64.zip with riz.exe, full src/ (AOT), examples, LSP (built + prod node_modules), and optional vendor/tcc.

.DESCRIPTION
    Run from repo root or pass -RepoRoot. Requires Node.js 18+ on PATH when -SkipLspBuild is not set.
    Output defaults to: <RepoRoot>\riz-v{VERSION}-windows-x64.zip

.PARAMETER RizExe
    Path to built riz.exe (default: <RepoRoot>\riz.exe).

.PARAMETER Version
    Override version string (default: parsed from src\common.h RIZ_VERSION).

.PARAMETER OutDir
    Directory for the zip file (default: repo root).

.PARAMETER SkipLspBuild
    Copy existing lsp\out from repo instead of npm ci / npm run build (fails if out\server.js missing).

.EXAMPLE
    pwsh tools/package_windows_release.ps1
    pwsh tools/package_windows_release.ps1 -RizExe D:\build\riz.exe -OutDir D:\dist
#>
[CmdletBinding()]
param(
    [string] $RepoRoot = "",
    [string] $RizExe = "",
    [string] $Version = "",
    [string] $OutDir = "",
    [switch] $SkipLspBuild
)

$ErrorActionPreference = "Stop"

function Get-RizVersionFromHeader {
    param([string] $CommonHPath)
    $raw = Get-Content -LiteralPath $CommonHPath -Raw -Encoding UTF8
    if ($raw -match '#define\s+RIZ_VERSION\s+"([^"]+)"') {
        return $Matches[1]
    }
    throw "Could not parse RIZ_VERSION from $CommonHPath"
}

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$commonH = Join-Path $RepoRoot "src\common.h"
if (-not (Test-Path -LiteralPath $commonH)) {
    throw "src\common.h not found under $RepoRoot"
}

if (-not $Version) {
    $Version = Get-RizVersionFromHeader -CommonHPath $commonH
}

if (-not $RizExe) {
    $RizExe = Join-Path $RepoRoot "riz.exe"
}
if (-not (Test-Path -LiteralPath $RizExe)) {
    throw "riz.exe not found: $RizExe (build first, or pass -RizExe)"
}

if (-not $OutDir) {
    $OutDir = $RepoRoot
}
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$bundleName = "riz-v$Version-windows-x64"
$stagingParent = Join-Path $env:TEMP ("riz-win-zip-" + [Guid]::NewGuid().ToString("n"))
$root = Join-Path $stagingParent $bundleName

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null

    Copy-Item -LiteralPath $RizExe -Destination (Join-Path $root "riz.exe") -Force

    # AOT + host sources (paths expected by riz --aot)
    Copy-Item -LiteralPath (Join-Path $RepoRoot "src") -Destination (Join-Path $root "src") -Recurse -Force
    Get-ChildItem -Path (Join-Path $root "src") -Recurse -File | Where-Object {
        $_.Extension -notin @(".c", ".h")
    } | Remove-Item -Force

    # Skip huge model payloads (often present locally but not in git)
    $exSrc = Join-Path $RepoRoot "examples"
    $exDst = Join-Path $root "examples"
    New-Item -ItemType Directory -Force -Path $exDst | Out-Null
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        & robocopy $exSrc $exDst /E /XF *.gguf *.zip *.exe *_aot.c *.pdb *.obj *.ilk *.exp *.lib /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy examples failed (exit $LASTEXITCODE)" }
    } else {
        Get-ChildItem -LiteralPath $exSrc -Recurse -File | ForEach-Object {
            $bn = $_.Name
            if ($_.Extension -match '^(?:\.gguf|\.zip|\.exe|\.pdb|\.obj|\.ilk|\.exp|\.lib)$') { return }
            if ($bn -like '*_aot.c') { return }
            $rel = $_.FullName.Substring($exSrc.Length).TrimStart('\', '/')
            $destFile = Join-Path $exDst $rel
            $destDir = Split-Path -Parent $destFile
            if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
            Copy-Item -LiteralPath $_.FullName -Destination $destFile -Force
        }
    }

    $toolsOut = Join-Path $root "tools"
    New-Item -ItemType Directory -Force -Path $toolsOut | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepoRoot "tools\check_examples.ps1") -Destination $toolsOut -Force
    if (Test-Path -LiteralPath (Join-Path $RepoRoot "tools\check_examples.sh")) {
        Copy-Item -LiteralPath (Join-Path $RepoRoot "tools\check_examples.sh") -Destination $toolsOut -Force
    }

    # Optional TinyCC fallback for AOT when gcc is not installed
    $tccSrc = Join-Path $RepoRoot "vendor\tcc\tcc.exe"
    if (Test-Path -LiteralPath $tccSrc) {
        $tccDst = Join-Path $root "vendor\tcc"
        New-Item -ItemType Directory -Force -Path $tccDst | Out-Null
        Copy-Item -LiteralPath $tccSrc -Destination $tccDst -Force
        Get-ChildItem -Path (Join-Path $RepoRoot "vendor\tcc") -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $tccDst -Force -ErrorAction SilentlyContinue
        }
    }

    $lspDest = Join-Path $root "lsp"
    New-Item -ItemType Directory -Force -Path $lspDest | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\package.json") -Destination $lspDest -Force
    Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\package-lock.json") -Destination $lspDest -Force
    Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\tsconfig.json") -Destination $lspDest -Force
    Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\src") -Destination (Join-Path $lspDest "src") -Recurse -Force

    if ($SkipLspBuild) {
        $built = Join-Path $RepoRoot "lsp\out\server.js"
        if (-not (Test-Path -LiteralPath $built)) {
            throw "SkipLspBuild: missing $built"
        }
        Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\out") -Destination (Join-Path $lspDest "out") -Recurse -Force
        if (Test-Path -LiteralPath (Join-Path $RepoRoot "lsp\node_modules")) {
            Copy-Item -LiteralPath (Join-Path $RepoRoot "lsp\node_modules") -Destination (Join-Path $lspDest "node_modules") -Recurse -Force
        } else {
            Write-Warning "SkipLspBuild: no lsp\node_modules; run npm ci in lsp before zipping for a portable LSP."
        }
    } else {
        Push-Location $lspDest
        try {
            # Need devDependencies (typescript) for `npm run build`; prune after compile.
            npm ci
            npm run build
            npm prune --omit=dev
        } finally {
            Pop-Location
        }
    }

    $readme = @"
# Riz $Version (Windows x64 bundle)

## Contents
- **riz.exe** — interpreter, ``riz check``, ``riz --vm``, ``riz --aot``, ``riz pkg``, ``riz env``
- **src/** — C sources required by ``riz --aot`` (relative paths)
- **examples/** — sample ``.riz`` files
- **tools/** — ``check_examples.ps1``
- **lsp/** — language server (Node.js 18+)

## Quick start (PowerShell)

    Set-Location `$PSScriptRoot
    .\riz.exe examples\hello.riz
    .\riz.exe check examples\hello.riz

## AOT

Use **gcc** on PATH (e.g. MSYS2), or add ``vendor\tcc\tcc.exe`` (TinyCC) under this folder.

    .\riz.exe --aot examples\hello.riz

Run the above from the extracted folder so ``src\`` resolves.

## LSP

    `$env:RIZ_PATH = Join-Path (Get-Location) 'riz.exe'
    node .\lsp\out\server.js --stdio

Archive: **riz-v$Version-windows-x64**
"@
    Set-Content -LiteralPath (Join-Path $root "README-WINDOWS.md") -Value $readme -Encoding UTF8

    $zipPath = Join-Path $OutDir "$bundleName.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path $root -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host "OK: $zipPath"
}
finally {
    Remove-Item -LiteralPath $stagingParent -Recurse -Force -ErrorAction SilentlyContinue
}
