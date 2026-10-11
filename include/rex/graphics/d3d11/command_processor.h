/**
 * @file        graphics/d3d11/command_processor.h
 * @brief       [svr2011] Direct3D 11 command processor: the guest's command
 *              stream without emulated drawing.
 *
 * For apps that draw the guest's frames with their own renderer
 * (rex/external_frame.h) on GPUs without Direct3D 12 / Vulkan: the base
 * class runs the PM4 stream on the CPU (registers, fences, interrupts, waits,
 * swaps); draws and copies (resolves) are skipped, and memexport draws, which
 * would need the emulated GPU, are only counted and logged. At each swap the
 * app's frame gets the guest's gamma ramp (a pixel shader pass, as the
 * D3D12 / Vulkan backends' compute pass does) into the presenter's guest
 * output.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/string/buffer.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

class D3D11GraphicsSystem;

class D3D11CommandProcessor final : public CommandProcessor {
 public:
  D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~D3D11CommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void OnGammaRamp256EntryTableValueWritten() override { gamma_ramp_up_to_date_ = false; }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  ui::d3d11::D3D11Provider& provider() const;

  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  string::StringBuffer ucode_disasm_buffer_;
  uint64_t memexport_draws_ = 0;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> rect_vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> apply_gamma_ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> rect_constants_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> gamma_ramp_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gamma_ramp_srv_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  bool gamma_ramp_up_to_date_ = false;
  // The app frame's view (recreated when the frame's texture changes).
  void* frame_texture_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frame_srv_;
};

}  // namespace rex::graphics::d3d11
