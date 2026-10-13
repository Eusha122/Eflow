"""Keyframe the four Eflow state clips on EflowArmature (run after build_rig.py)."""
import math

import bpy
from mathutils import Matrix, Vector

S = 0.001
scn = bpy.data.scenes["EflowRig"]
arm = bpy.data.objects["EflowArmature"]
FPS = 30


def W(px, py):
    return Vector(((px - 512) * S, 0.0, (512 - py) * S))


def sm(a, b, v):
    t = min(max((v - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def sin(x):
    return math.sin(x)


# Each clip function returns {bone: (ccw_deg, dx_m, dz_m, vertical_scale)}; ccw is as seen by the viewer.
def idle(t):
    T = 3.0
    ph = 2 * math.pi * t / T
    twitch = -7 * sin(math.pi * (t - 1.8) / 0.3) if 1.8 < t < 2.1 else 0.0
    return {
        "head": (0.4 * sin(ph), 0, 0.003 * sin(ph), 1 + 0.008 * sin(ph)),
        "earL": (-2.5 * sin(ph + 0.6) + twitch, 0, 0, 1),
        "earR": (2.5 * sin(ph + 1.4), 0, 0, 1),
        "antenna": (4 * sin(2 * ph + 0.3), 0, 0, 1),
        "tail": (5 * sin(ph + 2.0), 0, 0, 1),
    }


def music(t):
    b = (1 - math.cos(2 * math.pi * t / 0.5)) / 2
    k = 2 * math.pi * t / 0.5
    return {
        "head": (3.5 * sin(2 * math.pi * t), 0, -0.010 * b, 1 - 0.03 * b),
        "earL": (-7 * sin(k - 0.8), 0, 0, 1),
        "earR": (7 * sin(k - 0.4), 0, 0, 1),
        "antenna": (11 * sin(k - 1.5), 0, 0, 1),
        "pawL": (0, 0, 0.008 * max(0, sin(2 * math.pi * t)), 1),
        "pawR": (0, 0, 0.008 * max(0, -sin(2 * math.pi * t)), 1),
        "tail": (12 * sin(k - 2.2), 0, 0, 1),
    }


def notify(t):
    D = 0.9
    win = 1 - sm(0.7, D, t)
    dz = 0.05 * math.exp(-3.5 * t) * sin(2 * math.pi * t / 0.6) * win
    p = sin(math.pi * t / D) ** 0.7 if t < D else 0.0
    return {
        "head": (0, 0, dz, 1 + 0.8 * dz),
        "earL": (-14 * p + 3 * sin(2 * math.pi * 6 * t) * p, 0, 0, 1),
        "earR": (14 * p + 3 * sin(2 * math.pi * 6 * t + 1) * p, 0, 0, 1),
        "antenna": (12 * sin(2 * math.pi * 4 * t) * p, 0, 0, 1),
        "tail": (10 * sin(2 * math.pi * 5 * t) * p, 0, 0, 1),
    }


def permission(t):
    T = 2.4
    ph = 2 * math.pi * t / T
    return {
        "head": (6 + 2 * sin(ph), 0, 0.004 * sin(ph + 1), 1),
        "earR": (10 + 4 * sin(2 * ph), 0, 0, 1),
        "earL": (4 + 2 * sin(ph), 0, 0, 1),
        "antenna": (10 * sin(2 * math.pi * t / 1.2), 0, 0, 1),
        "pawR": (0, 0, 0.010 * max(0, sin(2 * math.pi * t / 1.2)), 1),
        "tail": (8 * sin(2 * math.pi * t / 1.2 + 1.0), 0, 0, 1),
    }


CLIPS = {            # name: (function, duration s, loop)
    "idle": (idle, 3.0, True),
    "music": (music, 1.0, True),
    "notify": (notify, 0.9, False),
    "permission": (permission, 2.4, True),
}

ORDER = ["root", "head", "earL", "earR", "antenna", "pawL", "pawR", "tail"]
PIVOT = {n: arm.data.bones[n].head_local.copy() for n in ORDER}
REST = {n: arm.data.bones[n].matrix_local.copy() for n in ORDER}


def basis_for(name, ccw, dx, dz, sz):
    p = PIVOT[name]
    d = (Matrix.Translation((dx, 0, dz)) @ Matrix.Translation(p)
         @ Matrix.Rotation(-math.radians(ccw), 4, "Y") @ Matrix.Diagonal((1, 1, sz, 1))
         @ Matrix.Translation(-p))
    return REST[name].inverted() @ d @ REST[name]


if arm.animation_data is None:
    arm.animation_data_create()
for a in [a for a in bpy.data.actions if a.name in CLIPS]:
    bpy.data.actions.remove(a)

summary = {}
for name, (fn, dur, loop) in CLIPS.items():
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    arm.animation_data.action = action
    n = int(round(dur * FPS))
    frames = range(0, n + 1) if (loop or True) else range(0, n)
    for f in frames:
        pose = fn((f % n if loop else f) / FPS) if loop else fn(f / FPS)
        for bone in ORDER:
            ccw, dx, dz, sz = pose.get(bone, (0, 0, 0, 1))
            loc, rot, scale = basis_for(bone, ccw, dx, dz, sz).decompose()
            pb = arm.pose.bones[bone]
            pb.rotation_mode = "QUATERNION"
            pb.location, pb.rotation_quaternion, pb.scale = loc, rot, scale
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)
            pb.keyframe_insert("scale", frame=f)
    action.frame_range = (0, n)
    summary[name] = {"frames": n, "loop": loop}

arm.animation_data.action = bpy.data.actions["idle"]
scn.frame_start, scn.frame_end = 0, 90
result = summary
