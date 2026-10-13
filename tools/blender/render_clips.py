"""Render every frame of every clip (accurate preview look).

.deps/blender-5.2.2/blender.exe --background build/mascot/eflow_rig.blend --python tools/blender/render_clips.py
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bpy  # noqa: E402
from paths import RENDER  # noqa: E402

HERE = Path(__file__).resolve().parent
exec(compile((HERE / "preview_setup.py").read_text(encoding="utf-8"), "preview_setup.py", "exec"), {})

scn = bpy.data.scenes["EflowRig"]
arm = bpy.data.objects["EflowArmature"]
out = RENDER / "frames"
scn.render.resolution_x, scn.render.resolution_y = 900, 800
FACE = {"idle": "neutral", "music": "happy", "notify": "surprised", "permission": "wink"}

for clip, face in FACE.items():
    action = bpy.data.actions[clip]
    arm.animation_data.action = action
    for o in scn.objects:
        if o.name.startswith("face_"):
            o.hide_render = not o.name.startswith(f"face_{face}_")
    d = out / clip
    os.makedirs(d, exist_ok=True)
    start, end = (int(v) for v in action.frame_range)
    for f in range(start, end):            # last frame == first frame for loops
        scn.frame_set(f)
        scn.render.filepath = str(d / f"{f:03d}.png")
        bpy.ops.render.render(write_still=True, scene=scn.name)
print("CLIPS DONE")
