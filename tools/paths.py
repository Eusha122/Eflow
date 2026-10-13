"""Project paths shared by the mascot tooling (plain Python and Blender scripts)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ART_SOURCE = ROOT / "art" / "source"          # generated character art (inputs, committed)
BUILD = ROOT / "build" / "mascot"             # everything below is regenerated (git-ignored)
MATTE = BUILD / "matte"
PATCHES = BUILD / "patches"
RENDER = BUILD / "render"
RIG_BLEND = BUILD / "eflow_rig.blend"
ASSETS = ROOT / "assets" / "mascot"           # runtime asset loaded by the app (committed)
GLB = ASSETS / "eflow.glb"
BLENDER = ROOT / ".deps" / "blender-5.2.2" / "blender.exe"
