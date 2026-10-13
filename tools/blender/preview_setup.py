"""Switch the scene to an accurate preview look (runs AFTER export so the GLB is untouched):
unlit image materials + EEVEE, so soft alpha edges blend like they will in the app."""
import bpy

scn = bpy.data.scenes["EflowRig"]
for mat in bpy.data.materials:
    if mat.node_tree is None:
        continue
    nt = mat.node_tree
    tex = next((n for n in nt.nodes if n.type == "TEX_IMAGE"), None)
    out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None)
    if tex is None or out is None:
        continue
    emit = nt.nodes.new("ShaderNodeEmission")
    transp = nt.nodes.new("ShaderNodeBsdfTransparent")
    mix = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(tex.outputs["Color"], emit.inputs["Color"])
    nt.links.new(tex.outputs["Alpha"], mix.inputs["Fac"])
    nt.links.new(transp.outputs["BSDF"], mix.inputs[1])
    nt.links.new(emit.outputs["Emission"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])

scn.render.engine = "BLENDER_EEVEE"
scn.render.film_transparent = True
scn.eevee.taa_render_samples = 16
result = {"ok": True}
