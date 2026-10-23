# Registers Eflow's sparse package, which is what gives eflow.exe the package identity Windows requires before an app
# may read the notifications it shows. Run by the installer after it copies the files, and again by Eflow itself if it
# ever finds the registration missing.
#
# This is best-effort on purpose. Windows only accepts a sparse package that is signed by a certificate it trusts, or
# on a machine with Developer Mode turned on. The copy from the website is not signed, so on most PCs this will fail
# and Eflow simply runs without the Notifications tab; everything else works. The Microsoft Store build has a real
# identity and does not need any of this.
param([Parameter(Mandatory = $true)][string]$AppDir)
$ErrorActionPreference = "Stop"
$manifest = Join-Path $AppDir "package\AppxManifest.xml"
if (-not (Test-Path $manifest)) { exit 1 }
try {
    $existing = Get-AppxPackage -Name "Eflow.Island" -ErrorAction SilentlyContinue
    if ($existing) { Remove-AppxPackage -Package $existing.PackageFullName -ErrorAction SilentlyContinue }
    Add-AppxPackage -Register $manifest -ExternalLocation $AppDir -ErrorAction Stop
    exit 0
} catch {
    exit 2            # Eflow reads this as "no notifications"; nothing else is affected
}
