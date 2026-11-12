// SDNX native renderer: copy the resolved front buffer into the presenter's guest output image (storage, rgb10a2).
// dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 present.cs.hlsl -Fo present.cs.spv
[[vk::binding(0, 0)]] Texture2D<float4> g_source;
[[vk::binding(1, 0)]] SamplerState g_sampler;
[[vk::binding(2, 0)]] [[vk::image_format("rgb10a2")]] RWTexture2D<float4> g_output;
struct PushConstants {
  float2 source_uv_scale;  // used part of the source image (front buffer size / image size)
  uint2 output_size;
};
[[vk::push_constant]] PushConstants g_pc;
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_pc.output_size)) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(g_pc.output_size) * g_pc.source_uv_scale;
  g_output[id.xy] = float4(g_source.SampleLevel(g_sampler, uv, 0.0).rgb, 1.0);
}
