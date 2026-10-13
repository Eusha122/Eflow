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
Add-AppxPackage -Register (Join-Path $root "package\AppxManifest.xml") -ExternalLocation $exeDir
Write-Host "Registered Eflow ($Config). Start $exeDir\eflow.exe."
