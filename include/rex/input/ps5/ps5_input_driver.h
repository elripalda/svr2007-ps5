#pragma once
/**
 * [ps5 port] DualSense controllers through scePad: one Xbox 360 pad per
 * logged-in PS5 user that has a controller.
 */
#include <array>
#include <cstdint>
#include <mutex>

#include <rex/input/input_driver.h>

namespace rex::input::ps5 {

class Ps5InputDriver final : public InputDriver {
 public:
  explicit Ps5InputDriver(rex::ui::Window* window, size_t window_z_order);
  ~Ps5InputDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  static constexpr size_t kMaxPads = 4;
  struct Pad {
    int32_t user_id = -1;
    int32_t handle = -1;
    DeviceId id = DeviceId::kInvalid;
    uint32_t packet_number = 0;
    X_INPUT_GAMEPAD last_gamepad{};
  };
  void RefreshPads();
  Pad* FindPad(DeviceId id);

  std::mutex mutex_;
  std::array<Pad, kMaxPads> pads_{};
  uint64_t next_device_id_ = 0x50530000;  // "PS"
  uint64_t last_refresh_ms_ = 0;
};

}  // namespace rex::input::ps5
