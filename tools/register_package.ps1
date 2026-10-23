# Registers (or removes) Eflow's sparse package so eflow.exe runs with package identity, which Windows
# requires before an app may read notifications. Needs Developer Mode (Settings > System > For developers).
#   tools\register_package.ps1             -> registers build\debug
#   tools\register_package.ps1 release     -> registers build\release
#   tools\register_package.ps1 -Remove     -> unregisters
param([ValidateSet("debug", "release")][string]$Config = "debug", [switch]$Remove)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$name = "Eflow.Island"
$existing = Get-AppxPackage -Name $name
if ($existing) { Remove-AppxPackage -Package $existing.PackageFullName }
if ($Remove) { Write-Host "Eflow package removed."; return }
$exeDir = Join-Path $root "build\$Config"
if (-not (Test-Path (Join-Path $exeDir "eflow.exe"))) { throw "Build first: tools\build_app.ps1 $Config" }
# resources.pri lets Windows pick the right logo file for each size and scale (assets\*.scale-*, *.targetsize-*). A sparse
# package reads its logos from the external location, so the build copies package\assets there and the index goes there too.
$makepri = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makepri.exe" | Sort-Object FullName | Select-Object -Last 1
if (-not $makepri) { throw "makepri.exe not found (Windows SDK)." }
$pkg = Join-Path $root "package"
$priConfig = Join-Path $env:TEMP "eflow-priconfig.xml"
& $makepri.FullName createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Null
& $makepri.FullName new /pr $pkg /cf $priConfig /mn (Join-Path $pkg "AppxManifest.xml") /of (Join-Path $exeDir "resources.pri") /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri failed." }
Add-AppxPackage -Register (Join-Path $root "package\AppxManifest.xml") -ExternalLocation $exeDir
Write-Host "Registered Eflow ($Config). Start $exeDir\eflow.exe."
