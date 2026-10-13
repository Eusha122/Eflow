"""Turn build/mascot/render/frames/<clip>/*.png into transparent animated WebPs and a viewer page
(build/mascot/render/preview.html) that plays them on light and dark backgrounds."""
from PIL import Image

from paths import RENDER as R

CLIPS = {"idle": "Idle", "music": "Music", "notify": "Notification", "permission": "AI permission"}
SIZE = (600, 533)
FRAME_MS = 33

for clip in CLIPS:
    frames = [Image.open(p).convert("RGBA").resize(SIZE, Image.LANCZOS)
              for p in sorted((R / "frames" / clip).glob("*.png"))]
    if clip == "notify":                       # one-shot: rest a moment before replaying
        frames += [frames[0]] * 30
    frames[0].save(R / f"anim_{clip}.webp", save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0, lossless=False, quality=92, method=3)

cards = "\n".join(
    f'<figure><img src="anim_{c}.webp" alt="{label}"><figcaption>{label}</figcaption></figure>'
    for c, label in CLIPS.items())
(R / "preview.html").write_text(f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Eflow mascot preview</title>
<style>
  :root {{ color-scheme: light dark; font-family: "Segoe UI Variable Text", "Segoe UI", sans-serif; }}
  body {{ margin: 0; }}
  section {{ padding: 28px 32px; }}
  section.light {{ background: #eef2fa; color: #1b1d27; }}
  section.dark  {{ background: #12141f; color: #d9dcea; }}
  h2 {{ font-weight: 600; font-size: 15px; margin: 0 0 16px; opacity: .7; }}
  .row {{ display: flex; flex-wrap: wrap; gap: 20px; }}
  figure {{ margin: 0; text-align: center; }}
  img {{ width: 300px; height: auto; display: block; }}
  figcaption {{ font-size: 13px; opacity: .65; margin-top: 6px; }}
</style></head><body>
<section class="light"><h2>On a light background</h2><div class="row">{cards}</div></section>
<section class="dark"><h2>On a dark background (the island)</h2><div class="row">{cards}</div></section>
</body></html>
""", encoding="utf-8")
print("preview:", R / "preview.html")
