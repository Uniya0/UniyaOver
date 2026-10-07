# Pack portable dist\UniyaOver (bin/ + assets/) and single SFX setup exe.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\pack_dist.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$release = Join-Path $root "build\Release"
$out = Join-Path $root "dist\UniyaOver"
$ffmpegBin = Join-Path $root "third_party\ffmpeg\bin"

$exeCandidates = @(
    (Join-Path $release "UniyaOver.exe")
)
$coreCandidates = @(
    (Join-Path $release "bin\UniyaOverCore.exe"),
    (Join-Path $release "UniyaOverCore.exe")
)
$exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$core = $coreCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw "UniyaOver.exe launcher not found. Build Release first." }
if (-not $core) { throw "UniyaOverCore.exe not found. Build Release first." }

$dlls = @(
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "swresample-6.dll",
    "swscale-9.dll"
)

if (Test-Path $out) {
    Remove-Item $out -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $out "bin") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out "assets") | Out-Null

Copy-Item $exe (Join-Path $out "UniyaOver.exe") -Force
Copy-Item $core (Join-Path $out "bin\UniyaOverCore.exe") -Force
foreach ($dll in $dlls) {
    $src = Join-Path $ffmpegBin $dll
    if (-not (Test-Path $src)) { $src = Join-Path $release "bin\$dll" }
    if (-not (Test-Path $src)) { throw "Missing DLL: $dll" }
    Copy-Item $src (Join-Path $out "bin\$dll") -Force
}

$ico = Join-Path $root "assets\uniyaover.ico"
if (Test-Path $ico) {
    Copy-Item $ico (Join-Path $out "assets\uniyaover.ico") -Force
}

$readme = Join-Path $root "dist\README.txt"
if (Test-Path $readme) {
    Copy-Item $readme (Join-Path $out "README.txt") -Force
}
foreach ($extra in @(
    @{ Src = "LICENSE"; Dst = "LICENSE.txt" },
    @{ Src = "THIRD_PARTY_NOTICES.txt"; Dst = "THIRD_PARTY_NOTICES.txt" },
    @{ Src = "third_party\ffmpeg\LICENSE.txt"; Dst = "FFMPEG-LICENSE.txt" }
)) {
    $srcPath = Join-Path $root $extra.Src
    if (Test-Path $srcPath) {
        Copy-Item $srcPath (Join-Path $out $extra.Dst) -Force
    }
}

$sizeMb = [math]::Round(((Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "Packed portable: $out  ($sizeMb MB)"
Get-ChildItem $out -Recurse -File | ForEach-Object {
    [PSCustomObject]@{
        Rel = $_.FullName.Substring($out.Length + 1)
        MB  = [math]::Round($_.Length / 1MB, 2)
    }
} | Format-Table -AutoSize

# ---- Build .uopk package + append to UniyaOverSetup.exe => one SFX ----
function Write-U32([IO.BinaryWriter]$w, [uint32]$v) {
    $w.Write([BitConverter]::GetBytes($v))
}
function Write-U64([IO.BinaryWriter]$w, [uint64]$v) {
    $w.Write([BitConverter]::GetBytes($v))
}

$setupStubCandidates = @(
    (Join-Path $release "UniyaOverSetup.exe"),
    (Join-Path $root "build\Release\UniyaOverSetup.exe")
)
$setupStub = $setupStubCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $setupStub) {
    Write-Host "UniyaOverSetup.exe stub not found - build target uniyaover_setup first."
    exit 0
}

$packPath = Join-Path $root "dist\payload.uopk"
$files = Get-ChildItem $out -Recurse -File
$fs = [IO.File]::Create($packPath)
$bw = New-Object IO.BinaryWriter $fs
try {
    $bw.Write([Text.Encoding]::ASCII.GetBytes("UOPACK01"))
    Write-U32 $bw ([uint32]$files.Count)
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($out.Length + 1).Replace("\", "/")
        $relBytes = [Text.Encoding]::UTF8.GetBytes($rel)
        Write-U32 $bw ([uint32]$relBytes.Length)
        $bw.Write($relBytes)
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        Write-U64 $bw ([uint64]$bytes.LongLength)
        $bw.Write($bytes)
    }
} finally {
    $bw.Flush()
    $bw.Close()
    $fs.Close()
}

$sfxOut = Join-Path $root "dist\UniyaOver-Setup.exe"
if (Test-Path $sfxOut) { Remove-Item $sfxOut -Force }

# stub + pack + uint64 offset + "UOSFX001"
$stubBytes = [IO.File]::ReadAllBytes($setupStub)
$packBytes = [IO.File]::ReadAllBytes($packPath)
$offset = [uint64]$stubBytes.LongLength
$outFs = [IO.File]::Create($sfxOut)
try {
    $outFs.Write($stubBytes, 0, $stubBytes.Length)
    $outFs.Write($packBytes, 0, $packBytes.Length)
    $outFs.Write([BitConverter]::GetBytes($offset), 0, 8)
    $magic = [Text.Encoding]::ASCII.GetBytes("UOSFX001")
    $outFs.Write($magic, 0, 8)
} finally {
    $outFs.Close()
}

Remove-Item $packPath -Force -ErrorAction SilentlyContinue

$sfxMb = [math]::Round((Get-Item $sfxOut).Length / 1MB, 1)
Write-Host ("SFX setup: {0} ({1} MB) - one file to share" -f $sfxOut, $sfxMb)
