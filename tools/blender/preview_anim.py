import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import bpy  # noqa: E402
from paths import RENDER  # noqa: E402

scn = bpy.data.scenes["EflowRig"]
arm = bpy.data.objects["EflowArmature"]
out = RENDER / "anim"
os.makedirs(out, exist_ok=True)
scn.render.resolution_x, scn.render.resolution_y = 900, 800

STATE_FACE = {"idle": "neutral", "music": "happy", "notify": "surprised", "permission": "wink"}
PICKS = {"idle": [0, 15, 30, 45, 60, 75], "music": [0, 4, 8, 12, 15, 19],
         "notify": [0, 3, 6, 10, 15, 22], "permission": [0, 12, 24, 36, 48, 60]}


def show(state):
    for o in scn.objects:
        if o.name.startswith("face_"):
            hide = not o.name.startswith(f"face_{state}_")
            o.hide_render = hide


for clip, frames in PICKS.items():
    arm.animation_data.action = bpy.data.actions[clip]
    show(STATE_FACE[clip])
    for f in frames:
        scn.frame_set(f)
        scn.render.filepath = f"{out}/{clip}_{f:03d}.png"
        bpy.ops.render.render(write_still=True, scene=scn.name)
arm.animation_data.action = bpy.data.actions["idle"]
result = {"ok": True}
