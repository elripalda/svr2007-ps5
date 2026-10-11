// [svr2011] Direct3D 11 graphics provider (rex/ui/d3d11/d3d11_provider.h).

#include <rex/ui/d3d11/d3d11_provider.h>

#include <iterator>
#include <string>
#include <vector>

#include <dxgi1_6.h>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_immediate_drawer.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

REXCVAR_DEFINE_BOOL(d3d11_debug, false, "UI/D3D11", "Enable the Direct3D 11 debug layer")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(d3d11_adapter, -1, "UI/D3D11",
                     "Index of the DXGI adapter to use (-1 for the high-performance physical GPU, "
                     "-2 for WARP, the software rasterizer)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(d3d11_feature_level, 0, "UI/D3D11",
                     "Highest Direct3D feature level to use (0 for the GPU's highest; 110 for 11_0, "
                     "101 for 10_1, 100 for 10_0 - to test what older GPUs get)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::ui::d3d11 {

namespace {

const char* FeatureLevelName(D3D_FEATURE_LEVEL level) {
  switch (level) {
    case D3D_FEATURE_LEVEL_12_1:
      return "12_1";
    case D3D_FEATURE_LEVEL_12_0:
      return "12_0";
    case D3D_FEATURE_LEVEL_11_1:
      return "11_1";
    case D3D_FEATURE_LEVEL_11_0:
      return "11_0";
    case D3D_FEATURE_LEVEL_10_1:
      return "10_1";
    case D3D_FEATURE_LEVEL_10_0:
      return "10_0";
    default:
      return "9_x";
  }
}

}  // namespace

bool D3D11Provider::IsD3D11APIAvailable() {
  HMODULE library = LoadLibraryW(L"d3d11.dll");
  if (!library) {
    return false;
  }
  FreeLibrary(library);
  return true;
}

std::unique_ptr<D3D11Provider> D3D11Provider::Create() {
  std::unique_ptr<D3D11Provider> provider(new D3D11Provider);
  if (!provider->Initialize()) {
    REXLOG_ERROR("D3D11Provider: no Direct3D 11 device (feature level 10_0 or newer) could be created");
    return nullptr;
  }
  return provider;
}

D3D11Provider::~D3D11Provider() {
  rex::external_frame::SetD3D11Device(nullptr);
  if (context_) {
    std::lock_guard lock(context_mutex_);
    context_->ClearState();
    context_->Flush();
  }
}

std::unique_ptr<Presenter> D3D11Provider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return D3D11Presenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> D3D11Provider::CreateImmediateDrawer() {
  return D3D11ImmediateDrawer::Create(*this);
}

bool D3D11Provider::Initialize() {
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory_)))) {
    REXLOG_ERROR("D3D11Provider: Failed to create a DXGI factory");
    return false;
  }
  dxgi_factory_.As(&dxgi_factory_2_);

  // The feature levels to try, highest first, capped by d3d11_feature_level.
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                              D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  size_t first_level = 0;
  switch (REXCVAR_GET(d3d11_feature_level)) {
    case 110:
      first_level = 1;
      break;
    case 101:
      first_level = 2;
      break;
    case 100:
      first_level = 3;
      break;
    default:
      break;
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (REXCVAR_GET(d3d11_debug)) {
    flags |= D3D11_CREATE_DEVICE_DEBUG;
  }

  auto create = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) -> bool {
    for (size_t first = first_level; first < std::size(kLevels); ++first) {
      HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, kLevels + first,
                                     UINT(std::size(kLevels) - first), D3D11_SDK_VERSION,
                                     &device_, &feature_level_, &context_);
      if (SUCCEEDED(hr)) {
        return true;
      }
      if ((flags & D3D11_CREATE_DEVICE_DEBUG) && hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        REXLOG_WARN("D3D11Provider: the debug layer isn't installed - creating the device without it");
        flags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
        --first;
        continue;
      }
      // The Windows 7 runtime without the platform update rejects 11_1 in the
      // list (E_INVALIDARG): the loop retries from the next level down.
      if (hr != E_INVALIDARG) {
        return false;
      }
    }
    return false;
  };

  const int32_t adapter_choice = REXCVAR_GET(d3d11_adapter);
  bool created = false;
  if (adapter_choice == -2) {
    created = create(nullptr, D3D_DRIVER_TYPE_WARP);
    is_warp_ = created;
  } else {
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> adapters;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
    if (adapter_choice == -1 && SUCCEEDED(dxgi_factory_.As(&factory6))) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                            IID_PPV_ARGS(&adapter)) == S_OK;
           ++i) {
        adapters.push_back(std::move(adapter));
      }
    } else {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; dxgi_factory_->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        adapters.push_back(std::move(adapter));
      }
    }
    for (size_t i = 0; i < adapters.size() && !created; ++i) {
      DXGI_ADAPTER_DESC1 desc = {};
      adapters[i]->GetDesc1(&desc);
      if (adapter_choice >= 0 ? int32_t(i) != adapter_choice
                              : (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        continue;
      }
      created = create(adapters[i].Get(), D3D_DRIVER_TYPE_UNKNOWN);
    }
  }
  if (!created) {
    return false;
  }

  context_.As(&context_1_);

  // The adapter the device is on (vendor, name for the log).
  std::string adapter_name = "?";
  {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(device_.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
      adapter_vendor_id_ = GpuVendorID(desc.VendorId);
      const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        adapter_name.resize(size_t(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter_name.data(), n, nullptr, nullptr);
      }
      if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8C) {
        is_warp_ = true;  // (Microsoft Basic Render Driver)
      }
    }
  }

  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                             sizeof(options)))) {
    constant_buffer_offsetting_ = options.ConstantBufferOffsetting && context_1_;
  }

  REXLOG_INFO("D3D11Provider: {} (vendor {:04X}), feature level {}{}, constant buffer offsets {}",
              adapter_name, uint32_t(adapter_vendor_id_), FeatureLevelName(feature_level_),
              is_warp_ ? " (WARP)" : "", constant_buffer_offsetting_ ? "yes" : "no");

  rex::external_frame::D3D11Device info;
  info.device = device_.Get();
  info.context = context_.Get();
  info.context_mutex = &context_mutex_;
  info.feature_level = uint32_t(feature_level_);
  info.constant_buffer_offsetting = constant_buffer_offsetting_;
  info.warp = is_warp_;
  info.vendor_id = uint32_t(adapter_vendor_id_);
  rex::external_frame::SetD3D11Device(&info);  // app renderers use this device
  return true;
}

}  // namespace rex::ui::d3d11
