param(
    [string]$Configuration = "Release",
    [string]$OutputDir = "build\release"
)

$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$binDir = Join-Path $projectDir "build\bin"
$releaseDir = Join-Path $projectDir $OutputDir

Write-Host "=== PeepoDrumKit Release Packager ===" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration"
Write-Host "Source: $binDir"
Write-Host "Output: $releaseDir"

if (-not (Test-Path $binDir)) {
    Write-Error "Build output not found at $binDir. Build the project first."
    exit 1
}

$exeName = if ($Configuration -eq "Debug") { "PeepoDrumKit_Debug.exe" } else { "PeepoDrumKit.exe" }
$exePath = Join-Path $binDir $exeName

if (-not (Test-Path $exePath)) {
    Write-Error "Executable not found: $exePath"
    exit 1
}

if (Test-Path $releaseDir) {
    Write-Host "Cleaning previous release..." -ForegroundColor Yellow
    Remove-Item $releaseDir -Recurse -Force
}

New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null

Write-Host "Copying executable..." -ForegroundColor Green
Copy-Item $exePath -Destination (Join-Path $releaseDir "PeepoDrumKit.exe") -Force

$pdbName = if ($Configuration -eq "Debug") { "PeepoDrumKit_Debug.pdb" } else { "PeepoDrumKit.pdb" }
$pdbPath = Join-Path $binDir $pdbName
if (Test-Path $pdbPath) {
    Write-Host "Copying PDB (for crash debugging)..." -ForegroundColor Green
    Copy-Item $pdbPath -Destination (Join-Path $releaseDir "PeepoDrumKit.pdb") -Force
}

Write-Host "Copying assets..." -ForegroundColor Green
Copy-Item (Join-Path $projectDir "assets") -Destination (Join-Path $releaseDir "assets") -Recurse -Force

Write-Host "Copying locales..." -ForegroundColor Green
Copy-Item (Join-Path $projectDir "locales") -Destination (Join-Path $releaseDir "locales") -Recurse -Force

Write-Host "Copying license..." -ForegroundColor Green
Copy-Item (Join-Path $projectDir "license_txt") -Destination (Join-Path $releaseDir "license_txt") -Recurse -Force

Write-Host ""
Write-Host "=== Release package created at: $releaseDir ===" -ForegroundColor Cyan
Get-ChildItem $releaseDir -Recurse | ForEach-Object {
    $size = if ($_.Length) { "{0:N0} bytes" -f $_.Length } else { "" }
    Write-Host "  $($_.FullName.Substring($releaseDir.Length+1)) $size"
}

Write-Host ""
Write-Host "Ready for GitHub Release! Zip the '$OutputDir' folder and upload." -ForegroundColor Green