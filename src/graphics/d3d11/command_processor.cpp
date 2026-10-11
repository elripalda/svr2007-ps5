// [svr2011] Direct3D 11 command processor (rex/graphics/d3d11/command_processor.h).

#include <rex/graphics/d3d11/command_processor.h>

#include <algorithm>
#include <cstring>

#include <rex/external_frame.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

#include <xxhash.h>

namespace rex::graphics::d3d11 {

namespace shaders {
#include "ui/shaders/bytecode/d3d11_4_0/apply_gamma_table_ps.h"
#include "ui/shaders/bytecode/d3d11_4_0/guest_output_rect_vs.h"
}  // namespace shaders

D3D11CommandProcessor::D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

D3D11CommandProcessor::~D3D11CommandProcessor() = default;

ui::d3d11::D3D11Provider& D3D11CommandProcessor::provider() const {
  return *static_cast<ui::d3d11::D3D11Provider*>(graphics_system_->provider());
}

bool D3D11CommandProcessor::SetupContext() {
  ID3D11Device* device = provider().GetDevice();
  if (FAILED(device->CreateVertexShader(shaders::guest_output_rect_vs,
                                        sizeof(shaders::guest_output_rect_vs), nullptr,
                                        &rect_vs_)) ||
      FAILED(device->CreatePixelShader(shaders::apply_gamma_table_ps,
                                       sizeof(shaders::apply_gamma_table_ps), nullptr,
                                       &apply_gamma_ps_))) {
    REXGPU_ERROR("D3D11: Failed to create the gamma ramp shaders");
    return false;
  }
  // The whole target: offset (-1, 1), size (2, -2) in NDC.
  const float rect[4] = {-1.0f, 1.0f, 2.0f, -2.0f};
  D3D11_BUFFER_DESC constants = {};
  constants.ByteWidth = sizeof(rect);
  constants.Usage = D3D11_USAGE_IMMUTABLE;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  D3D11_SUBRESOURCE_DATA constants_data = {rect, 0, 0};
  D3D11_TEXTURE2D_DESC ramp = {};
  ramp.Width = 256;
  ramp.Height = 1;
  ramp.MipLevels = 1;
  ramp.ArraySize = 1;
  ramp.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  ramp.SampleDesc.Count = 1;
  ramp.Usage = D3D11_USAGE_DEFAULT;
  ramp.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  if (FAILED(device->CreateBuffer(&constants, &constants_data, &rect_constants_)) ||
      FAILED(device->CreateTexture2D(&ramp, nullptr, &gamma_ramp_)) ||
      FAILED(device->CreateShaderResourceView(gamma_ramp_.Get(), nullptr, &gamma_ramp_srv_)) ||
      FAILED(device->CreateRasterizerState(&raster, &rasterizer_))) {
    REXGPU_ERROR("D3D11: Failed to create the gamma ramp resources");
    return false;
  }
  gamma_ramp_up_to_date_ = false;
  REXGPU_INFO("D3D11: guest GPU commands run without emulated drawing (the app draws the frames)");
  return true;
}

void D3D11CommandProcessor::ShutdownContext() {
  frame_srv_.Reset();
  frame_texture_ = nullptr;
  gamma_ramp_srv_.Reset();
  gamma_ramp_.Reset();
  rect_constants_.Reset();
  apply_gamma_ps_.Reset();
  rect_vs_.Reset();
  rasterizer_.Reset();
  shaders_.clear();
}

Shader* D3D11CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  // (Only to know which draws use memexport: those need the emulated GPU.)
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  if (shader->memexport_eM_written()) {
    REXGPU_WARN("D3D11: shader {:016X} at {:08X} uses memexport - its draws are not run", hash,
                guest_address);
  }
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool D3D11CommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  const Shader* vs = active_vertex_shader();
  const Shader* ps = active_pixel_shader();
  if ((vs && vs->memexport_eM_written()) || (ps && ps->memexport_eM_written())) {
    if (memexport_draws_++ < 8) {
      REXGPU_WARN("D3D11: memexport draw #{} skipped (no emulated GPU drawing)", memexport_draws_);
    }
  }
  return true;
}

bool D3D11CommandProcessor::IssueCopy() { return true; }

void D3D11CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // (the GPU thread got this far: a frame's commands all went through)
  rex::external_frame::CountSwap();
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }
  rex::external_frame::Frame frame;
  if (!rex::external_frame::GetFrame(frame) || !frame.d3d11_texture || !frame.width ||
      !frame.height) {
    // Nothing to show (yet): black, with the UI on top.
    presenter->RefreshGuestOutput(0, 0, 1, 1, nullptr);
    return;
  }
  const uint32_t width = frame.width, height = frame.height;
  presenter->RefreshGuestOutput(
      width, height, width, height,
      [this, &frame, width, height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d11_context =
            static_cast<ui::d3d11::D3D11Presenter::D3D11GuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        ui::d3d11::D3D11Provider& d3d11 = provider();
        std::lock_guard lock(d3d11.context_mutex());
        ID3D11DeviceContext* ctx = d3d11.GetContext();
        if (frame.d3d11_texture != frame_texture_ || !frame_srv_) {
          frame_srv_.Reset();
          frame_texture_ = frame.d3d11_texture;
          D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
          srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
          srv.Texture2D.MipLevels = 1;
          if (FAILED(d3d11.GetDevice()->CreateShaderResourceView(
                  static_cast<ID3D11Texture2D*>(frame.d3d11_texture), &srv, &frame_srv_))) {
            frame_texture_ = nullptr;
            return false;
          }
        }
        if (!gamma_ramp_up_to_date_) {
          ctx->UpdateSubresource(gamma_ramp_.Get(), 0, nullptr, gamma_ramp_256_entry_table(),
                                 256 * sizeof(uint32_t), 0);
          gamma_ramp_up_to_date_ = true;
        }
        ctx->ClearState();
        ID3D11RenderTargetView* rtv = d3d11_context.rtv();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = float(width);
        viewport.Height = float(height);
        viewport.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &viewport);
        ctx->RSSetState(rasterizer_.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11Buffer* constants = rect_constants_.Get();
        ctx->VSSetConstantBuffers(0, 1, &constants);
        ctx->VSSetShader(rect_vs_.Get(), nullptr, 0);
        ctx->PSSetShader(apply_gamma_ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[2] = {frame_srv_.Get(), gamma_ramp_srv_.Get()};
        ctx->PSSetShaderResources(0, 2, views);
        ctx->Draw(4, 0);
        ctx->ClearState();
        // Debug: SVR2011_D3D11_PROBE=1 logs the average colour of the app's
        // frame and of the guest output every 300 swaps.
        static const bool probe = std::getenv("SVR2011_D3D11_PROBE") != nullptr;
        static uint32_t swaps = 0;
        if (probe && ++swaps % 300 == 1) {
          auto average = [&](ID3D11Texture2D* texture, bool rgb10) {
            D3D11_TEXTURE2D_DESC desc;
            texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.MipLevels = 1;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            double sum[3] = {};
            if (FAILED(d3d11.GetDevice()->CreateTexture2D(&desc, nullptr, &staging))) return std::string("?");
            ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return std::string("?");
            for (uint32_t y = 0; y < desc.Height; y += 8) {
              const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
              for (uint32_t x = 0; x < desc.Width; x += 8) {
                const uint32_t v = row[x];
                for (int c = 0; c < 3; ++c)
                  sum[c] += rgb10 ? ((v >> (10 * c)) & 0x3FF) / 1023.0 : ((v >> (8 * c)) & 0xFF) / 255.0;
              }
            }
            ctx->Unmap(staging.Get(), 0);
            const double n = double((desc.Width + 7) / 8) * double((desc.Height + 7) / 8);
            return fmt::format("{:.3f} {:.3f} {:.3f}", sum[0] / n, sum[1] / n, sum[2] / n);
          };
          REXGPU_INFO("D3D11 probe: swap {} frame avg {} -> guest output avg {}", swaps,
                      average(static_cast<ID3D11Texture2D*>(frame.d3d11_texture), false),
                      average(d3d11_context.texture(), true));
        }
        return true;
      });
}

}  // namespace rex::graphics::d3d11
