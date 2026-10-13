"""Prepare the transparent-background Eflow art (art/source) for rigging.

The images were generated with a real transparent background, so no colour un-mixing is
needed (that is what left dark fringes in v1). Alpha is normalised (the generator tops out
at 254) and everything is brought to a common 2048 px canvas so the 1k expression variants
line up with the 2k master. Coordinates elsewhere stay in 1024-px "design units".
"""
import numpy as np
from PIL import Image

from paths import ART_SOURCE as SRC, MATTE as OUT

OUT.mkdir(parents=True, exist_ok=True)
CANVAS = 2048
NAMES = ["master", "happy", "surprised", "wink", "blink"]


def main():
    for name in NAMES:
        img = Image.open(SRC / f"{name}.png").convert("RGBA")
        if img.size != (CANVAS, CANVAS):
            img = img.resize((CANVAS, CANVAS), Image.LANCZOS)
        a = np.asarray(img, np.float32)
        a[..., 3] = np.clip(a[..., 3] * (255.0 / 254.0), 0, 255)
        Image.fromarray(a.round().astype(np.uint8), "RGBA").save(OUT / f"{name}.png")
        print(name, img.size)


if __name__ == "__main__":
    main()
