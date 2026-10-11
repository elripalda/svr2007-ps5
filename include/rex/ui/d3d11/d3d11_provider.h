/**
 * @file        ui/d3d11/d3d11_provider.h
 * @brief       [svr2011] Direct3D 11 graphics provider: the device the
 *              presenter, the ImGui drawer and an app-side renderer share.
 *
 * For GPUs / drivers that run neither Direct3D 12 nor Vulkan. One immediate
 * context serves every thread: whoever uses it holds context_mutex() for the
 * whole sequence of calls (the presenter while painting, the command
 * processor while refreshing the guest output, an app renderer while it
 * replays a frame), and sets all the state it needs (ClearState first) -
 * nothing may assume state left by another user.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::d3d11 {

class D3D11Provider : public GraphicsProvider {
 public:
  ~D3D11Provider() override;

  static bool IsD3D11APIAvailable();
  // Null (with the reason logged) if no device can be created.
  static std::unique_ptr<D3D11Provider> Create();

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  IDXGIFactory1* GetDXGIFactory() const { return dxgi_factory_.Get(); }
  // Null below Windows 8 / DXGI 1.2 (no flip model, no CreateSwapChainForHwnd).
  IDXGIFactory2* GetDXGIFactory2() const { return dxgi_factory_2_.Get(); }
  ID3D11Device* GetDevice() const { return device_.Get(); }
  // Hold context_mutex() while using it.
  ID3D11DeviceContext* GetContext() const { return context_.Get(); }
  // Direct3D 11.1 context (null on Windows 7 without the platform update).
  ID3D11DeviceContext1* GetContext1() const { return context_1_.Get(); }
  std::recursive_mutex& context_mutex() const { return context_mutex_; }
  D3D_FEATURE_LEVEL GetFeatureLevel() const { return feature_level_; }
  GpuVendorID GetAdapterVendorID() const { return adapter_vendor_id_; }
  // VS/PSSetConstantBuffers1 with offsets work (11.1 runtime + driver).
  bool constant_buffer_offsetting() const { return constant_buffer_offsetting_; }
  bool is_warp() const { return is_warp_; }

 private:
  D3D11Provider() = default;
  bool Initialize();

  Microsoft::WRL::ComPtr<IDXGIFactory1> dxgi_factory_;
  Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory_2_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_1_;
  mutable std::recursive_mutex context_mutex_;
  D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
  GpuVendorID adapter_vendor_id_ = GpuVendorID(0);
  bool constant_buffer_offsetting_ = false;
  bool is_warp_ = false;
};

}  // namespace rex::ui::d3d11
