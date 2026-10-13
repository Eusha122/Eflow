"""Print a summary of assets/mascot/eflow.glb (structure the app's loader depends on)."""
import json
import struct
import sys
from pathlib import Path

from paths import GLB

path = Path(sys.argv[1]) if len(sys.argv) > 1 else GLB
d = path.read_bytes()
_, ver, length = struct.unpack("<4sII", d[:12])
clen, _ = struct.unpack("<II", d[12:20])
j = json.loads(d[20:20 + clen])
print(f"{path.name}: glTF {ver}, {length / 1e6:.2f} MB")
print("nodes", len(j["nodes"]), "meshes", len(j["meshes"]), "skins", len(j.get("skins", [])),
      "materials", len(j["materials"]), "images", len(j.get("images", [])))
skin = j["skins"][0]
print("joints:", [j["nodes"][i]["name"] for i in skin["joints"]])
for a in j["animations"]:
    acc = j["accessors"][a["samplers"][0]["input"]]
    print(f"anim {a['name']:<11} channels {len(a['channels']):>3}  t={acc['min'][0]:.2f}..{acc['max'][0]:.2f}s")
print("alphaModes:", sorted({m.get("alphaMode", "OPAQUE") for m in j["materials"]}))
print("meshes:", [m["name"] for m in j["meshes"]])
for im in j.get("images", []):
    print("image", im.get("name"), im.get("mimeType"), j["bufferViews"][im["bufferView"]]["byteLength"] // 1024, "KB")
