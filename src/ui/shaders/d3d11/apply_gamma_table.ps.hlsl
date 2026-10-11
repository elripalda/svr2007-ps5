// [svr2011] Direct3D 11 guest output refresh: the guest's 256-entry gamma
// ramp applied to the app renderer's frame, pixel for pixel (the same as the
// D3D12 / Vulkan backends' compute pass). The ramp is a 256x1
// R10G10B10A2_UNORM texture of the DC_LUT_30_COLOR registers: blue in the
// low bits (.r), red in the high (.b).
Texture2D<float4> source : register(t0);
Texture2D<float4> ramp : register(t1);

float4 main(float2 texcoord : TEXCOORD, float4 position : SV_Position) : SV_Target {
  float3 color = source.Load(int3(int2(position.xy), 0)).rgb;
  uint3 index = uint3(saturate(color) * 255.0 + 0.5);
  return float4(ramp.Load(int3(index.r, 0, 0)).b, ramp.Load(int3(index.g, 0, 0)).g,
                ramp.Load(int3(index.b, 0, 0)).r, 1.0);
}
