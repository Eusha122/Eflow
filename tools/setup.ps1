# One-time setup for the mascot tooling:
#   .venv/                 Python packages from tools/requirements.txt
#   .deps/blender-5.2.2/   portable Blender (official build, SHA-256 verified)
# Both folders are git-ignored. Re-running skips anything already present.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$BlenderVersion = "5.2.2"
$BlenderZip = "blender-$BlenderVersion-windows-x64.zip"
$BlenderUrl = "https://download.blender.org/release/Blender5.2/$BlenderZip"
$BlenderSha256 = "3849d17a682cba006075aaa3f3597ecb5c9c30ec31035b2e092c53e40679b535"
$BlenderDir = Join-Path $root ".deps\blender-$BlenderVersion"

if (-not (Test-Path ".venv\Scripts\python.exe")) {
    Write-Host "Creating .venv"
    python -m venv .venv
}
& .\.venv\Scripts\python.exe -m pip install --quiet --disable-pip-version-check -r tools\requirements.txt

if (-not (Test-Path (Join-Path $BlenderDir "blender.exe"))) {
    $tmp = Join-Path $root ".deps\_download"
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp $BlenderZip
    Write-Host "Downloading Blender $BlenderVersion (~400 MB)"
    Invoke-WebRequest -Uri $BlenderUrl -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $BlenderSha256) { throw "Blender download checksum mismatch: $hash" }
    Expand-Archive $zip -DestinationPath $tmp -Force
    Move-Item (Join-Path $tmp "blender-$BlenderVersion-windows-x64") $BlenderDir
    Remove-Item $tmp -Recurse -Force
}
Write-Host "Setup complete. Build the mascot with: tools\build_mascot.ps1"
