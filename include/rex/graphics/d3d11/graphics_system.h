/**
 * @file        graphics/d3d11/graphics_system.h
 * @brief       [svr2011] Direct3D 11 graphics system: the D3D11 provider and
 *              presenter, and a command processor without emulated drawing
 *              (rex/graphics/d3d11/command_processor.h).
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::d3d11 {

class D3D11GraphicsSystem : public GraphicsSystem {
 public:
  D3D11GraphicsSystem();
  ~D3D11GraphicsSystem() override;

  static bool IsAvailable();

  std::string name() const override { return "Direct3D 11"; }

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::d3d11
