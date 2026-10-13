import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import bpy  # noqa: E402
from paths import RENDER  # noqa: E402

scn = bpy.data.scenes["EflowRig"]
out = RENDER
os.makedirs(out, exist_ok=True)


def show_state(state):
    for o in scn.objects:
        if o.name.startswith("face_"):
            hide = not o.name.startswith(f"face_{state}_")
            o.hide_render = hide
            o.hide_viewport = hide


scn.render.resolution_x, scn.render.resolution_y = 900, 800
for state in ("neutral", "happy", "surprised", "wink", "blink"):
    show_state(state)
    scn.render.filepath = f"{out}/rest_{state}.png"
    bpy.ops.render.render(write_still=True, scene=scn.name)
result = {"ok": True}
