# Builds EflowSetup.exe, the installer people download from the website.
#
#   tools\build_installer.ps1 [-Version 1.0.0]
#
# It is deliberately not code-signed (a certificate costs money we are not spending): Windows SmartScreen will warn
# the first time someone runs it, and the website says so and shows what to click. The Store build is the route for
# people who would rather not see that warning at all.
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root "build\release"
if (-not (Test-Path (Join-Path $release "eflow.exe"))) { throw "Build it first: tools\build_app.ps1 release" }
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Use a version like 1.0.0" }

$iscc = Get-ChildItem "${env:ProgramFiles(x86)}\Inno Setup*\ISCC.exe", "${env:ProgramFiles}\Inno Setup*\ISCC.exe",
                      "$env:LOCALAPPDATA\Programs\Inno Setup*\ISCC.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup not found. Install it: winget install JRSoftware.InnoSetup" }
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makepri.exe" | Sort-Object FullName | Select-Object -Last 1
if (-not $sdk) { throw "makepri.exe not found (install the Windows SDK)." }

$stage = Join-Path $root "build\installer\payload"
Remove-Item -Recurse -Force (Join-Path $root "build\installer") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# ---- what gets installed --------------------------------------------------------------------------------------
Copy-Item (Join-Path $release "eflow.exe") $stage
Copy-Item (Join-Path $release "eflow-hook.exe") $stage
Copy-Item (Join-Path $release "assets") $stage -Recurse                          # the mascot and the agent logos
Copy-Item (Join-Path $root "package\assets\*.png") (Join-Path $stage "assets")    # the tiles the sparse package names

# Solu's Python source only: its runtime and models are downloaded later, into %LOCALAPPDATA%\Eflow\solu.
$soluOut = Join-Path $stage "solu"
New-Item -ItemType Directory -Force $soluOut | Out-Null
Copy-Item (Join-Path $root "solu\*.py") $soluOut
foreach ($extra in "requirements.txt", "presets.json") {
    $path = Join-Path $root "solu\$extra"
    if (Test-Path $path) { Copy-Item $path $soluOut }
}

# ---- the sparse package, for the Notifications tab -------------------------------------------------------------
$packageOut = Join-Path $stage "package"
New-Item -ItemType Directory -Force $packageOut | Out-Null
Copy-Item (Join-Path $root "package\AppxManifest.xml") $packageOut
Copy-Item (Join-Path $root "package\installer\register.ps1") $packageOut
# resources.pri belongs next to the manifest's external location (the install folder), and end users have no SDK.
$bin = Split-Path $sdk.FullName -Parent
$config = Join-Path $env:TEMP "eflow-installer-priconfig.xml"
& "$bin\makepri.exe" createconfig /cf $config /dq en-US /pv 10.0.0 /o | Out-Null
& "$bin\makepri.exe" new /pr $stage /cf $config /mn (Join-Path $packageOut "AppxManifest.xml") /of (Join-Path $stage "resources.pri") /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri failed." }

& $iscc.FullName "/DAppVersion=$Version" "/DPayload=$stage" (Join-Path $root "package\installer\Eflow.iss") | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }

$setup = Join-Path $root "build\installer\EflowSetup.exe"
Write-Host ("Built {0} ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
