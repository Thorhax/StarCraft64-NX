# StarCraft 64 NX installer for Windows (PowerShell + Docker Desktop): builds the Switch
# NRO from your own ROM and extracts your own classic StarCraft PC files, all inside
# Docker on this computer. See docs/installer.md.
#
#   powershell -ExecutionPolicy Bypass -File tools\installer\install.ps1 `
#       -Rom "C:\Games\StarCraft 64 (USA).z64" -StarCraft "C:\Games\StarCraft" [-Out sd-card] [-NoWidescreen]
param(
    [string]$Rom,
    [string]$StarCraft = "",
    [string]$Out = "sd-card",
    [switch]$NoWidescreen
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..\..")

if (-not $Rom) { $Rom = Read-Host "Path to your StarCraft 64 (USA) ROM" }
if (-not (Test-Path -LiteralPath $Rom -PathType Leaf)) { throw "ROM not found: $Rom" }
if (-not $StarCraft) {
    $StarCraft = Read-Host "Path to your classic StarCraft install (Enter to skip the PC graphics, music and voices)"
}
if ($StarCraft -and -not (Test-Path -LiteralPath $StarCraft -PathType Container)) { throw "StarCraft folder not found: $StarCraft" }
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw "Docker Desktop is required: https://docs.docker.com/desktop/" }
docker info *> $null
if ($LASTEXITCODE -ne 0) { throw "Docker Desktop is installed but not running: start it and try again" }

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$romPath = (Resolve-Path -LiteralPath $Rom).Path
$outPath = (Resolve-Path -LiteralPath $Out).Path

Write-Host "Building the installer image (the first time takes 30-60 minutes; later runs reuse it)..."
docker build -f tools/installer/Dockerfile -t sc64-installer . *> (Join-Path $outPath "image-build.log")
if ($LASTEXITCODE -ne 0) {
    Get-Content (Join-Path $outPath "image-build.log") -Tail 30
    throw "Image build failed (full log: $outPath\image-build.log)"
}

$mounts = @("-v", "${romPath}:/in/rom:ro", "-v", "${outPath}:/out")
if ($StarCraft) { $mounts += @("-v", "$((Resolve-Path -LiteralPath $StarCraft).Path):/in/starcraft:ro") }
$widescreen = if ($NoWidescreen) { "0" } else { "1" }
docker run --rm @mounts -e "SC64_WIDESCREEN=$widescreen" sc64-installer
if ($LASTEXITCODE -ne 0) { throw "The build failed: see the logs folder in $outPath" }
Write-Host "Your SD card files are in $outPath\switch"
