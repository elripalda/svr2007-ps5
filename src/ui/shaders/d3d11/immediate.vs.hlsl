// [svr2011] Direct3D 11 ImGui / immediate drawer vertex shader.
cbuffer ImmediateConstants : register(b0) {
  float2 coordinate_space_size_inv;
};

struct VsInput {
  float2 position : POSITION;
  float2 texcoord : TEXCOORD;
  float4 color : COLOR;
};

struct VsOutput {
  float2 texcoord : TEXCOORD;
  float4 color : COLOR;
  float4 position : SV_Position;
};

VsOutput main(VsInput input) {
  VsOutput output;
  output.position = float4(
      input.position * coordinate_space_size_inv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}
