"""Contact sheets from the Blender preview renders (build/mascot/render)."""
from PIL import Image

from paths import RENDER as R

BG = (20, 22, 34)

states = ["neutral", "happy", "surprised", "wink", "blink"]
strip = Image.new("RGB", (450 * len(states), 400), BG)
for i, n in enumerate(states):
    im = Image.open(R / f"rest_{n}.png").convert("RGBA").resize((450, 400), Image.LANCZOS)
    strip.paste(im, (i * 450, 0), im)
strip.save(R / "_rest_strip.png")

eyes = Image.new("RGB", (580 * 3, 300), BG)
for i, n in enumerate(["happy", "surprised", "wink"]):
    im = Image.open(R / f"rest_{n}.png").convert("RGBA").crop((180, 300, 760, 600)).resize((580, 300), Image.LANCZOS)
    eyes.paste(im, (i * 580, 0), im)
eyes.save(R / "_zoom_eyes.png")

clips = {"idle": [0, 15, 30, 45, 60, 75], "music": [0, 4, 8, 12, 15, 19],
         "notify": [0, 3, 6, 10, 15, 22], "permission": [0, 12, 24, 36, 48, 60]}
sheet = Image.new("RGB", (300 * 6, 267 * len(clips)), BG)
for r, (clip, frames) in enumerate(clips.items()):
    for i, f in enumerate(frames):
        im = Image.open(R / "anim" / f"{clip}_{f:03d}.png").convert("RGBA").resize((300, 267), Image.LANCZOS)
        sheet.paste(im, (i * 300, r * 267), im)
sheet.save(R / "_anim_sheet.png")
print("sheets ok")
