import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import bpy  # noqa: E402
from paths import ASSETS, GLB  # noqa: E402

scn = bpy.data.scenes["EflowRig"]
vl = bpy.context.view_layer
os.makedirs(ASSETS, exist_ok=True)

for o in scn.objects:                       # every patch exported; the app decides which one is visible
    o.hide_render = False
    o.hide_viewport = False
    o.hide_set(False)
    o.select_set(o.name.startswith(("eflow", "Eflow", "face_")))
vl.objects.active = bpy.data.objects["EflowArmature"]

if True:
    bpy.ops.export_scene.gltf(
        filepath=str(GLB), export_format="GLB", use_selection=True,
        export_animation_mode="ACTIONS", export_skins=True, export_materials="EXPORT",
        export_yup=True, export_apply=False, export_cameras=False, export_lights=False,
        export_force_sampling=True, export_optimize_animation_size=False)
result = {"size": os.path.getsize(GLB)}
