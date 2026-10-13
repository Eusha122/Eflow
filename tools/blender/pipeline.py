"""Headless build: rig -> animations -> .blend + GLB -> accurate preview renders.

.deps/blender-5.2.2/blender.exe --background --factory-startup --python tools/blender/pipeline.py
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import bpy  # noqa: E402
from paths import RIG_BLEND  # noqa: E402


def step(name):
    print(f"== {name}")
    ns = {"__file__": str(HERE / name), "__name__": "__main__"}
    exec(compile((HERE / name).read_text(encoding="utf-8"), name, "exec"), ns)
    print("   ->", ns.get("result"))


step("build_rig.py")
step("build_anim.py")
step("export_glb.py")
RIG_BLEND.parent.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=str(RIG_BLEND))   # source rig, before preview-only changes
step("preview_setup.py")
step("preview_states.py")
step("preview_anim.py")
print("PIPELINE DONE")
