// [svr2011] Direct3D 11 graphics system (rex/graphics/d3d11/graphics_system.h).

#include <rex/graphics/d3d11/graphics_system.h>

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11GraphicsSystem::D3D11GraphicsSystem() {}

D3D11GraphicsSystem::~D3D11GraphicsSystem() {}

bool D3D11GraphicsSystem::IsAvailable() {
  return rex::ui::d3d11::D3D11Provider::IsD3D11APIAvailable();
}

void D3D11GraphicsSystem::CreateProvider(bool /*with_presentation*/) {
  provider_ = rex::ui::d3d11::D3D11Provider::Create();
}

std::unique_ptr<CommandProcessor> D3D11GraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new D3D11CommandProcessor(this, kernel_state_));
}

}  // namespace rex::graphics::d3d11
