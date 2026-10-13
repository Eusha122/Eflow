// Skinned, textured, premultiplied-alpha quads for the mascot. Row-vector convention (mul(v, M)),
// matching DirectXMath on the CPU side.

cbuffer Frame : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 bones[16];
    float opacity;
    float3 pad;
};

struct VSIn
{
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(VSIn i)
{
    float4x4 skin = bones[i.joints.x] * i.weights.x + bones[i.joints.y] * i.weights.y +
                    bones[i.joints.z] * i.weights.z + bones[i.joints.w] * i.weights.w;
    VSOut o;
    o.pos = mul(mul(float4(i.pos, 1.0), skin), viewProj);
    o.uv = i.uv;
    return o;
}

Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 ps_main(VSOut i) : SV_Target
{
    return tex.Sample(samp, i.uv) * opacity;   // texture is premultiplied BGRA
}
