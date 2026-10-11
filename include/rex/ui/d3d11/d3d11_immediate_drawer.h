/**
 * @file        ui/d3d11/d3d11_immediate_drawer.h
 * @brief       [svr2011] Direct3D 11 immediate drawer (ImGui and other
 *              overlays), drawing on the provider's immediate context while
 *              the presenter paints (the presenter holds the context lock).
 */

#pragma once

#include <memory>

#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui::d3d11 {

class D3D11ImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<D3D11ImmediateDrawer> Create(const D3D11Provider& provider);

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  enum SamplerIndex { kNearestClamp, kLinearClamp, kNearestRepeat, kLinearRepeat, kSamplerCount };

  class D3D11ImmediateTexture final : public ImmediateTexture {
   public:
    D3D11ImmediateTexture(uint32_t width, uint32_t height,
                          Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view,
                          SamplerIndex sampler)
        : ImmediateTexture(width, height), view_(std::move(view)), sampler_(sampler) {}
    ID3D11ShaderResourceView* view() const { return view_.Get(); }
    SamplerIndex sampler() const { return sampler_; }

   private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    SamplerIndex sampler_;
  };

  explicit D3D11ImmediateDrawer(const D3D11Provider& provider) : provider_(provider) {}
  bool Initialize();
  // Grows `buffer` (dynamic, `bind` flags) to at least `size` bytes.
  bool EnsureBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, UINT& capacity, UINT size,
                    UINT bind);

  const D3D11Provider& provider_;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> samplers_[kSamplerCount];
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  // A 1x1 white texture for draws without one (solid color).
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> white_view_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
  UINT vertex_buffer_capacity_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer_;
  UINT index_buffer_capacity_ = 0;

  ID3D11DeviceContext* context_ = nullptr;  // between Begin and End
  bool batch_open_ = false;
  bool batch_has_index_buffer_ = false;
  D3D11_PRIMITIVE_TOPOLOGY current_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11ShaderResourceView* current_view_ = nullptr;
  int current_sampler_ = -1;
  D3D11_RECT current_scissor_ = {};
};

}  // namespace rex::ui::d3d11
