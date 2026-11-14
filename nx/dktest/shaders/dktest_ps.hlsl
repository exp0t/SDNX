// deko3d test: the slot 0 texture times the tints of the pixel and shared UBOs (bindings 1 and 2 of set 4, as in
// the game library). A wrong UBO binding or layout gives black, a wrong texture binding gives black or garbage.
struct BlockPs { float4 v[224]; };
struct SharedBlock { float4 v[59]; };
[[vk::binding(1, 4)]] ConstantBuffer<BlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<SharedBlock> g_UboShared;
Texture2D<float4> g_dkT2D_0 : register(t0, space0);
SamplerState g_dkS_0 : register(s0, space3);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return g_dkT2D_0.Sample(g_dkS_0, uv) * g_UboPixel.v[0] * g_UboShared.v[58];
}
