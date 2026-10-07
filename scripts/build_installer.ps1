# Build UniyaOver-Setup-*.exe with Inno Setup 6 (free).
# Prereq: pack dist first, then install Inno Setup from https://jrsoftware.org/isinfo.php

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$packed = Join-Path $root "dist\UniyaOver\UniyaOver.exe"
if (-not (Test-Path $packed)) {
    Write-Host "dist\UniyaOver missing - running pack_dist.ps1..."
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "scripts\pack_dist.ps1")
}

$iss = Join-Path $root "installer\UniyaOver.iss"
$isccCandidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    Write-Host "Inno Setup 6 not found (ISCC.exe)."
    Write-Host "1. Install free Inno Setup: https://jrsoftware.org/isinfo.php"
    Write-Host "2. Re-run: powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1"
    Write-Host "Or open installer\UniyaOver.iss in Inno Setup Compiler and click Build."
    exit 1
}

Write-Host "Compiling with: $iscc"
& $iscc $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit $LASTEXITCODE" }

Get-ChildItem (Join-Path $root "dist") -Filter "UniyaOver-Setup-*.exe" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 3 Name, Length, LastWriteTime
Write-Host "Done."
