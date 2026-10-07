# Finish GitHub publish after: gh auth login
# Creates public repo, pushes main, uploads UniyaOver-Setup.exe as Release v1.0.0

$ErrorActionPreference = "Stop"
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

gh auth status
if ($LASTEXITCODE -ne 0) {
    Write-Host "Run first: gh auth login"
    exit 1
}

$setup = Join-Path $root "dist\UniyaOver-Setup.exe"
if (-not (Test-Path $setup)) {
    throw "Missing dist\UniyaOver-Setup.exe — run scripts\pack_dist.ps1 first"
}

$user = (gh api user --jq .login)
$repoName = "UniyaOver"
Write-Host "GitHub user: $user"

# Create repo if missing
$exists = $false
gh repo view "$user/$repoName" 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { $exists = $true }

if (-not $exists) {
    gh repo create $repoName --public --source=. --remote=origin --description "UniyaOver — Instant Replay for Windows (NVIDIA NVENC). Offline. (c) UNIYA."
} else {
    $url = "https://github.com/$user/$repoName.git"
    git remote remove origin 2>$null
    git remote add origin $url
}

git push -u origin main

# Release with installer asset
$notes = @"
## UniyaOver v1.0.0

Instant replay for Windows (NVIDIA NVENC). Offline, no account.

### Download
- **UniyaOver-Setup.exe** — installer (pick folder, INSTALL)

### Requirements
- Windows 10/11 x64
- NVIDIA GPU with NVENC
- [VC++ Redistributable x64](https://aka.ms/vs/17/release/vc_redist.x64.exe)

### Notes
- Not for competitive anti-cheat (Valorant, etc.) — use at your own risk
- Unsigned build may trigger SmartScreen
- License: (c) UNIYA, all rights reserved — see LICENSE
- Contact: https://t.me/UNIYA0
"@

gh release create v1.0.0 "$setup" --title "UniyaOver v1.0.0" --notes $notes --latest
Write-Host "Done: https://github.com/$user/$repoName/releases/tag/v1.0.0"
