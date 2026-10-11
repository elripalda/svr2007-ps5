#pragma once
/**
 * [svr2011] Android: a Vulkan surface on an ANativeWindow (after Xenia's
 * xenia/ui/surface_android.h).
 */

#include <rex/ui/surface.h>

struct ANativeWindow;

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
