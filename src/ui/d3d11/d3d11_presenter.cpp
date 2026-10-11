// [svr2011] Direct3D 11 presenter (rex/ui/d3d11/d3d11_presenter.h).

#include <rex/ui/d3d11/d3d11_presenter.h>

#include <algorithm>
#include <cstring>

#include <dxgi1_5.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_win.h>

REXCVAR_DEFINE_BOOL(d3d11_allow_tearing, true, "UI/D3D11",
                    "Allow variable refresh rate and tearing (flip model swap chains)");

namespace rex::ui::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_4_0/guest_output_bilinear_ps.h"
#include "../shaders/bytecode/d3d11_4_0/guest_output_rect_vs.h"
}  // namespace shaders

namespace {

// RectConstants of guest_output_rect.vs.hlsl.
struct RectConstants {
  float offset[2];
  float size[2];
};

}  // namespace

std::unique_ptr<D3D11Presenter> D3D11Presenter::Create(HostGpuLossCallback host_gpu_loss_callback,
                                                       const D3D11Provider& provider) {
  std::unique_ptr<D3D11Presenter> presenter(new D3D11Presenter(host_gpu_loss_callback, provider));
  if (!presenter->InitializeSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

D3D11Presenter::~D3D11Presenter() { DestroySwapChain(); }

Surface::TypeFlags D3D11Presenter::GetSupportedSurfaceTypes() const {
  return Surface::kTypeFlag_Win32Hwnd;
}

bool D3D11Presenter::InitializeSurfaceIndependent() {
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    dxgi_supports_tearing_ =
        SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory5))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                sizeof(tearing))) &&
        tearing;
  }

  ID3D11Device* device = provider_.GetDevice();
  if (FAILED(device->CreateVertexShader(shaders::guest_output_rect_vs,
                                        sizeof(shaders::guest_output_rect_vs), nullptr,
                                        &rect_vs_)) ||
      FAILED(device->CreatePixelShader(shaders::guest_output_bilinear_ps,
                                       sizeof(shaders::guest_output_bilinear_ps), nullptr,
                                       &bilinear_ps_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output shaders");
    return false;
  }
  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  D3D11_BLEND_DESC blend = {};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  D3D11_DEPTH_STENCIL_DESC depth = {};
  D3D11_BUFFER_DESC constants = {};
  constants.ByteWidth = 16;
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateSamplerState(&sampler, &linear_clamp_)) ||
      FAILED(device->CreateBlendState(&blend, &blend_opaque_)) ||
      FAILED(device->CreateRasterizerState(&raster, &rasterizer_)) ||
      FAILED(device->CreateDepthStencilState(&depth, &depth_off_)) ||
      FAILED(device->CreateBuffer(&constants, nullptr, &rect_constants_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the painting state");
    return false;
  }
  return InitializeCommonSurfaceIndependent();
}

void D3D11Presenter::DestroySwapChain() {
  if (!swap_chain_) {
    return;
  }
  {
    std::lock_guard lock(provider_.context_mutex());
    provider_.GetContext()->ClearState();
    back_buffer_rtv_.Reset();
    provider_.GetContext()->Flush();
  }
  swap_chain_.Reset();
  if (frame_latency_waitable_) {
    CloseHandle(frame_latency_waitable_);
    frame_latency_waitable_ = nullptr;
  }
  swap_chain_width_ = 0;
  swap_chain_height_ = 0;
  swap_chain_flags_ = 0;
  swap_chain_flip_ = false;
}

bool D3D11Presenter::GetBackBufferRTV() {
  // Buffer 0 is always the current back buffer in Direct3D 11, flip model too.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
  if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
      FAILED(provider_.GetDevice()->CreateRenderTargetView(buffer.Get(), nullptr,
                                                           &back_buffer_rtv_))) {
    REXLOG_ERROR("D3D11Presenter: Failed to get the swap chain's back buffer");
    return false;
  }
  return true;
}

Presenter::SurfacePaintConnectResult D3D11Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  is_vsync_implicit_out = false;
  const uint32_t width =
      std::min(new_surface_width, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  const uint32_t height =
      std::min(new_surface_height, uint32_t(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  if (swap_chain_) {
    if (was_paintable && swap_chain_width_ == width && swap_chain_height_ == height) {
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    bool resized;
    {
      std::lock_guard lock(provider_.context_mutex());
      provider_.GetContext()->ClearState();
      back_buffer_rtv_.Reset();
      resized = SUCCEEDED(swap_chain_->ResizeBuffers(0, UINT(width), UINT(height),
                                                     DXGI_FORMAT_UNKNOWN, swap_chain_flags_)) &&
                GetBackBufferRTV();
    }
    if (resized) {
      swap_chain_width_ = width;
      swap_chain_height_ = height;
      return SurfacePaintConnectResult::kSuccess;
    }
    REXLOG_ERROR("D3D11Presenter: Failed to resize the swap chain - creating a new one");
    DestroySwapChain();
  }

  if (new_surface.GetType() != Surface::kTypeIndex_Win32Hwnd) {
    REXLOG_ERROR("D3D11Presenter: unsupported surface type");
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  HWND hwnd = static_cast<const Win32HwndSurface&>(new_surface).hwnd();
  ID3D11Device* device = provider_.GetDevice();

  // The flip model (Windows 10: FLIP_DISCARD, Windows 8: FLIP_SEQUENTIAL),
  // else the blt model (Windows 7).
  if (IDXGIFactory2* factory2 = provider_.GetDXGIFactory2()) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.Format = kSwapChainFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    for (DXGI_SWAP_EFFECT effect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL}) {
      desc.SwapEffect = effect;
      desc.Flags = effect == DXGI_SWAP_EFFECT_FLIP_DISCARD && dxgi_supports_tearing_ &&
                           REXCVAR_GET(d3d11_allow_tearing)
                       ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                       : 0;
      Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
      // (Windows 8.1+: a waitable swap chain, see frame_latency_waitable_)
      desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
      HRESULT hr = factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr,
                                                    &swap_chain_1);
      if (FAILED(hr)) {
        desc.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        hr = factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &swap_chain_1);
      }
      if (FAILED(hr) && desc.Scaling == DXGI_SCALING_NONE) {
        desc.Scaling = DXGI_SCALING_STRETCH;  // (Windows 8 has no SCALING_NONE)
        hr = factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &swap_chain_1);
      }
      if (SUCCEEDED(hr)) {
        swap_chain_ = swap_chain_1;
        swap_chain_flags_ = desc.Flags;
        swap_chain_flip_ = true;
        Microsoft::WRL::ComPtr<IDXGISwapChain2> swap_chain_2;
        if ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
            SUCCEEDED(swap_chain_.As(&swap_chain_2))) {
          swap_chain_2->SetMaximumFrameLatency(2);
          frame_latency_waitable_ = swap_chain_2->GetFrameLatencyWaitableObject();
        }
        break;
      }
    }
  }
  if (!swap_chain_) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = UINT(width);
    desc.BufferDesc.Height = UINT(height);
    desc.BufferDesc.Format = kSwapChainFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(provider_.GetDXGIFactory()->CreateSwapChain(device, &desc, &swap_chain_))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create a swap chain for the window");
      return SurfacePaintConnectResult::kFailure;
    }
    swap_chain_flags_ = 0;
    swap_chain_flip_ = false;
  }
  // The window implementation does borderless fullscreen itself.
  provider_.GetDXGIFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  swap_chain_width_ = width;
  swap_chain_height_ = height;
  bool got_rtv;
  {
    std::lock_guard lock(provider_.context_mutex());
    got_rtv = GetBackBufferRTV();
  }
  if (!got_rtv) {
    DestroySwapChain();
    return SurfacePaintConnectResult::kFailure;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("D3D11Presenter: {} swap chain{}",
                swap_chain_flip_ ? "flip model" : "blt model (no DXGI 1.2 flip model)",
                (swap_chain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? ", tearing allowed" : "");
  }
  return SurfacePaintConnectResult::kSuccess;
}

void D3D11Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() { DestroySwapChain(); }

bool D3D11Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  GuestOutput& output = guest_outputs_[mailbox_index];
  if (output.texture) {
    D3D11_TEXTURE2D_DESC desc;
    output.texture->GetDesc(&desc);
    if (desc.Width != frontbuffer_width || desc.Height != frontbuffer_height) {
      // (Painting holds its own references to the one it draws.)
      output = GuestOutput();
    }
  }
  if (!output.texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = frontbuffer_width;
    desc.Height = frontbuffer_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device* device = provider_.GetDevice();
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &output.texture)) ||
        FAILED(device->CreateRenderTargetView(output.texture.Get(), nullptr, &output.rtv)) ||
        FAILED(device->CreateShaderResourceView(output.texture.Get(), nullptr, &output.srv))) {
      REXLOG_ERROR("D3D11Presenter: Failed to create the guest output {}x{} texture",
                   frontbuffer_width, frontbuffer_height);
      output = GuestOutput();
      return false;
    }
  }
  D3D11GuestOutputRefreshContext context(is_8bpc_out_ref, output.texture.Get(), output.rtv.Get());
  return refresher(context);
}

Presenter::PaintResult D3D11Presenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  if (!swap_chain_ || !back_buffer_rtv_) {
    return PaintResult::kNotPresented;
  }

  GuestOutputProperties properties;
  GuestOutputPaintConfig paint_config;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> guest_output_srv;
  {
    uint32_t mailbox_index;
    std::unique_lock<std::mutex> consumer_lock(
        ConsumeGuestOutput(mailbox_index, &properties, &paint_config));
    if (mailbox_index != UINT32_MAX) {
      guest_output_srv = guest_outputs_[mailbox_index].srv;
    }
  }

  // A free back buffer first, without the context lock.
  if (frame_latency_waitable_) {
    WaitForSingleObjectEx(frame_latency_waitable_, 100, TRUE);
  }
  std::lock_guard lock(provider_.context_mutex());
  ID3D11DeviceContext* context = provider_.GetContext();
  context->ClearState();
  ID3D11RenderTargetView* rtv = back_buffer_rtv_.Get();
  context->OMSetRenderTargets(1, &rtv, nullptr);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  context->ClearRenderTargetView(rtv, black);

  if (guest_output_srv) {
    GuestOutputPaintFlow flow =
        GetGuestOutputPaintFlow(properties, swap_chain_width_, swap_chain_height_,
                                D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                                D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, paint_config);
    if (flow.effect_count) {
      // One bilinear pass to the final rectangle (any effect chain).
      const std::pair<uint32_t, uint32_t>& size = flow.effect_output_sizes[flow.effect_count - 1];
      D3D11_MAPPED_SUBRESOURCE mapped;
      if (SUCCEEDED(context->Map(rect_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        RectConstants rect;
        rect.offset[0] = float(flow.output_x) * 2.0f / float(swap_chain_width_) - 1.0f;
        rect.offset[1] = 1.0f - float(flow.output_y) * 2.0f / float(swap_chain_height_);
        rect.size[0] = float(size.first) * 2.0f / float(swap_chain_width_);
        rect.size[1] = -float(size.second) * 2.0f / float(swap_chain_height_);
        std::memcpy(mapped.pData, &rect, sizeof(rect));
        context->Unmap(rect_constants_.Get(), 0);
      }
      D3D11_VIEWPORT viewport = {};
      viewport.Width = float(swap_chain_width_);
      viewport.Height = float(swap_chain_height_);
      viewport.MaxDepth = 1.0f;
      context->RSSetViewports(1, &viewport);
      context->RSSetState(rasterizer_.Get());
      context->OMSetBlendState(blend_opaque_.Get(), nullptr, 0xFFFFFFFF);
      context->OMSetDepthStencilState(depth_off_.Get(), 0);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      ID3D11Buffer* constants = rect_constants_.Get();
      context->VSSetConstantBuffers(0, 1, &constants);
      context->VSSetShader(rect_vs_.Get(), nullptr, 0);
      context->PSSetShader(bilinear_ps_.Get(), nullptr, 0);
      ID3D11ShaderResourceView* srv = guest_output_srv.Get();
      context->PSSetShaderResources(0, 1, &srv);
      ID3D11SamplerState* sampler = linear_clamp_.Get();
      context->PSSetSamplers(0, 1, &sampler);
      context->Draw(4, 0);
      ID3D11ShaderResourceView* null_srv = nullptr;
      context->PSSetShaderResources(0, 1, &null_srv);
    }
  }

  if (execute_ui_drawers) {
    D3D11UIDrawContext ui_draw_context(*this, swap_chain_width_, swap_chain_height_, context);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  // Present as soon as possible (no vsync wait: the guest's own rate decides),
  // tearing allowed for variable refresh rate where the swap chain has it.
  HRESULT hr = swap_chain_->Present(
      0, (swap_chain_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
  // (Flip model unbinds the back buffer after Present.)
  context->ClearState();
  switch (hr) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGpuLostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGpuLostResponsible;
    default:
      return SUCCEEDED(hr) ? PaintResult::kPresented : PaintResult::kNotPresented;
  }
}

bool D3D11Presenter::CaptureGuestOutput(RawImage& image_out) {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  {
    uint32_t mailbox_index;
    std::unique_lock<std::mutex> consumer_lock(ConsumeGuestOutput(mailbox_index, nullptr, nullptr));
    if (mailbox_index != UINT32_MAX) {
      texture = guest_outputs_[mailbox_index].texture;
    }
  }
  if (!texture) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(provider_.GetDevice()->CreateTexture2D(&desc, nullptr, &staging))) {
    REXLOG_ERROR("D3D11Presenter: Failed to create the guest output capture texture");
    return false;
  }
  std::lock_guard lock(provider_.context_mutex());
  ID3D11DeviceContext* context = provider_.GetContext();
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    REXLOG_ERROR("D3D11Presenter: Failed to map the guest output capture texture");
    return false;
  }
  image_out.width = desc.Width;
  image_out.height = desc.Height;
  image_out.stride = sizeof(uint32_t) * image_out.width;
  image_out.data.resize(size_t(image_out.stride) * image_out.height);
  auto* out = reinterpret_cast<uint32_t*>(image_out.data.data());
  for (uint32_t y = 0; y < desc.Height; ++y) {
    const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
                                                        size_t(mapped.RowPitch) * y);
    for (uint32_t x = 0; x < desc.Width; ++x) {
      out[size_t(desc.Width) * y + x] = Packed10bpcRGBTo8bpcBytes(row[x]);
    }
  }
  context->Unmap(staging.Get(), 0);
  return true;
}

}  // namespace rex::ui::d3d11
