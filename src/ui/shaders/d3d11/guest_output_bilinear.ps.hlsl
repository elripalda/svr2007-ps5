// [svr2011] Direct3D 11 presenter: the guest output, bilinear-filtered.
Texture2D<float3> guest_output : register(t0);
SamplerState linear_clamp : register(s0);

float4 main(float2 texcoord : TEXCOORD) : SV_Target {
  return float4(guest_output.SampleLevel(linear_clamp, texcoord, 0.0), 1.0);
}
