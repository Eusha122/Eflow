#pragma once
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "mascot/MascotAsset.h"

namespace eflow {

// GPU resources for the mascot (shared by every island; tied to one D3D11 device).
class MascotRenderer {
public:
    static constexpr int kMaxBones = 16;

    bool Init(ID3D11Device* device, const MascotAsset& asset);

    // Draws the base mesh and the face patches for `face` into `target`, clipped to `clip` (pixels).
    // `viewProj` maps model space to clip space (row-vector convention).
    void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, UINT width, UINT height, const RECT& clip,
              const DirectX::XMFLOAT4X4& viewProj, const std::vector<DirectX::XMFLOAT4X4>& skin,
              const char* face, float opacity);

private:
    template <class T> using Com = Microsoft::WRL::ComPtr<T>;
    struct GpuMesh {
        std::string name;
        Com<ID3D11Buffer> vb, ib;
        UINT indexCount = 0;
        int texture = -1;
    };
    struct FrameConstants {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 bones[kMaxBones];
        float opacity;
        float pad[3];
    };

    std::vector<GpuMesh> meshes_;
    std::vector<Com<ID3D11ShaderResourceView>> textures_;
    Com<ID3D11VertexShader> vs_;
    Com<ID3D11PixelShader> ps_;
    Com<ID3D11InputLayout> layout_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11RasterizerState> raster_;
};

} // namespace eflow
