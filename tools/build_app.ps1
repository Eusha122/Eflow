# Configures and builds the app with the Visual Studio C++ toolchain (CMake + Ninja).
#   tools\build_app.ps1            -> build\debug\eflow.exe
#   tools\build_app.ps1 release    -> build\release\eflow.exe
param([ValidateSet("debug", "release")][string]$Config = "debug")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ desktop workload is required." }
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

# Run configure + build inside the VS developer environment (cl, ninja and cmake on PATH).
cmd /c "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && cd /d `"$root`" && cmake --preset $Config && cmake --build --preset $Config"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
