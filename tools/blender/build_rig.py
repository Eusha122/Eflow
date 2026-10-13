"""Build the Eflow 2.5D puppet rig in Blender (run through tools/blender/run.py).

Creates a fresh scene "EflowRig":
  * eflow_base      - subdivided image mesh of the tail-less master head, skinned to 7 bones
  * face_<s>_<part> - rigid face patches (eyes+brows, mouth) skinned 100% to `head`
  * armature "EflowArmature" with bones root/head/earL/earR/antenna/pawL/pawR
Animation clips are added by build_anim.py.
"""
import contextlib
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from paths import MATTE, PATCHES  # noqa: E402

S = 0.001          # 1 px of the 1024 working canvas = 1 mm
CANVAS = 1024


def W(px, py):
    """canvas pixel (x right, y down) -> world (x, 0, z)"""
    return Vector(((px - 512) * S, 0.0, (512 - py) * S))


# ---------------------------------------------------------------- clean slate
for sc in list(bpy.data.scenes):
    if sc.name == "EflowRig":
        bpy.data.scenes.remove(sc)
for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.armatures, bpy.data.materials,
             bpy.data.actions, bpy.data.cameras):
    for item in list(coll):
        if item.name.startswith(("eflow", "Eflow", "face_", "EflowCam")):
            coll.remove(item)

# Headless only (blender --background --factory-startup --python pipeline.py): running operators from
# the live window over the MCP socket crashed Blender twice.
assert bpy.app.background, "run through tools/blender/pipeline.ps1"
scn = bpy.context.scene
scn.name = "EflowRig"
for o in list(scn.objects):
    bpy.data.objects.remove(o)
view_layer = bpy.context.view_layer
col = bpy.data.collections.new("Eflow")
scn.collection.children.link(col)


def override(obj=None):
    if obj is not None:
        for o in view_layer.objects:
            o.select_set(False)
        obj.select_set(True)
        view_layer.objects.active = obj
    return contextlib.nullcontext()


# ---------------------------------------------------------------- materials
def image_material(name, path):
    img = bpy.data.images.load(str(path), check_existing=False)
    img.alpha_mode = "STRAIGHT"
    mat = bpy.data.materials.new(name)
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    tex.interpolation = "Linear"
    tex.extension = "CLIP"
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    bsdf.inputs["Roughness"].default_value = 1.0
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    try:
        mat.surface_render_method = "BLENDED"
    except Exception:
        mat.blend_method = "BLEND"
    return mat, img


# ---------------------------------------------------------------- armature
BONES = {
    # name: (head px, tail px, parent)
    "root":    ((512, 790), (512, 740), None),
    "head":    ((512, 720), (512, 480), "root"),
    "earL":    ((245, 430), (150, 270), "head"),
    "earR":    ((725, 430), (825, 255), "head"),
    "antenna": ((506, 236), (506, 140), "head"),
    "pawL":    ((345, 650), (345, 700), "root"),
    "pawR":    ((583, 650), (583, 700), "root"),
    "tail":    ((700, 760), (900, 560), "root"),
}

arm_data = bpy.data.armatures.new("EflowArmature")
arm = bpy.data.objects.new("EflowArmature", arm_data)
col.objects.link(arm)
view_layer.objects.active = arm
with override(arm):
    bpy.ops.object.mode_set(mode="EDIT")
    eb = arm_data.edit_bones
    for name, (h, t, parent) in BONES.items():
        b = eb.new(name)
        b.head, b.tail = W(*h), W(*t)
        b.roll = 0.0
    for name, (_, _, parent) in BONES.items():
        if parent:
            eb[name].parent = eb[parent]
    bpy.ops.object.mode_set(mode="OBJECT")


# ---------------------------------------------------------------- base mesh
base_img_path = MATTE / "master.png"
mat_base, img_base = image_material("eflow_base_mat", base_img_path)

px = np.empty(img_base.size[0] * img_base.size[1] * 4, np.float32)
img_base.pixels.foreach_get(px)
alpha = px.reshape(img_base.size[1], img_base.size[0], 4)[::-1, :, 3]   # row 0 = top
KA = img_base.size[0] / CANVAS                                          # texture px per design unit

X0, X1, Y0, Y1, STEP = 60, 975, 110, 815, 15
cols_n, rows_n = (X1 - X0) // STEP, (Y1 - Y0) // STEP
verts, uvs, faces = [], [], []
index = {}
for r in range(rows_n + 1):
    for c in range(cols_n + 1):
        index[(r, c)] = None
for r in range(rows_n):
    for c in range(cols_n):
        x, y = X0 + c * STEP, Y0 + r * STEP
        ya, yb, xa, xb = (int(v * KA) for v in (max(y - 2, 0), y + STEP + 2, max(x - 2, 0), x + STEP + 2))
        if alpha[ya:yb, xa:xb].max() < 0.01:
            continue
        quad = []
        for dr, dc in ((0, 0), (0, 1), (1, 1), (1, 0)):
            key = (r + dr, c + dc)
            if index[key] is None:
                index[key] = len(verts)
                vx, vy = X0 + key[1] * STEP, Y0 + key[0] * STEP
                verts.append((vx, vy))
            quad.append(index[key])
        faces.append(quad[::-1])   # winding so the normal faces -Y (camera)

mesh = bpy.data.meshes.new("eflow_base")
co = [tuple(W(x, y)) for x, y in verts]
mesh.from_pydata(co, [], faces)
mesh.update()
uv_layer = mesh.uv_layers.new(name="UVMap")
for poly in mesh.polygons:
    for li in poly.loop_indices:
        vx, vy = verts[mesh.loops[li].vertex_index]
        uv_layer.data[li].uv = (vx / CANVAS, 1 - vy / CANVAS)
mesh.materials.append(mat_base)
base = bpy.data.objects.new("eflow_base", mesh)
col.objects.link(base)

# ---------------------------------------------------------------- weights
def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


P = np.array(verts, np.float32)           # canvas px
w = {n: np.zeros(len(P), np.float32) for n in BONES}


def ear_weight(head, tail, reach, base_ramp):
    h, t = np.array(head, np.float32), np.array(tail, np.float32)
    axis = (t - h) / np.linalg.norm(t - h)
    along = (P - h) @ axis                       # px along the ear
    side = np.abs((P - h) @ np.array([-axis[1], axis[0]]))
    length = np.linalg.norm(t - h)
    ramp = smooth((along + base_ramp) / (length * 0.75))        # 0 at the base -> 1 toward the tip
    width = smooth(1 - (side - reach) / 60.0)                   # soft lateral cutoff
    beyond = smooth(1 - (along - length * 1.25) / 40.0)
    return np.clip(ramp * width * beyond, 0, 1)


w["earL"] = ear_weight(*BONES["earL"][:2], reach=105, base_ramp=-10)
w["earR"] = ear_weight(*BONES["earR"][:2], reach=105, base_ramp=-10)
# keep the ears off the face: nothing below the cheek line
cheek = smooth(1 - (P[:, 1] - 470) / 40.0)
w["earL"] *= cheek
w["earR"] *= cheek

ah, at = np.array(BONES["antenna"][0], np.float32), np.array(BONES["antenna"][1], np.float32)
w["antenna"] = smooth((ah[1] - P[:, 1]) / (ah[1] - at[1])) * smooth(1 - (np.abs(P[:, 0] - ah[0]) - 70) / 40.0)


def paw_weight(center, rx, ry):
    d = np.sqrt(((P[:, 0] - center[0]) / rx) ** 2 + ((P[:, 1] - center[1]) / ry) ** 2)
    return smooth(1.6 - d)


w["pawL"] = paw_weight((345, 705), 110, 70) * smooth((P[:, 1] - 640) / 30.0)
w["pawR"] = paw_weight((583, 705), 110, 70) * smooth((P[:, 1] - 640) / 30.0)

TAIL_POLY = np.array([(800, 515), (1010, 515), (1010, 830), (676, 830), (676, 748), (688, 728), (712, 708),
                      (745, 695), (770, 667), (783, 643), (770, 622), (783, 597), (775, 580), (783, 558),
                      (793, 530)], np.float32)


def inside(pts, poly):
    """Vectorised even-odd point-in-polygon."""
    x, y = pts[:, 0][:, None], pts[:, 1][:, None]
    x0, y0 = poly[:, 0][None], poly[:, 1][None]
    x1, y1 = np.roll(poly[:, 0], -1)[None], np.roll(poly[:, 1], -1)[None]
    cross = ((y0 > y) != (y1 > y)) & (x < (x1 - x0) * (y - y0) / (y1 - y0 + 1e-9) + x0)
    return (cross.sum(axis=1) % 2).astype(np.float32)


# soft tail region: average of the polygon test over a small neighbourhood, ramped from the hidden base
soft = np.mean([inside(P + (dx, dy), TAIL_POLY) for dx in (-16, -8, 0, 8, 16) for dy in (-16, -8, 0, 8, 16)], axis=0)
th, tt = np.array(BONES["tail"][0], np.float32), np.array(BONES["tail"][1], np.float32)
t_axis = (tt - th) / np.linalg.norm(tt - th)
w["tail"] = soft * smooth(((P - th) @ t_axis + 20) / (np.linalg.norm(tt - th) * 0.5))

MOVERS = ("earL", "earR", "antenna", "pawL", "pawR", "tail")
over = np.maximum(sum(w[n] for n in MOVERS), 1.0)
for n in MOVERS:
    w[n] = w[n] / over
rest = np.clip(1 - sum(w[n] for n in MOVERS), 0, 1)
root_pull = smooth((P[:, 1] - 700) / 90.0)       # lower edge anchors to the ledge
w["root"] = rest * root_pull
w["head"] = rest * (1 - root_pull)

for name in BONES:
    vg = base.vertex_groups.new(name=name)
    for i, val in enumerate(w[name]):
        if val > 0.001:
            vg.add([i], float(val), "REPLACE")
mod = base.modifiers.new("Armature", "ARMATURE")
mod.object = arm
base.parent = arm

# ---------------------------------------------------------------- face patches
meta = json.loads((PATCHES / "patches.json").read_text())
patch_objs = []
for state in meta["states"]:
    if state == "neutral":
        continue
    for part, (x0, y0, x1, y1) in meta["parts"].items():
        name = f"face_{state}_{part}"
        mat, _ = image_material(name + "_mat", PATCHES / f"{state}_{part}.png")
        m = bpy.data.meshes.new(name)
        a, b = W(x0, y0), W(x1, y1)
        y = -0.002
        m.from_pydata([(a.x, y, a.z), (b.x, y, a.z), (b.x, y, b.z), (a.x, y, b.z)], [], [(3, 2, 1, 0)])
        uv = m.uv_layers.new(name="UVMap")
        corner_uv = ((0, 1), (1, 1), (1, 0), (0, 0))      # per VERTEX: top-left, top-right, bottom-right, bottom-left
        for loop in m.loops:
            uv.data[loop.index].uv = corner_uv[loop.vertex_index]
        m.materials.append(mat)
        o = bpy.data.objects.new(name, m)
        col.objects.link(o)
        vg = o.vertex_groups.new(name="head")
        vg.add([0, 1, 2, 3], 1.0, "REPLACE")
        am = o.modifiers.new("Armature", "ARMATURE")
        am.object = arm
        o.parent = arm
        patch_objs.append(o)

# ---------------------------------------------------------------- camera + render preset
cam_data = bpy.data.cameras.new("EflowCam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = 0.98
cam = bpy.data.objects.new("EflowCam", cam_data)
col.objects.link(cam)
cam.location = (0.005, -3.0, 0.05)
cam.rotation_euler = (math.radians(90), 0, 0)
scn.camera = cam
scn.render.engine = "BLENDER_WORKBENCH"
scn.render.film_transparent = True
scn.render.resolution_x, scn.render.resolution_y = 900, 800
sh = scn.display.shading
sh.light, sh.color_type, sh.show_shadows = "FLAT", "TEXTURE", False
scn.render.image_settings.media_type = "IMAGE"
scn.render.image_settings.file_format = "PNG"
scn.render.image_settings.color_mode = "RGBA"
scn.render.fps = 30
scn.view_settings.view_transform = "Standard"
scn.view_settings.look = "None"
scn.display_settings.display_device = "sRGB"

result = {"verts": len(verts), "faces": len(faces), "patches": len(patch_objs),
          "weights_max": {n: float(w[n].max()) for n in BONES}}
