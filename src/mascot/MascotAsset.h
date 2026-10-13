#pragma once
#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eflow {

// CPU-side copy of assets/mascot/eflow.glb: everything the renderer and animator need, decoded once.
// Model space is glTF's: x right, y up, 1 unit = 1000 design px of the 1024-px art canvas, origin at
// the canvas centre.
struct MascotAsset {
    struct Vertex {
        float pos[3];
        float uv[2];
        uint8_t joints[4];
        float weights[4];
    };
    struct Texture {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> bgra;              // premultiplied BGRA8, top-down
    };
    struct Mesh {
        std::string name;                       // "eflow_base" or "face_<expression>_<part>"
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        int texture = -1;
    };
    struct TRS {
        DirectX::XMFLOAT3 t{0, 0, 0};
        DirectX::XMFLOAT4 r{0, 0, 0, 1};
        DirectX::XMFLOAT3 s{1, 1, 1};
    };
    struct Node {
        std::string name;
        int parent = -1;
        TRS rest;
    };
    struct Channel {
        int node = -1;
        enum class Path { Translation, Rotation, Scale } path{};
        bool step = false;                      // STEP vs LINEAR interpolation
        std::vector<float> times;
        std::vector<float> values;              // 3 or 4 floats per key
    };
    struct Clip {
        std::string name;
        float duration = 0;
        std::vector<Channel> channels;
    };

    std::vector<Node> nodes;                    // in glTF order; parents always resolvable
    std::vector<int> joints;                    // skin joint index -> node index
    std::vector<DirectX::XMFLOAT4X4> inverseBind;
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
    std::vector<Clip> clips;

    // Loads and decodes the file. Textures are downscaled by `textureScale` (the art is authored at
    // 2048 px but shown at a few dozen DIPs). Returns false and fills `error` on failure.
    bool Load(const std::filesystem::path& file, float textureScale, std::string& error);

    int FindClip(const std::string& name) const;
};

} // namespace eflow
