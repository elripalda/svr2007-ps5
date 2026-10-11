// [svr2011] Direct3D 11 presenter: a rectangle (triangle strip of 4 vertices,
// no vertex buffer) at an offset and size in normalized device coordinates.
cbuffer RectConstants : register(b0) {
  float2 rect_offset;  // NDC of the top-left corner
  float2 rect_size;    // NDC size (y negative: down)
};

void main(uint vertex_id : SV_VertexID, out float2 texcoord : TEXCOORD,
          out float4 position : SV_Position) {
  float2 uv = float2(float(vertex_id & 1u), float(vertex_id >> 1u));
  texcoord = uv;
  position = float4(rect_offset + uv * rect_size, 0.0, 1.0);
}
