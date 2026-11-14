// SDNX (Sleeping Dogs port): run-time vertex fetch swizzles, see fetchApply / g_FetchSwizzle.
#define SDNX_FETCH_SWIZZLE 1
#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// MASSEFFECT: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTS_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation.
#define SPEC_CONSTANT_INV_TEX_SIZE    (1 << 9)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing. It was not needed
 * at all: the app already knows the function when it creates the pipeline (it comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
// Bits 15, 19 and 23 are reserved (unused here; left free for the app's own use).
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

#ifdef SDNX_DEKO3D
// SDNX deko3d (uam, Mesa's GLSL 4.60 front end): no push constants, no buffer references, no 64-bit integers and
// no bindless heaps. Constants come only through the UBOs (MASSEFFECT_UBO is true), so the raw-load branches are
// dead; this stand-in keeps them compiling with 32-bit addresses. Each Xenos texture slot S is its own texture
// (g_dkT2D_S, g_dkT3D_S, g_dkTCube_S) and sampler (g_dkS_S); the texture bindings are given after SPIRV-Cross.
struct PushConstants
{
    uint VertexShaderConstants;
    uint PixelShaderConstants;
    uint SharedConstants;
};

static PushConstants g_PushConstants;
template<typename T> T sdnxNoLoad(uint address) { return (T)0; }
template<typename T> T sdnxNoLoad(uint address, uint alignment) { return (T)0; }
#define SDNX_RAW_LOAD sdnxNoLoad
#else
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define SDNX_RAW_LOAD vk::RawBufferLoad
#endif

// MASSEFFECT: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct MassEffectBlockVs { float4 v[256]; };
struct MassEffectBlockPs { float4 v[224]; };
#ifdef SDNX_FETCH_SWIZZLE
// SDNX: the block grows to 944 bytes: _InvSize of slots 16-31 reach byte 616, the vertex fetch swizzle
// codes take bytes 624-687 (g_FetchSwizzle), the pulled vertex fetches bytes 688-943 (sdnxPullFetch).
struct MassEffectSharedBlock { float4 v[59]; };
#else
struct MassEffectSharedBlock { float4 v[31]; };
#endif
[[vk::binding(0, 4)]] ConstantBuffer<MassEffectBlockVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<MassEffectBlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<MassEffectSharedBlock> g_UboShared;
#ifdef SDNX_DEKO3D
#define MASSEFFECT_UBO true
#else
#define MASSEFFECT_UBO ((g_SpecConstants & SPEC_CONSTANT_CONSTANTS_UBO) != 0)
#endif
#define MASSEFFECT_SHARED_UINT(B)  asuint(g_UboShared.v[(B) / 16][((B) % 16) / 4])
#define MASSEFFECT_SHARED_FLOAT(B) g_UboShared.v[(B) / 16][((B) % 16) / 4]

#define g_Booleans                 (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(256) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 256))
#define g_SwappedTexcoords         (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(260) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(264), MASSEFFECT_SHARED_FLOAT(268)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (MASSEFFECT_UBO ? MASSEFFECT_SHARED_FLOAT(272) : SDNX_RAW_LOAD<float>(g_PushConstants.SharedConstants + 272))
// MASSEFFECT: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(276) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 276))
// MASSEFFECT: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(280), MASSEFFECT_SHARED_FLOAT(284)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(288), MASSEFFECT_SHARED_FLOAT(292)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 288))
// MASSEFFECT: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
#define g_InputRemap(LOC)          (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(296 + (LOC) * 4) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))
// SDNX: the 12-bit destination swizzle Direct3D wrote into vertex fetch number K of the loaded vertex
// shader (K = order of first emission, listed by the "SDNX_FETCH_ORDER" comment at the end of the HLSL),
// two per word: bits 0-11 even K, bits 16-27 odd K, at byte 624 + (K / 2) * 4 of the shared constants.
#define g_FetchSwizzle(K)          (((MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(624 + ((K) / 2) * 4) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 624 + ((K) / 2) * 4)) >> (((K) & 1) * 16)) & 0xFFF)

#ifdef SDNX_FETCH_SWIZZLE
// SDNX: vertex shaders that compute the index of their vertex fetches (hand-made instancing: the index is read
// from a stream first) cannot use vertex attributes. The renderer copies the fetched buffers into its upload
// buffer (binding 3) and gives fetch K, at byte 688 + K * 16 of the shared block: the byte offset of its first
// element, the stride in bytes, the VkFormat Direct3D's fetch word maps to, the number of vertices. The data is
// in host word order (as for vertex attributes), so the components are read like Vulkan reads that format.
[[vk::binding(3, 4)]] ByteAddressBuffer g_PullData;

float sdnxSnorm(int v, int bits)
{
    return max(float(v) / float((1 << (bits - 1)) - 1), -1.0);
}

float4 sdnxPullFetch(uint k, float indexValue, uint rounded)
{
    uint offset = MASSEFFECT_SHARED_UINT(688 + k * 16), stride = MASSEFFECT_SHARED_UINT(692 + k * 16);
    uint format = MASSEFFECT_SHARED_UINT(696 + k * 16), count = MASSEFFECT_SHARED_UINT(700 + k * 16);
    // Bits 16-21: the fetch's exp_adjust (signed): the result is scaled by 2^exp_adjust, as on Xenos.
    int expAdjust = int(format << 10) >> 26;
    format &= 0xFFFF;
    float4 r = float4(0.0, 0.0, 0.0, 1.0);
    if (count == 0)
        return r;
    uint index = uint(max(floor(indexValue + (rounded != 0 ? 0.5 : 0.0)), 0.0));
    uint a = offset + min(index, count - 1) * stride;
    switch (format)
    {
    case 100: r.x = asfloat(g_PullData.Load(a)); break;                      // R32_SFLOAT
    case 103: r.xy = asfloat(g_PullData.Load2(a)); break;                    // R32G32_SFLOAT
    case 106: r.xyz = asfloat(g_PullData.Load3(a)); break;                   // R32G32B32_SFLOAT
    case 109: r = asfloat(g_PullData.Load4(a)); break;                       // R32G32B32A32_SFLOAT
    case 98: r.x = float(g_PullData.Load(a)); break;                         // R32_UINT
    case 99: r.x = float(asint(g_PullData.Load(a))); break;                  // R32_SINT
    case 101: r.xy = float2(g_PullData.Load2(a)); break;                     // R32G32_UINT
    case 102: r.xy = float2(asint(g_PullData.Load2(a))); break;              // R32G32_SINT
    case 107: r = float4(g_PullData.Load4(a)); break;                        // R32G32B32A32_UINT
    case 108: r = float4(asint(g_PullData.Load4(a))); break;                 // R32G32B32A32_SINT
    case 83:                                                                 // R16G16_SFLOAT
    {
        uint w = g_PullData.Load(a);
        r.xy = f16tof32(uint2(w, w >> 16));
        break;
    }
    case 97:                                                                 // R16G16B16A16_SFLOAT
    {
        uint2 w = g_PullData.Load2(a);
        r = f16tof32(uint4(w.x, w.x >> 16, w.y, w.y >> 16));
        break;
    }
    case 37: case 38: case 39: case 40:                                      // R8G8B8A8 UNORM SNORM USCALED SSCALED
    {
        uint w = g_PullData.Load(a);
        uint4 u = uint4(w, w >> 8, w >> 16, w >> 24) & 0xFF;
        int4 s = int4(u << 24) >> 24;
        if (format == 37) r = float4(u) / 255.0;
        else if (format == 38) r = float4(sdnxSnorm(s.x, 8), sdnxSnorm(s.y, 8), sdnxSnorm(s.z, 8), sdnxSnorm(s.w, 8));
        else if (format == 39) r = float4(u);
        else r = float4(s);
        break;
    }
    case 64: case 65: case 66: case 67:                                      // A2B10G10R10 UNORM SNORM USCALED SSCALED
    {
        uint w = g_PullData.Load(a);
        uint4 u = uint4(w & 0x3FF, (w >> 10) & 0x3FF, (w >> 20) & 0x3FF, w >> 30);
        int4 s = int4(int(u.x << 22) >> 22, int(u.y << 22) >> 22, int(u.z << 22) >> 22, int(u.w << 30) >> 30);
        if (format == 64) r = float4(u) / float4(1023.0, 1023.0, 1023.0, 3.0);
        else if (format == 65) r = float4(sdnxSnorm(s.x, 10), sdnxSnorm(s.y, 10), sdnxSnorm(s.z, 10), sdnxSnorm(s.w, 2));
        else if (format == 66) r = float4(u);
        else r = float4(s);
        break;
    }
    case 77: case 78: case 79: case 80:                                      // R16G16 UNORM SNORM USCALED SSCALED
    case 91: case 92: case 93: case 94:                                      // R16G16B16A16 UNORM SNORM USCALED SSCALED
    {
        bool four = format >= 91;
        uint2 w = four ? g_PullData.Load2(a) : uint2(g_PullData.Load(a), 0);
        uint4 u = uint4(w.x & 0xFFFF, w.x >> 16, w.y & 0xFFFF, w.y >> 16);
        int4 s = int4(u << 16) >> 16;
        uint kind = four ? format - 91 : format - 77;
        float4 v = kind == 0 ? float4(u) / 65535.0 :
                   kind == 1 ? float4(sdnxSnorm(s.x, 16), sdnxSnorm(s.y, 16), sdnxSnorm(s.z, 16), sdnxSnorm(s.w, 16)) :
                   kind == 2 ? float4(u) : float4(s);
        r.xy = v.xy;
        if (four)
            r.zw = v.zw;
        break;
    }
    }
    if (expAdjust != 0)
        r *= exp2(float(expAdjust));
    return r;
}
#endif

#ifdef SDNX_DEKO3D
// SDNX deko3d: one DKSH per shader. The alpha test and its function, which Vulkan specializes, are read from the
// shared constants (byte 616, a free word between _InvSize and g_FetchSwizzle: the renderer writes the pipeline's
// specialization value there); the other bits are the values the renderer always uses (CONSTANTS_UBO only).
#define SDNX_DK_SPEC_OFFSET 616
#define g_SpecConstants() (SPEC_CONSTANT_CONSTANTS_UBO | (MASSEFFECT_SHARED_UINT(SDNX_DK_SPEC_OFFSET) & (SPEC_CONSTANT_ALPHA_TEST | SPEC_CONSTANT_ALPHA_FUNC_MASK)))
#else
[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants
#endif

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF
#define g_FetchSwizzle(K) 0x688

#endif

// SDNX: the translator writes every texture fetch as SDNX_TEX(function, slot, descriptor index, sampler index, ...):
// the bindless call on Vulkan, the slot's own texture and sampler on deko3d (the index still carries the signs).
#ifdef SDNX_DEKO3D
Texture2D<float4> g_dkT2D_0 : register(t0, space0); Texture3D<float4> g_dkT3D_0 : register(t0, space1); TextureCube<float4> g_dkTCube_0 : register(t0, space2); SamplerState g_dkS_0 : register(s0, space3);
Texture2D<float4> g_dkT2D_1 : register(t1, space0); Texture3D<float4> g_dkT3D_1 : register(t1, space1); TextureCube<float4> g_dkTCube_1 : register(t1, space2); SamplerState g_dkS_1 : register(s1, space3);
Texture2D<float4> g_dkT2D_2 : register(t2, space0); Texture3D<float4> g_dkT3D_2 : register(t2, space1); TextureCube<float4> g_dkTCube_2 : register(t2, space2); SamplerState g_dkS_2 : register(s2, space3);
Texture2D<float4> g_dkT2D_3 : register(t3, space0); Texture3D<float4> g_dkT3D_3 : register(t3, space1); TextureCube<float4> g_dkTCube_3 : register(t3, space2); SamplerState g_dkS_3 : register(s3, space3);
Texture2D<float4> g_dkT2D_4 : register(t4, space0); Texture3D<float4> g_dkT3D_4 : register(t4, space1); TextureCube<float4> g_dkTCube_4 : register(t4, space2); SamplerState g_dkS_4 : register(s4, space3);
Texture2D<float4> g_dkT2D_5 : register(t5, space0); Texture3D<float4> g_dkT3D_5 : register(t5, space1); TextureCube<float4> g_dkTCube_5 : register(t5, space2); SamplerState g_dkS_5 : register(s5, space3);
Texture2D<float4> g_dkT2D_6 : register(t6, space0); Texture3D<float4> g_dkT3D_6 : register(t6, space1); TextureCube<float4> g_dkTCube_6 : register(t6, space2); SamplerState g_dkS_6 : register(s6, space3);
Texture2D<float4> g_dkT2D_7 : register(t7, space0); Texture3D<float4> g_dkT3D_7 : register(t7, space1); TextureCube<float4> g_dkTCube_7 : register(t7, space2); SamplerState g_dkS_7 : register(s7, space3);
Texture2D<float4> g_dkT2D_8 : register(t8, space0); Texture3D<float4> g_dkT3D_8 : register(t8, space1); TextureCube<float4> g_dkTCube_8 : register(t8, space2); SamplerState g_dkS_8 : register(s8, space3);
Texture2D<float4> g_dkT2D_9 : register(t9, space0); Texture3D<float4> g_dkT3D_9 : register(t9, space1); TextureCube<float4> g_dkTCube_9 : register(t9, space2); SamplerState g_dkS_9 : register(s9, space3);
Texture2D<float4> g_dkT2D_10 : register(t10, space0); Texture3D<float4> g_dkT3D_10 : register(t10, space1); TextureCube<float4> g_dkTCube_10 : register(t10, space2); SamplerState g_dkS_10 : register(s10, space3);
Texture2D<float4> g_dkT2D_11 : register(t11, space0); Texture3D<float4> g_dkT3D_11 : register(t11, space1); TextureCube<float4> g_dkTCube_11 : register(t11, space2); SamplerState g_dkS_11 : register(s11, space3);
Texture2D<float4> g_dkT2D_12 : register(t12, space0); Texture3D<float4> g_dkT3D_12 : register(t12, space1); TextureCube<float4> g_dkTCube_12 : register(t12, space2); SamplerState g_dkS_12 : register(s12, space3);
Texture2D<float4> g_dkT2D_13 : register(t13, space0); Texture3D<float4> g_dkT3D_13 : register(t13, space1); TextureCube<float4> g_dkTCube_13 : register(t13, space2); SamplerState g_dkS_13 : register(s13, space3);
Texture2D<float4> g_dkT2D_14 : register(t14, space0); Texture3D<float4> g_dkT3D_14 : register(t14, space1); TextureCube<float4> g_dkTCube_14 : register(t14, space2); SamplerState g_dkS_14 : register(s14, space3);
Texture2D<float4> g_dkT2D_15 : register(t15, space0); Texture3D<float4> g_dkT3D_15 : register(t15, space1); TextureCube<float4> g_dkTCube_15 : register(t15, space2); SamplerState g_dkS_15 : register(s15, space3);
Texture2D<float4> g_dkT2D_16 : register(t16, space0); Texture3D<float4> g_dkT3D_16 : register(t16, space1); TextureCube<float4> g_dkTCube_16 : register(t16, space2); SamplerState g_dkS_16 : register(s16, space3);
Texture2D<float4> g_dkT2D_17 : register(t17, space0); Texture3D<float4> g_dkT3D_17 : register(t17, space1); TextureCube<float4> g_dkTCube_17 : register(t17, space2); SamplerState g_dkS_17 : register(s17, space3);
Texture2D<float4> g_dkT2D_18 : register(t18, space0); Texture3D<float4> g_dkT3D_18 : register(t18, space1); TextureCube<float4> g_dkTCube_18 : register(t18, space2); SamplerState g_dkS_18 : register(s18, space3);
Texture2D<float4> g_dkT2D_19 : register(t19, space0); Texture3D<float4> g_dkT3D_19 : register(t19, space1); TextureCube<float4> g_dkTCube_19 : register(t19, space2); SamplerState g_dkS_19 : register(s19, space3);
Texture2D<float4> g_dkT2D_20 : register(t20, space0); Texture3D<float4> g_dkT3D_20 : register(t20, space1); TextureCube<float4> g_dkTCube_20 : register(t20, space2); SamplerState g_dkS_20 : register(s20, space3);
Texture2D<float4> g_dkT2D_21 : register(t21, space0); Texture3D<float4> g_dkT3D_21 : register(t21, space1); TextureCube<float4> g_dkTCube_21 : register(t21, space2); SamplerState g_dkS_21 : register(s21, space3);
Texture2D<float4> g_dkT2D_22 : register(t22, space0); Texture3D<float4> g_dkT3D_22 : register(t22, space1); TextureCube<float4> g_dkTCube_22 : register(t22, space2); SamplerState g_dkS_22 : register(s22, space3);
Texture2D<float4> g_dkT2D_23 : register(t23, space0); Texture3D<float4> g_dkT3D_23 : register(t23, space1); TextureCube<float4> g_dkTCube_23 : register(t23, space2); SamplerState g_dkS_23 : register(s23, space3);
Texture2D<float4> g_dkT2D_24 : register(t24, space0); Texture3D<float4> g_dkT3D_24 : register(t24, space1); TextureCube<float4> g_dkTCube_24 : register(t24, space2); SamplerState g_dkS_24 : register(s24, space3);
Texture2D<float4> g_dkT2D_25 : register(t25, space0); Texture3D<float4> g_dkT3D_25 : register(t25, space1); TextureCube<float4> g_dkTCube_25 : register(t25, space2); SamplerState g_dkS_25 : register(s25, space3);
Texture2D<float4> g_dkT2D_26 : register(t26, space0); Texture3D<float4> g_dkT3D_26 : register(t26, space1); TextureCube<float4> g_dkTCube_26 : register(t26, space2); SamplerState g_dkS_26 : register(s26, space3);
Texture2D<float4> g_dkT2D_27 : register(t27, space0); Texture3D<float4> g_dkT3D_27 : register(t27, space1); TextureCube<float4> g_dkTCube_27 : register(t27, space2); SamplerState g_dkS_27 : register(s27, space3);
Texture2D<float4> g_dkT2D_28 : register(t28, space0); Texture3D<float4> g_dkT3D_28 : register(t28, space1); TextureCube<float4> g_dkTCube_28 : register(t28, space2); SamplerState g_dkS_28 : register(s28, space3);
Texture2D<float4> g_dkT2D_29 : register(t29, space0); Texture3D<float4> g_dkT3D_29 : register(t29, space1); TextureCube<float4> g_dkTCube_29 : register(t29, space2); SamplerState g_dkS_29 : register(s29, space3);
Texture2D<float4> g_dkT2D_30 : register(t30, space0); Texture3D<float4> g_dkT3D_30 : register(t30, space1); TextureCube<float4> g_dkTCube_30 : register(t30, space2); SamplerState g_dkS_30 : register(s30, space3);
Texture2D<float4> g_dkT2D_31 : register(t31, space0); Texture3D<float4> g_dkT3D_31 : register(t31, space1); TextureCube<float4> g_dkTCube_31 : register(t31, space2); SamplerState g_dkS_31 : register(s31, space3);
#define SDNX_TEX(FN, SLOT, INDEX, SAMPLER, ...) dk_##FN(SDNX_DK_TEXTURE_##FN(SLOT), g_dkS_##SLOT, INDEX, __VA_ARGS__)
#define SDNX_DK_TEXTURE_tfetch2D(SLOT) g_dkT2D_##SLOT
#define SDNX_DK_TEXTURE_getWeights2D(SLOT) g_dkT2D_##SLOT
#define SDNX_DK_TEXTURE_tfetch3D(SLOT) g_dkT3D_##SLOT
#define SDNX_DK_TEXTURE_tfetchCube(SLOT) g_dkTCube_##SLOT
#else
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
#define SDNX_TEX(FN, SLOT, INDEX, SAMPLER, ...) FN(INDEX, SAMPLER, __VA_ARGS__)
#endif

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// The native renderer packs the four post-swizzle Xenos TextureSign values into bits 24-31 of the
// descriptor index. Values: 0 unsigned, 1 signed (the renderer still warns for it), 2 unsigned biased,
// 3 Xenos piecewise-linear gamma. Descriptor heaps use the low 24 bits.
float masseffectGammaToLinear(float gamma)
{
    gamma = saturate(gamma);
    float scale;
    float offset;
    if (gamma >= 96.0 / 255.0)
    {
        if (gamma >= 192.0 / 255.0) { scale = 8.0 / 1024.0; offset = -1024.0; }
        else                         { scale = 4.0 / 1024.0; offset = -256.0; }
    }
    else
    {
        if (gamma >= 64.0 / 255.0) { scale = 2.0 / 1024.0; offset = -64.0; }
        else                        { scale = 1.0 / 1024.0; offset = 0.0; }
    }
    float linearValue = gamma * ((255.0 * 1024.0) * scale) + offset;
    linearValue += trunc(linearValue * scale);
    return linearValue * (1.0 / 1023.0);
}

float4 masseffectApplyTextureSigns(float4 value, uint packedSigns)
{
    for (uint i = 0; i < 4; ++i)
    {
        uint sign = (packedSigns >> (i * 2)) & 3;
        if (sign == 2)
            value[i] = value[i] * 2.0 - 1.0;
        else if (sign == 3)
            value[i] = masseffectGammaToLinear(value[i]);
    }
    return value;
}

#ifndef SDNX_DEKO3D
float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    uint packedSigns = resourceDescriptorIndex >> 24;
    resourceDescriptorIndex &= 0x00FFFFFF;
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    float2 displacement;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        displacement = offset * invSize;
    else
        displacement = offset / getTexture2DDimensions(texture);
    // SDNX: vertex shaders have no derivatives - sample level 0 there (2 Sleeping Dogs vertex shaders).
#if defined(__SHADER_TARGET_STAGE) && __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
    return masseffectApplyTextureSigns(
        texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + displacement, 0.0), packedSigns);
#else
    return masseffectApplyTextureSigns(
        texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + displacement), packedSigns);
#endif
}
#else
float4 dk_tfetch2D(Texture2D<float4> texture, SamplerState samplerState, uint resourceDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    uint packedSigns = resourceDescriptorIndex >> 24;
    float2 displacement;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TEX_SIZE)
        displacement = offset * invSize;
    else
        displacement = offset / getTexture2DDimensions(texture);
#if defined(__SHADER_TARGET_STAGE) && __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
    return masseffectApplyTextureSigns(texture.SampleLevel(samplerState, texCoord + displacement, 0.0), packedSigns);
#else
    return masseffectApplyTextureSigns(texture.Sample(samplerState, texCoord + displacement), packedSigns);
#endif
}
#endif

#ifndef SDNX_DEKO3D
float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    resourceDescriptorIndex &= 0x00FFFFFF;
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}
#else
float2 dk_getWeights2D(Texture2D<float4> texture, SamplerState samplerState, uint resourceDescriptorIndex, float2 texCoord, float2 offset)
{
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}
#endif

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

#ifndef SDNX_DEKO3D
float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    uint packedSigns = resourceDescriptorIndex >> 24;
    resourceDescriptorIndex &= 0x00FFFFFF;
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return masseffectApplyTextureSigns(r, packedSigns);
}
#endif

#ifndef SDNX_DEKO3D
float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    uint packedSigns = resourceDescriptorIndex >> 24;
    resourceDescriptorIndex &= 0x00FFFFFF;
    return masseffectApplyTextureSigns(
        g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord),
        packedSigns);
}
#else
float4 dk_tfetch3D(Texture3D<float4> texture, SamplerState samplerState, uint resourceDescriptorIndex, float3 texCoord)
{
    return masseffectApplyTextureSigns(texture.Sample(samplerState, texCoord), resourceDescriptorIndex >> 24);
}
#endif

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

#ifndef SDNX_DEKO3D
float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    uint packedSigns = resourceDescriptorIndex >> 24;
    resourceDescriptorIndex &= 0x00FFFFFF;
    return masseffectApplyTextureSigns(
        g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]),
        packedSigns);
}
#else
float4 dk_tfetchCube(TextureCube<float4> texture, SamplerState samplerState, uint resourceDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return masseffectApplyTextureSigns(texture.Sample(samplerState, cubeMapData.cubeMapDirections[texCoord.z]),
        resourceDescriptorIndex >> 24);
}
#endif

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
#ifdef SDNX_DEKO3D
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
#else
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
#endif
}

// SDNX: a vertex fetch with a run-time destination swizzle (Xenos FetchDestinationSwizzle, 3 bits per
// component: 0-3 = data component, 4 = 0, 5 = 1, 6 and 7 = keep the register's component).
float4 fetchApply(float4 previous, float4 value, uint code)
{
    float4 result = previous;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// MASSEFFECT: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// MASSEFFECT: Xenos alpha test with its comparison function. Positive if the pixel passes.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

#ifndef SDNX_DEKO3D
float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    resourceDescriptorIndex &= 0x00FFFFFF;
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}
#endif

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif

#ifdef __spirv__

#ifndef s0_SamplerDescriptorIndex
#define s0_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(0) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 0))
#define s0_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(64) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 64))
#define s0_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(128) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 128))
#define s0_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(192) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 192))
#define s0_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(360), MASSEFFECT_SHARED_FLOAT(364)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 360))
#endif
#ifndef s1_SamplerDescriptorIndex
#define s1_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(4) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 4))
#define s1_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(68) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 68))
#define s1_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(132) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 132))
#define s1_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(196) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 196))
#define s1_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(368), MASSEFFECT_SHARED_FLOAT(372)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 368))
#endif
#ifndef s2_SamplerDescriptorIndex
#define s2_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(8) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 8))
#define s2_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(72) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 72))
#define s2_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(136) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 136))
#define s2_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(200) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 200))
#define s2_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(376), MASSEFFECT_SHARED_FLOAT(380)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 376))
#endif
#ifndef s3_SamplerDescriptorIndex
#define s3_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(12) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 12))
#define s3_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(76) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 76))
#define s3_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(140) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 140))
#define s3_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(204) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 204))
#define s3_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(384), MASSEFFECT_SHARED_FLOAT(388)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 384))
#endif
#ifndef s4_SamplerDescriptorIndex
#define s4_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(16) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 16))
#define s4_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(80) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 80))
#define s4_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(144) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 144))
#define s4_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(208) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 208))
#define s4_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(392), MASSEFFECT_SHARED_FLOAT(396)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 392))
#endif
#ifndef s5_SamplerDescriptorIndex
#define s5_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(20) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 20))
#define s5_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(84) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 84))
#define s5_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(148) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 148))
#define s5_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(212) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 212))
#define s5_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(400), MASSEFFECT_SHARED_FLOAT(404)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 400))
#endif
#ifndef s6_SamplerDescriptorIndex
#define s6_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(24) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 24))
#define s6_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(88) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 88))
#define s6_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(152) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 152))
#define s6_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(216) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 216))
#define s6_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(408), MASSEFFECT_SHARED_FLOAT(412)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 408))
#endif
#ifndef s7_SamplerDescriptorIndex
#define s7_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(28) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 28))
#define s7_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(92) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 92))
#define s7_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(156) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 156))
#define s7_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(220) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 220))
#define s7_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(416), MASSEFFECT_SHARED_FLOAT(420)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 416))
#endif
#ifndef s8_SamplerDescriptorIndex
#define s8_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(32) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 32))
#define s8_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(96) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 96))
#define s8_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(160) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 160))
#define s8_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(224) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 224))
#define s8_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(424), MASSEFFECT_SHARED_FLOAT(428)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 424))
#endif
#ifndef s9_SamplerDescriptorIndex
#define s9_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(36) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 36))
#define s9_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(100) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 100))
#define s9_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(164) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 164))
#define s9_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(228) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 228))
#define s9_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(432), MASSEFFECT_SHARED_FLOAT(436)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 432))
#endif
#ifndef s10_SamplerDescriptorIndex
#define s10_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(40) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 40))
#define s10_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(104) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 104))
#define s10_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(168) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 168))
#define s10_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(232) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 232))
#define s10_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(440), MASSEFFECT_SHARED_FLOAT(444)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 440))
#endif
#ifndef s11_SamplerDescriptorIndex
#define s11_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(44) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 44))
#define s11_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(108) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 108))
#define s11_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(172) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 172))
#define s11_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(236) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 236))
#define s11_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(448), MASSEFFECT_SHARED_FLOAT(452)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 448))
#endif
#ifndef s12_SamplerDescriptorIndex
#define s12_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(48) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 48))
#define s12_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(112) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 112))
#define s12_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(176) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 176))
#define s12_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(240) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 240))
#define s12_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(456), MASSEFFECT_SHARED_FLOAT(460)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 456))
#endif
#ifndef s13_SamplerDescriptorIndex
#define s13_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(52) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 52))
#define s13_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(116) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 116))
#define s13_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(180) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 180))
#define s13_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(244) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 244))
#define s13_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(464), MASSEFFECT_SHARED_FLOAT(468)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 464))
#endif
#ifndef s14_SamplerDescriptorIndex
#define s14_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(56) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 56))
#define s14_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(120) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 120))
#define s14_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(184) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 184))
#define s14_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(248) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 248))
#define s14_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(472), MASSEFFECT_SHARED_FLOAT(476)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 472))
#endif
#ifndef s15_SamplerDescriptorIndex
#define s15_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(60) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 60))
#define s15_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(124) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 124))
#define s15_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(188) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 188))
#define s15_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(252) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 252))
#define s15_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(480), MASSEFFECT_SHARED_FLOAT(484)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 480))
#endif
#ifndef s16_SamplerDescriptorIndex
#define s16_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(64) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 64))
#define s16_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(128) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 128))
#define s16_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(192) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 192))
#define s16_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(256) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 256))
#define s16_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(488), MASSEFFECT_SHARED_FLOAT(492)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 488))
#endif
#ifndef s17_SamplerDescriptorIndex
#define s17_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(68) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 68))
#define s17_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(132) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 132))
#define s17_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(196) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 196))
#define s17_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(260) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 260))
#define s17_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(496), MASSEFFECT_SHARED_FLOAT(500)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 496))
#endif
#ifndef s18_SamplerDescriptorIndex
#define s18_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(72) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 72))
#define s18_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(136) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 136))
#define s18_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(200) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 200))
#define s18_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(264) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 264))
#define s18_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(504), MASSEFFECT_SHARED_FLOAT(508)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 504))
#endif
#ifndef s19_SamplerDescriptorIndex
#define s19_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(76) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 76))
#define s19_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(140) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 140))
#define s19_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(204) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 204))
#define s19_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(268) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 268))
#define s19_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(512), MASSEFFECT_SHARED_FLOAT(516)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 512))
#endif
#ifndef s20_SamplerDescriptorIndex
#define s20_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(80) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 80))
#define s20_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(144) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 144))
#define s20_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(208) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 208))
#define s20_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(272) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 272))
#define s20_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(520), MASSEFFECT_SHARED_FLOAT(524)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 520))
#endif
#ifndef s21_SamplerDescriptorIndex
#define s21_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(84) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 84))
#define s21_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(148) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 148))
#define s21_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(212) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 212))
#define s21_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(276) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 276))
#define s21_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(528), MASSEFFECT_SHARED_FLOAT(532)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 528))
#endif
#ifndef s22_SamplerDescriptorIndex
#define s22_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(88) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 88))
#define s22_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(152) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 152))
#define s22_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(216) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 216))
#define s22_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(280) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 280))
#define s22_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(536), MASSEFFECT_SHARED_FLOAT(540)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 536))
#endif
#ifndef s23_SamplerDescriptorIndex
#define s23_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(92) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 92))
#define s23_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(156) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 156))
#define s23_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(220) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 220))
#define s23_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(284) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 284))
#define s23_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(544), MASSEFFECT_SHARED_FLOAT(548)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 544))
#endif
#ifndef s24_SamplerDescriptorIndex
#define s24_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(96) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 96))
#define s24_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(160) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 160))
#define s24_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(224) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 224))
#define s24_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(288) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 288))
#define s24_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(552), MASSEFFECT_SHARED_FLOAT(556)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 552))
#endif
#ifndef s25_SamplerDescriptorIndex
#define s25_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(100) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 100))
#define s25_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(164) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 164))
#define s25_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(228) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 228))
#define s25_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(292) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 292))
#define s25_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(560), MASSEFFECT_SHARED_FLOAT(564)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 560))
#endif
#ifndef s26_SamplerDescriptorIndex
#define s26_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(104) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 104))
#define s26_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(168) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 168))
#define s26_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(232) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 232))
#define s26_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(296) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 296))
#define s26_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(568), MASSEFFECT_SHARED_FLOAT(572)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 568))
#endif
#ifndef s27_SamplerDescriptorIndex
#define s27_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(108) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 108))
#define s27_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(172) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 172))
#define s27_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(236) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 236))
#define s27_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(300) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 300))
#define s27_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(576), MASSEFFECT_SHARED_FLOAT(580)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 576))
#endif
#ifndef s28_SamplerDescriptorIndex
#define s28_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(112) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 112))
#define s28_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(176) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 176))
#define s28_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(240) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 240))
#define s28_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(304) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 304))
#define s28_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(584), MASSEFFECT_SHARED_FLOAT(588)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 584))
#endif
#ifndef s29_SamplerDescriptorIndex
#define s29_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(116) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 116))
#define s29_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(180) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 180))
#define s29_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(244) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 244))
#define s29_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(308) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 308))
#define s29_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(592), MASSEFFECT_SHARED_FLOAT(596)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 592))
#endif
#ifndef s30_SamplerDescriptorIndex
#define s30_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(120) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 120))
#define s30_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(184) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 184))
#define s30_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(248) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 248))
#define s30_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(312) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 312))
#define s30_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(600), MASSEFFECT_SHARED_FLOAT(604)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 600))
#endif
#ifndef s31_SamplerDescriptorIndex
#define s31_Texture2DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(124) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 124))
#define s31_Texture3DDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(188) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 188))
#define s31_TextureCubeDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(252) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 252))
#define s31_SamplerDescriptorIndex (MASSEFFECT_UBO ? MASSEFFECT_SHARED_UINT(316) : SDNX_RAW_LOAD<uint>(g_PushConstants.SharedConstants + 316))
#define s31_InvSize (MASSEFFECT_UBO ? float2(MASSEFFECT_SHARED_FLOAT(608), MASSEFFECT_SHARED_FLOAT(612)) : SDNX_RAW_LOAD<float2>(g_PushConstants.SharedConstants + 608))
#endif

#else

cbuffer VertexShaderConstants : register(b0, space4)
{
};

cbuffer SharedConstants : register(b2, space4)
{
	DEFINE_SHARED_CONSTANTS();
};

#endif

#ifndef __spirv__
[shader("vertex")]
#endif
void main(
	out float4 oPos : SV_Position,
	out float4 oTexCoord0 : TEXCOORD0,
	out float4 oTexCoord1 : TEXCOORD1,
	out float4 oTexCoord2 : TEXCOORD2,
	out float4 oTexCoord3 : TEXCOORD3,
	out float4 oTexCoord4 : TEXCOORD4,
	out float4 oTexCoord5 : TEXCOORD5,
	out float4 oTexCoord6 : TEXCOORD6,
	out float4 oTexCoord7 : TEXCOORD7,
	out float4 oTexCoord8 : TEXCOORD8,
	out float4 oTexCoord9 : TEXCOORD9,
	out float4 oTexCoord10 : TEXCOORD10,
	out float4 oTexCoord11 : TEXCOORD11,
	out float4 oTexCoord12 : TEXCOORD12,
	out float4 oTexCoord13 : TEXCOORD13,
	out float4 oTexCoord14 : TEXCOORD14,
	out float4 oTexCoord15 : TEXCOORD15,
	out float4 oColor0 : COLOR0,
	out float4 oColor1 : COLOR1,
	out float4 oColor2 : COLOR2,
	out float4 oColor3 : COLOR3,
	out float4 oColor4 : COLOR4,
	out float4 oColor5 : COLOR5)
{
	oTexCoord0 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord1 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord2 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord3 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord4 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord5 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord6 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord7 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord8 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord9 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord10 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord11 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord12 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord13 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord14 = float4(0.0, 0.0, 0.0, 1.0);
	oTexCoord15 = float4(0.0, 0.0, 0.0, 1.0);
	oColor0 = float4(0.0, 0.0, 0.0, 1.0);
	oColor1 = float4(0.0, 0.0, 0.0, 1.0);
	oColor2 = float4(0.0, 0.0, 0.0, 1.0);
	oColor3 = float4(0.0, 0.0, 0.0, 1.0);
	oColor4 = float4(0.0, 0.0, 0.0, 1.0);
	oColor5 = float4(0.0, 0.0, 0.0, 1.0);

	float4 r0 = 0.0;
	float4 r1 = 0.0;
	float4 r2 = 0.0;
	float4 r3 = 0.0;
	float4 r4 = 0.0;
	float4 r5 = 0.0;
	float4 r6 = 0.0;
	float4 r7 = 0.0;
	float4 r8 = 0.0;
	float4 r9 = 0.0;
	float4 r10 = 0.0;
	float4 r11 = 0.0;
	float4 r12 = 0.0;
	float4 r13 = 0.0;
	float4 r14 = 0.0;
	float4 r15 = 0.0;
	float4 r16 = 0.0;
	float4 r17 = 0.0;
	float4 r18 = 0.0;
	float4 r19 = 0.0;
	float4 r20 = 0.0;
	float4 r21 = 0.0;
	float4 r22 = 0.0;
	float4 r23 = 0.0;
	float4 r24 = 0.0;
	float4 r25 = 0.0;
	float4 r26 = 0.0;
	float4 r27 = 0.0;
	float4 r28 = 0.0;
	float4 r29 = 0.0;
	float4 r30 = 0.0;
	float4 r31 = 0.0;
	int a0 = 0;
	int aL = 0;
	bool p0 = false;
	float ps = 0.0;

	oTexCoord0.x = 0.0;
	oTexCoord0.y = 0.0;
	oTexCoord0.z = 0.0;
	oTexCoord0.w = 0.0;
	oPos.x = 0.0;
	oPos.y = 0.0;
	oPos.z = 0.0;
	oPos.w = 1.0;
	oPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w;
	oPos.xy += g_HalfPixelOffset * oPos.w;
	return;
}
// SDNX_FETCH_ORDER:
