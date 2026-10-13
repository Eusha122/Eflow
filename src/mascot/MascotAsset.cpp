#include "mascot/MascotAsset.h"

#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/Image.h"

namespace eflow {
namespace {

struct DataDeleter {
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};

std::vector<float> Unpack(const cgltf_accessor* a) {
    std::vector<float> v(cgltf_accessor_unpack_floats(a, nullptr, 0));
    cgltf_accessor_unpack_floats(a, v.data(), v.size());
    return v;
}

} // namespace

bool MascotAsset::Load(const std::filesystem::path& file, float textureScale, std::string& error) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    const std::string path = file.string();
    if (cgltf_parse_file(&options, path.c_str(), &raw) != cgltf_result_success) {
        error = "cannot parse " + path;
        return false;
    }
    std::unique_ptr<cgltf_data, DataDeleter> data(raw);
    if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) {
        error = "invalid glTF: " + path;
        return false;
    }
    if (data->skins_count != 1) {
        error = "expected exactly one skin";
        return false;
    }
    auto nodeIndex = [&](const cgltf_node* n) { return n ? static_cast<int>(n - data->nodes) : -1; };

    // Nodes (rest pose).
    nodes.resize(data->nodes_count);
    for (size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& n = data->nodes[i];
        Node& out = nodes[i];
        out.name = n.name ? n.name : "";
        out.parent = nodeIndex(n.parent);
        if (n.has_translation) out.rest.t = {n.translation[0], n.translation[1], n.translation[2]};
        if (n.has_rotation) out.rest.r = {n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]};
        if (n.has_scale) out.rest.s = {n.scale[0], n.scale[1], n.scale[2]};
    }

    // Skin.
    const cgltf_skin& skin = data->skins[0];
    joints.clear();
    for (size_t j = 0; j < skin.joints_count; ++j) joints.push_back(nodeIndex(skin.joints[j]));
    const std::vector<float> ibm = Unpack(skin.inverse_bind_matrices);
    inverseBind.resize(joints.size());
    // glTF stores column-major matrices for column vectors; read row-major they are exactly the
    // row-vector (DirectXMath) form, so no transpose is needed.
    for (size_t j = 0; j < joints.size(); ++j) std::copy_n(&ibm[j * 16], 16, &inverseBind[j].m[0][0]);

    // Textures: one per image, in glTF order.
    textures.assign(data->images_count, {});
    for (size_t i = 0; i < data->images_count; ++i) {
        const cgltf_image& img = data->images[i];
        ImageBGRA decoded;
        if (!img.buffer_view || !DecodeImage(static_cast<const uint8_t*>(cgltf_buffer_view_data(img.buffer_view)),
                                             img.buffer_view->size, textureScale, 0, decoded)) {
            error = "cannot decode image " + std::to_string(i);
            return false;
        }
        textures[i] = {decoded.width, decoded.height, std::move(decoded.pixels)};
    }

    // Meshes: one primitive each (as exported).
    meshes.clear();
    for (size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& src = data->meshes[m];
        if (src.primitives_count != 1) continue;
        const cgltf_primitive& prim = src.primitives[0];
        Mesh mesh;
        mesh.name = src.name ? src.name : "";
        const cgltf_accessor *pos = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& at = prim.attributes[a];
            if (at.type == cgltf_attribute_type_position) pos = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
            else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
            else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
        }
        if (!pos || !uv || !jnt || !wgt || !prim.indices) {
            error = "mesh " + mesh.name + " lacks position/uv/joints/weights/indices";
            return false;
        }
        mesh.vertices.resize(pos->count);
        for (size_t v = 0; v < pos->count; ++v) {
            Vertex& vx = mesh.vertices[v];
            cgltf_accessor_read_float(pos, v, vx.pos, 3);
            cgltf_accessor_read_float(uv, v, vx.uv, 2);
            cgltf_uint j4[4]{};
            cgltf_accessor_read_uint(jnt, v, j4, 4);
            for (int k = 0; k < 4; ++k) vx.joints[k] = static_cast<uint8_t>(j4[k]);
            cgltf_accessor_read_float(wgt, v, vx.weights, 4);
        }
        mesh.indices.resize(prim.indices->count);
        for (size_t k = 0; k < prim.indices->count; ++k)
            mesh.indices[k] = static_cast<uint16_t>(cgltf_accessor_read_index(prim.indices, k));
        if (prim.material && prim.material->has_pbr_metallic_roughness) {
            const cgltf_texture* tex = prim.material->pbr_metallic_roughness.base_color_texture.texture;
            if (tex && tex->image) mesh.texture = static_cast<int>(tex->image - data->images);
        }
        meshes.push_back(std::move(mesh));
    }

    // Animation clips.
    clips.clear();
    for (size_t a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        Clip clip;
        clip.name = anim.name ? anim.name : "";
        for (size_t c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            Channel out;
            out.node = nodeIndex(ch.target_node);
            switch (ch.target_path) {
            case cgltf_animation_path_type_translation: out.path = Channel::Path::Translation; break;
            case cgltf_animation_path_type_rotation: out.path = Channel::Path::Rotation; break;
            case cgltf_animation_path_type_scale: out.path = Channel::Path::Scale; break;
            default: continue;
            }
            out.step = ch.sampler->interpolation == cgltf_interpolation_type_step;
            out.times = Unpack(ch.sampler->input);
            out.values = Unpack(ch.sampler->output);
            if (!out.times.empty()) clip.duration = std::max(clip.duration, out.times.back());
            clip.channels.push_back(std::move(out));
        }
        clips.push_back(std::move(clip));
    }
    return true;
}

int MascotAsset::FindClip(const std::string& name) const {
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == name) return static_cast<int>(i);
    return -1;
}

} // namespace eflow
