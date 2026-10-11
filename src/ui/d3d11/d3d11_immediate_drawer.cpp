// [svr2011] Direct3D 11 immediate drawer (rex/ui/d3d11/d3d11_immediate_drawer.h).

#include <rex/ui/d3d11/d3d11_immediate_drawer.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>

#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_presenter.h>

namespace rex::ui::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_4_0/immediate_ps.h"
#include "../shaders/bytecode/d3d11_4_0/immediate_vs.h"
}  // namespace shaders

std::unique_ptr<D3D11ImmediateDrawer> D3D11ImmediateDrawer::Create(const D3D11Provider& provider) {
  std::unique_ptr<D3D11ImmediateDrawer> drawer(new D3D11ImmediateDrawer(provider));
  if (!drawer->Initialize()) {
    return nullptr;
  }
  return drawer;
}

bool D3D11ImmediateDrawer::Initialize() {
  ID3D11Device* device = provider_.GetDevice();
  if (FAILED(device->CreateVertexShader(shaders::immediate_vs, sizeof(shaders::immediate_vs),
                                        nullptr, &vertex_shader_)) ||
      FAILED(device->CreatePixelShader(shaders::immediate_ps, sizeof(shaders::immediate_ps), nullptr,
                                       &pixel_shader_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the shaders");
    return false;
  }
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImmediateVertex, x)),
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImmediateVertex, u)),
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(ImmediateVertex, color)),
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(device->CreateInputLayout(elements, UINT(std::size(elements)), shaders::immediate_vs,
                                       sizeof(shaders::immediate_vs), &input_layout_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the input layout");
    return false;
  }

  D3D11_BLEND_DESC blend = {};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  raster.ScissorEnable = TRUE;
  D3D11_DEPTH_STENCIL_DESC depth = {};
  if (FAILED(device->CreateBlendState(&blend, &blend_state_)) ||
      FAILED(device->CreateRasterizerState(&raster, &rasterizer_state_)) ||
      FAILED(device->CreateDepthStencilState(&depth, &depth_stencil_state_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the state objects");
    return false;
  }

  D3D11_SAMPLER_DESC sampler = {};
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  for (int i = 0; i < kSamplerCount; ++i) {
    const bool linear = i == kLinearClamp || i == kLinearRepeat;
    const bool repeat = i == kNearestRepeat || i == kLinearRepeat;
    sampler.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sampler, &samplers_[i]))) {
      REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the samplers");
      return false;
    }
  }

  D3D11_BUFFER_DESC constants = {};
  constants.ByteWidth = 16;
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&constants, nullptr, &constants_))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create the constant buffer");
    return false;
  }

  const uint32_t white = 0xFFFFFFFF;
  auto texture = CreateTexture(1, 1, ImmediateTextureFilter::kNearest, false,
                               reinterpret_cast<const uint8_t*>(&white));
  if (!texture) {
    return false;
  }
  white_view_ = static_cast<D3D11ImmediateTexture*>(texture.get())->view();
  return true;
}

std::unique_ptr<ImmediateTexture> D3D11ImmediateDrawer::CreateTexture(uint32_t width,
                                                                      uint32_t height,
                                                                      ImmediateTextureFilter filter,
                                                                      bool is_repeated,
                                                                      const uint8_t* data) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA initial = {data, UINT(sizeof(uint32_t) * width), 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  // (Device calls are free-threaded: no context lock.)
  if (!data || FAILED(provider_.GetDevice()->CreateTexture2D(&desc, &initial, &texture)) ||
      FAILED(provider_.GetDevice()->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a {}x{} texture", width, height);
  }
  SamplerIndex sampler = filter == ImmediateTextureFilter::kLinear
                             ? (is_repeated ? kLinearRepeat : kLinearClamp)
                             : (is_repeated ? kNearestRepeat : kNearestClamp);
  return std::make_unique<D3D11ImmediateTexture>(width, height, std::move(view), sampler);
}

bool D3D11ImmediateDrawer::EnsureBuffer(Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer,
                                        UINT& capacity, UINT size, UINT bind) {
  if (buffer && capacity >= size) {
    return true;
  }
  UINT new_capacity = std::max(capacity ? capacity : UINT(64 * 1024), UINT(4096));
  while (new_capacity < size) {
    new_capacity *= 2;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = new_capacity;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  buffer.Reset();
  capacity = 0;
  if (FAILED(provider_.GetDevice()->CreateBuffer(&desc, nullptr, &buffer))) {
    REXLOG_ERROR("D3D11ImmediateDrawer: Failed to create a {}-byte buffer", new_capacity);
    return false;
  }
  capacity = new_capacity;
  return true;
}

void D3D11ImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                 float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  context_ = static_cast<D3D11UIDrawContext&>(ui_draw_context).context();
  batch_open_ = false;

  // The presenter has its back buffer bound as the render target.
  D3D11_VIEWPORT viewport = {};
  viewport.Width = float(ui_draw_context.render_target_width());
  viewport.Height = float(ui_draw_context.render_target_height());
  viewport.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &viewport);
  context_->RSSetState(rasterizer_state_.Get());
  context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);
  context_->OMSetDepthStencilState(depth_stencil_state_.Get(), 0);
  context_->IASetInputLayout(input_layout_.Get());
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context_->GSSetShader(nullptr, nullptr, 0);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    float size_inv[4] = {1.0f / coordinate_space_width, 1.0f / coordinate_space_height, 0, 0};
    std::memcpy(mapped.pData, size_inv, sizeof(size_inv));
    context_->Unmap(constants_.Get(), 0);
  }
  ID3D11Buffer* constants = constants_.Get();
  context_->VSSetConstantBuffers(0, 1, &constants);

  current_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  current_view_ = nullptr;
  current_sampler_ = -1;
  current_scissor_ = {-1, -1, -1, -1};
}

void D3D11ImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  batch_open_ = false;
  const UINT vertex_bytes = UINT(sizeof(ImmediateVertex)) * UINT(batch.vertex_count);
  if (!vertex_bytes ||
      !EnsureBuffer(vertex_buffer_, vertex_buffer_capacity_, vertex_bytes,
                    D3D11_BIND_VERTEX_BUFFER)) {
    return;
  }
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context_->Map(vertex_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return;
  }
  std::memcpy(mapped.pData, batch.vertices, vertex_bytes);
  context_->Unmap(vertex_buffer_.Get(), 0);
  ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
  const UINT stride = UINT(sizeof(ImmediateVertex)), offset = 0;
  context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);

  batch_has_index_buffer_ = batch.indices != nullptr;
  if (batch_has_index_buffer_) {
    const UINT index_bytes = UINT(sizeof(uint16_t)) * UINT(batch.index_count);
    if (!EnsureBuffer(index_buffer_, index_buffer_capacity_, index_bytes,
                      D3D11_BIND_INDEX_BUFFER) ||
        FAILED(context_->Map(index_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return;
    }
    std::memcpy(mapped.pData, batch.indices, index_bytes);
    context_->Unmap(index_buffer_.Get(), 0);
    context_->IASetIndexBuffer(index_buffer_.Get(), DXGI_FORMAT_R16_UINT, 0);
  }
  batch_open_ = true;
}

void D3D11ImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_open_) {
    return;
  }
  uint32_t left, top, width, height;
  if (!ScissorToRenderTarget(draw, left, top, width, height)) {
    return;
  }
  const D3D11_RECT scissor = {LONG(left), LONG(top), LONG(left + width), LONG(top + height)};
  if (std::memcmp(&scissor, &current_scissor_, sizeof(scissor))) {
    current_scissor_ = scissor;
    context_->RSSetScissorRects(1, &scissor);
  }

  auto* texture = static_cast<D3D11ImmediateTexture*>(draw.texture);
  ID3D11ShaderResourceView* view = texture && texture->view() ? texture->view() : white_view_.Get();
  if (view != current_view_) {
    current_view_ = view;
    context_->PSSetShaderResources(0, 1, &view);
  }
  const int sampler = texture && texture->view() ? texture->sampler() : kNearestClamp;
  if (sampler != current_sampler_) {
    current_sampler_ = sampler;
    ID3D11SamplerState* state = samplers_[sampler].Get();
    context_->PSSetSamplers(0, 1, &state);
  }

  const D3D11_PRIMITIVE_TOPOLOGY topology = draw.primitive_type == ImmediatePrimitiveType::kLines
                                                ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (topology != current_topology_) {
    current_topology_ = topology;
    context_->IASetPrimitiveTopology(topology);
  }

  if (batch_has_index_buffer_) {
    context_->DrawIndexed(UINT(draw.count), UINT(draw.index_offset), draw.base_vertex);
  } else {
    context_->Draw(UINT(draw.count), UINT(draw.base_vertex));
  }
}

void D3D11ImmediateDrawer::EndDrawBatch() { batch_open_ = false; }

void D3D11ImmediateDrawer::End() {
  ImmediateDrawer::End();
  context_ = nullptr;
}

}  // namespace rex::ui::d3d11
