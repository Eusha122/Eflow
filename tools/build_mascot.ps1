# Rebuilds the mascot from the generated art (run tools\setup.ps1 once first):
#   art/source/*.png -> build/mascot/matte (2048 canvas) -> build/mascot/patches (face parts)
#   -> Blender (headless) rig + animations -> assets/mascot/eflow.glb
#   -> build/mascot/render previews (stills, contact sheets, animated WebP + preview.html)
# "Continue": Windows PowerShell 5.1 turns any native stderr line into a terminating error under "Stop".
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$py = Join-Path $root ".venv\Scripts\python.exe"
$blender = Join-Path $root ".deps\blender-5.2.2\blender.exe"
if (-not (Test-Path $py) -or -not (Test-Path $blender)) { throw "Run tools\setup.ps1 first." }

& $py tools\matte.py | Out-Null
& $py tools\face_patches.py
& $blender --background --factory-startup --python tools\blender\pipeline.py 2>&1 |
    Select-String -Pattern "^==|->|Error|Traceback|PIPELINE DONE"
if ($LASTEXITCODE -ne 0) { throw "Blender pipeline failed ($LASTEXITCODE)" }
Remove-Item build\mascot\render\frames -Recurse -Force -ErrorAction SilentlyContinue
& $blender --background build\mascot\eflow_rig.blend --python tools\blender\render_clips.py 2>&1 |
    Select-String -Pattern "Error|Traceback|CLIPS DONE"
if ($LASTEXITCODE -ne 0) { throw "Clip render failed ($LASTEXITCODE)" }
& $py tools\make_sheets.py
& $py tools\make_preview.py
& $py tools\inspect_glb.py
