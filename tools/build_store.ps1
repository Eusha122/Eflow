# Packs the release build into an .msix for the Microsoft Store.
#
#   tools\build_store.ps1 -IdentityName 12345Publisher.Eflow -Publisher "CN=ABC123..." -PublisherDisplayName "Your name"
#
# Those three values come from Partner Center (your app's "Product identity" page) and must match it exactly, or the
# upload is rejected. -TestSign additionally signs the package with a throwaway certificate so it can be installed on
# a test machine (Windows Sandbox); the Store signs the real thing itself, so never ship a test-signed package.
param(
    [Parameter(Mandatory = $true)][string]$IdentityName,
    [Parameter(Mandatory = $true)][string]$Publisher,
    [Parameter(Mandatory = $true)][string]$PublisherDisplayName,
    [string]$Version = "1.0.0.0",
    [switch]$TestSign
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root "build\release"
if (-not (Test-Path (Join-Path $release "eflow.exe"))) { throw "Build it first: tools\build_app.ps1 release" }
if ($Version -notmatch '^\d+\.\d+\.\d+\.0$') { throw "The Store wants a version like 1.0.0.0, with the last number 0." }

$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe" | Sort-Object FullName | Select-Object -Last 1
if (-not $sdk) { throw "makeappx.exe not found (install the Windows SDK)." }
$bin = Split-Path $sdk.FullName -Parent

$stage = Join-Path $root "build\store\payload"
$out = Join-Path $root "build\store\Eflow.msix"
Remove-Item -Recurse -Force (Join-Path $root "build\store") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# ---- the app itself -------------------------------------------------------------------------------------------
Copy-Item (Join-Path $release "eflow.exe") $stage
Copy-Item (Join-Path $release "eflow-hook.exe") $stage
Copy-Item (Join-Path $release "assets") $stage -Recurse            # the mascot and the agent logos
Copy-Item (Join-Path $root "package\assets\*.png") (Join-Path $stage "assets")   # the tiles and store logo

# Solu's Python source only: its runtime and models are downloaded later, into a writable folder.
$soluOut = Join-Path $stage "solu"
New-Item -ItemType Directory -Force $soluOut | Out-Null
Copy-Item (Join-Path $root "solu\*.py") $soluOut
foreach ($extra in "requirements.txt", "presets.json") {
    $path = Join-Path $root "solu\$extra"
    if (Test-Path $path) { Copy-Item $path $soluOut }
}

# ---- the manifest, with this submission's identity ------------------------------------------------------------
$manifest = Get-Content (Join-Path $root "package\store\AppxManifest.xml") -Raw
$manifest = $manifest.Replace("{IDENTITY_NAME}", $IdentityName).Replace("{PUBLISHER}", $Publisher)
$manifest = $manifest.Replace("{PUBLISHER_DISPLAY_NAME}", $PublisherDisplayName).Replace("{VERSION}", $Version)
Set-Content (Join-Path $stage "AppxManifest.xml") $manifest -Encoding UTF8

# ---- resources.pri: how Windows picks the right tile for each size and scale -----------------------------------
$config = Join-Path $env:TEMP "eflow-store-priconfig.xml"
& "$bin\makepri.exe" createconfig /cf $config /dq en-US /pv 10.0.0 /o | Out-Null
& "$bin\makepri.exe" new /pr $stage /cf $config /mn (Join-Path $stage "AppxManifest.xml") /of (Join-Path $stage "resources.pri") /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri failed." }

& "$bin\makeappx.exe" pack /d $stage /p $out /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx failed." }

if ($TestSign) {
    # A throwaway certificate, only so the package can be installed on a test machine. Its subject must equal the
    # manifest's Publisher, or Windows refuses the package.
    $pfx = Join-Path $root "build\store\test-cert.pfx"
    $password = ConvertTo-SecureString -String "eflow-test" -Force -AsPlainText
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $password | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $root "build\store\test-cert.cer") | Out-Null
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force
    & "$bin\signtool.exe" sign /fd SHA256 /f $pfx /p "eflow-test" $out | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool failed." }
    Write-Host "Test-signed. Install it on a test machine by trusting build\store\test-cert.cer first."
}

Write-Host ("Packed {0} ({1:N1} MB)" -f $out, ((Get-Item $out).Length / 1MB))
