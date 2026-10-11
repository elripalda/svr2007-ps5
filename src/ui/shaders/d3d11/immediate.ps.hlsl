// [svr2011] Direct3D 11 ImGui / immediate drawer pixel shader (a draw
// without a texture binds a 1x1 white one).
Texture2D<float4> immediate_texture : register(t0);
SamplerState immediate_sampler : register(s0);

float4 main(float2 texcoord : TEXCOORD, float4 color : COLOR) : SV_Target {
  return immediate_texture.Sample(immediate_sampler, texcoord) * color;
}
