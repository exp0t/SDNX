// deko3d test: a triangle that covers the rectangle g_UboVertex.v[0] (x0, y0, x1, y1 in clip space, y up) and
// gives it uv 0..1 with v = 0 at the top. Same UBO layout as the game library (binding 0 of set 4).
struct BlockVs { float4 v[256]; };
[[vk::binding(0, 4)]] ConstantBuffer<BlockVs> g_UboVertex;

struct Out
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Out main(uint id : SV_VertexID)
{
    // 0: (0, 0), 1: (2, 0), 2: (0, 2) in rectangle units: the rectangle is the lower-left quarter of it
    float2 t = float2((id << 1) & 2, id & 2);
    float4 r = g_UboVertex.v[0];
    Out o;
    o.position = float4(lerp(r.x, r.z, t.x), lerp(r.w, r.y, t.y), 0.0, 1.0);
    o.uv = float2(t.x, t.y);
    return o;
}
