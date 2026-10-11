/**
 * @file        ui/d3d11/d3d11_presenter.h
 * @brief       [svr2011] Direct3D 11 presenter: the guest output (bilinear,
 *              letterboxed) and the UI drawers on a DXGI swap chain - the flip
 *              model where DXGI has it (Windows 8+), the blt model otherwise.
 *
 * Everything runs on the provider's immediate context under its lock; the
 * guest output textures are reference-counted, so no submission tracking.
 * The FidelityFX effects of the D3D12 / Vulkan presenters aren't here: any
 * effect chain is drawn as one bilinear pass.
 */

#pragma once

#include <array>
#include <memory>

#include <rex/ui/d3d11/d3d11_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>

namespace rex::ui::d3d11 {

class D3D11UIDrawContext final : public UIDrawContext {
 public:
  D3D11UIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height, ID3D11DeviceContext* context)
      : UIDrawContext(presenter, render_target_width, render_target_height), context_(context) {}
  ID3D11DeviceContext* context() const { return context_; }

 private:
  ID3D11DeviceContext* context_;
};

class D3D11Presenter final : public Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  class D3D11GuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    D3D11GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D11Texture2D* texture,
                                   ID3D11RenderTargetView* rtv)
        : GuestOutputRefreshContext(is_8bpc_out_ref), texture_(texture), rtv_(rtv) {}
    // kGuestOutputFormat, the guest output's size; drawn to (hold the
    // provider's context lock).
    ID3D11Texture2D* texture() const { return texture_; }
    ID3D11RenderTargetView* rtv() const { return rtv_; }

   private:
    ID3D11Texture2D* texture_;
    ID3D11RenderTargetView* rtv_;
  };

  static std::unique_ptr<D3D11Presenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                const D3D11Provider& provider);
  ~D3D11Presenter();

  const D3D11Provider& provider() const { return provider_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutput {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  };

  explicit D3D11Presenter(HostGpuLossCallback host_gpu_loss_callback,
                          const D3D11Provider& provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}
  bool InitializeSurfaceIndependent();
  void DestroySwapChain();
  bool GetBackBufferRTV();

  const D3D11Provider& provider_;
  bool dxgi_supports_tearing_ = false;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> rect_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> bilinear_ps_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_clamp_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_opaque_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_off_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> rect_constants_;

  std::array<GuestOutput, kGuestOutputMailboxSize> guest_outputs_;

  // Painting (UI thread).
  Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
  uint32_t swap_chain_width_ = 0;
  uint32_t swap_chain_height_ = 0;
  UINT swap_chain_flags_ = 0;
  bool swap_chain_flip_ = false;
  // Flip model on Windows 8.1+: signaled when a back buffer is free, waited
  // for before the context lock - Present then doesn't block with the lock
  // held (the native renderer submits on the same context).
  HANDLE frame_latency_waitable_ = nullptr;
};

}  // namespace rex::ui::d3d11
