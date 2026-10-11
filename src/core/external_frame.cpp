// [svr2011] See rex/external_frame.h.

#include <rex/external_frame.h>

#include <atomic>
#include <mutex>

namespace rex::external_frame {

namespace {
std::mutex g_mutex;
Provider g_provider;
std::atomic<bool> g_host_drawing_disabled{false};
std::atomic<void*> g_device{nullptr};
VulkanDevice g_vulkan_device;
std::atomic<bool> g_vulkan_device_set{false};
D3D11Device g_d3d11_device;
std::atomic<bool> g_d3d11_device_set{false};
}  // namespace

void SetD3D11Device(const D3D11Device* device) {
  if (device) g_d3d11_device = *device;
  g_d3d11_device_set = device != nullptr;
}

const D3D11Device* GetD3D11Device() { return g_d3d11_device_set ? &g_d3d11_device : nullptr; }

void SetVulkanDevice(const VulkanDevice* device) {
  if (device) g_vulkan_device = *device;
  g_vulkan_device_set = device != nullptr;
}

const VulkanDevice* GetVulkanDevice() { return g_vulkan_device_set ? &g_vulkan_device : nullptr; }

void SetDevice(void* d3d12_device) { g_device = d3d12_device; }

void* Device() { return g_device.load(); }

void SetProvider(Provider provider) {
  std::lock_guard lock(g_mutex);
  g_provider = std::move(provider);
}

bool GetFrame(Frame& frame) {
  std::lock_guard lock(g_mutex);
  return g_provider && g_provider(frame);
}

void SetHostDrawingDisabled(bool disabled) { g_host_drawing_disabled = disabled; }

bool HostDrawingDisabled() { return g_host_drawing_disabled.load(std::memory_order_relaxed); }

namespace {
std::atomic<uint64_t> g_swaps{0};
}  // namespace

void CountSwap() { g_swaps.fetch_add(1, std::memory_order_relaxed); }

uint64_t SwapCount() { return g_swaps.load(std::memory_order_relaxed); }

}  // namespace rex::external_frame
