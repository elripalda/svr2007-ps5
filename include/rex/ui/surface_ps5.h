#pragma once
/**
 * [ps5 port] PS5: the TV, through VK_KHR_display (PS5_Mesa's RADV winsys).
 * There is no window: the surface is the whole display, sized by the display
 * mode the presenter picks (4K when the TV offers it).
 */

#include <cstdint>

#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class Ps5DisplaySurface final : public Surface {
 public:
  Ps5DisplaySurface(uint32_t width, uint32_t height) : width_(width), height_(height) {}
  TypeIndex GetType() const override { return kTypeIndex_Ps5Display; }
  void SetSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
  }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    width_out = width_;
    height_out = height_;
    return true;
  }

 private:
  uint32_t width_;
  uint32_t height_;
};

}  // namespace ui
}  // namespace rex
