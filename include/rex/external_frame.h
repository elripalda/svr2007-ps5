/**
 * @file        rex/external_frame.h
 * @brief       [svr2011] Frames from an app-side renderer, shown instead of
 *              the emulated GPU's.
 *
 * An app that renders the guest's frames itself (on the same D3D12 or Vulkan
 * device) registers a provider; at each guest swap the backend asks it for the
 * finished frame and shows that (through its usual gamma / scaling / overlay
 * path) instead of the guest front buffer. With host drawing disabled the
 * emulated GPU still runs the guest's command stream (fences, interrupts,
 * memory exports) but skips its own draws and resolves.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

namespace rex::external_frame {

struct Frame {
  // D3D12
  void* resource = nullptr;   // ID3D12Resource*, RGBA8, in a shader-readable state
  uint32_t srv_format = 0;    // DXGI_FORMAT for the view
  uint32_t component_mapping = 0;  // D3D12 Shader4ComponentMapping
  uint32_t width = 0, height = 0;
  void* fence = nullptr;      // ID3D12Fence*: the frame is ready once it reaches
  uint64_t fence_value = 0;   // fence_value (waited for on the GPU)
  // Vulkan: an RGBA8 image in SHADER_READ_ONLY_OPTIMAL, drawn by work
  // submitted earlier on the same queue (so no semaphore).
  uint64_t vk_image = 0;       // VkImage
  uint64_t vk_image_view = 0;  // VkImageView (2D, identity swizzle)
  // Direct3D 11: an RGBA8 ID3D11Texture2D (shader resource) on the same
  // device, drawn by work already issued on its immediate context (so no
  // fence: the context runs in order).
  void* d3d11_texture = nullptr;
};

// The emulator's Direct3D 11 device (set by the D3D11 provider), for an app
// renderer to draw on. One immediate context serves everyone: hold
// `context_mutex` for every sequence of context calls, and set all the state
// needed (nothing may assume another user's state).
struct D3D11Device {
  void* device = nullptr;           // ID3D11Device
  void* context = nullptr;          // ID3D11DeviceContext (immediate)
  std::recursive_mutex* context_mutex = nullptr;
  uint32_t feature_level = 0;       // D3D_FEATURE_LEVEL
  bool constant_buffer_offsetting = false;  // *SetConstantBuffers1 with offsets
  bool warp = false;                // the software rasterizer
  uint32_t vendor_id = 0;
};

// The emulator's Vulkan device (set by the Vulkan backend), for an app
// renderer to draw on: the handles are the Vulkan types; the renderer
// submits to `queue` holding `queue_mutex` (the backend does the same).
struct VulkanDevice {
  void* instance = nullptr;          // VkInstance
  void* get_instance_proc_addr = nullptr;  // PFN_vkGetInstanceProcAddr of the loader it came from
                                           // (a custom driver's: not the system's libvulkan)
  uint32_t api_version = 0;
  void* physical_device = nullptr;   // VkPhysicalDevice
  void* device = nullptr;            // VkDevice
  uint32_t queue_family = 0;         // graphics + compute
  void* queue = nullptr;             // VkQueue
  std::recursive_mutex* queue_mutex = nullptr;
  // Enabled on the device:
  bool app_renderer_features = false;  // descriptor indexing (update after bind), buffer
                                       // device address, shaderInt64
  bool scalar_block_layout = false;
  bool null_descriptor = false;
  bool sampler_mirror_clamp_to_edge = false;
  bool geometry_shader = false;
};

using Provider = std::function<bool(Frame& frame)>;

// Registers the provider (null to remove). Any thread.
void SetProvider(Provider provider);
// The latest frame, or false if there is no provider / no frame yet.
bool GetFrame(Frame& frame);

// The emulator's ID3D12Device (set by the D3D12 backend): an app renderer
// must use the same GPU for its frames to be shown. Null before that.
void SetDevice(void* d3d12_device);
void* Device();
// Vulkan: the same, or null.
void SetVulkanDevice(const VulkanDevice* device);
const VulkanDevice* GetVulkanDevice();
// Direct3D 11: the same, or null.
void SetD3D11Device(const D3D11Device* device);
const D3D11Device* GetD3D11Device();

// While true, the emulated GPU skips drawing (see above).
void SetHostDrawingDisabled(bool disabled);
bool HostDrawingDisabled();

// Guest swaps the backend has finished (any thread): an app can tell a GPU
// that never gets its first frame out (a driver stuck at start).
void CountSwap();
uint64_t SwapCount();

}  // namespace rex::external_frame
