"""Cut feathered face patches (eyes+brows, mouth) from each expression variant and
re-compose every expression onto the master to verify the seams (on light and dark)."""
import json

import cv2
import numpy as np
from PIL import Image

from paths import MATTE as M, PATCHES as P

P.mkdir(parents=True, exist_ok=True)

K = 2  # matte canvas is 2048 px; coordinates below are 1024-px design units
# x0, y0, x1, y1 in design units
PARTS = {
    "eyeL": (235, 395, 410, 690),   # viewer's left eye + brow (top leaves room for a raised brow)
    "eyeR": (490, 395, 670, 690),   # viewer's right eye + brow
    "mouth": (395, 598, 505, 692),
}
STATES = ["neutral", "happy", "surprised", "wink", "blink"]
SRC = {"neutral": "master"}
FEATHER = 12 * K


def soft_mask(w, h):
    # Round top (rectangle corners would clip hair strands crossing the eye area), squarer bottom
    # so the variant fully covers the master's open eye, which sits lower than closed eyes.
    yy, xx = np.mgrid[:h, :w].astype(np.float32)
    dx = np.abs(xx - w / 2) / (w / 2 - FEATHER)
    dy = (yy - h / 2) / (h / 2 - FEATHER)
    p = np.where(dy < 0, 2.0, 4.0)
    m = ((dx ** p + np.abs(dy) ** p) <= 1).astype(np.float32)
    return cv2.GaussianBlur(m, (0, 0), FEATHER / 2.2)


def load(name):
    return Image.open(M / f"{name}.png").convert("RGBA")


base = load("master")
meta = {"size": [1024, 1024], "parts": PARTS, "states": STATES}
renders = []
for st in STATES:
    src = load(SRC.get(st, st))
    comp = base.copy()
    for part, box in PARTS.items():
        x0, y0, x1, y1 = (v * K for v in box)
        patch = np.asarray(src.crop((x0, y0, x1, y1)), np.float32).copy()
        patch[..., 3] *= soft_mask(x1 - x0, y1 - y0)
        img = Image.fromarray(patch.round().astype(np.uint8), "RGBA")
        img.save(P / f"{st}_{part}.png")
        comp.alpha_composite(img, (x0, y0))
    row = []
    for bgc in ((235, 240, 250), (20, 22, 34)):
        bg = Image.new("RGBA", comp.size, bgc + (255,))
        bg.alpha_composite(comp)
        row.append(bg.crop((200 * K, 380 * K, 720 * K, 720 * K)).resize((520, 340), Image.LANCZOS))
    renders.append(row)

strip = Image.new("RGB", (520 * len(renders), 680))
for i, (light, dark) in enumerate(renders):
    strip.paste(light.convert("RGB"), (i * 520, 0))
    strip.paste(dark.convert("RGB"), (i * 520, 340))
strip.save(M / "_faces_recomposed.png")
(P / "patches.json").write_text(json.dumps(meta, indent=1))
print("ok", len(STATES), "states")
