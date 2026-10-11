/**
 * [ps5 port] DualSense input via scePad (layout verified on hardware by
 * Nazky's ps5-plume-triangle).
 *
 * Mapping (Xbox 360 <- DualSense): A <- Cross, B <- Circle, X <- Square,
 * Y <- Triangle, LB/RB <- L1/R1, LT/RT <- L2/R2 (analog), START <- Options,
 * BACK <- touchpad click, stick clicks <- L3/R3, d-pad <- d-pad.
 */
#include <rex/input/ps5/ps5_input_driver.h>

#include <chrono>
#include <cstring>

#include <rex/logging.h>

extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user_id);
int sceUserServiceGetInitialUser(int32_t* user_id);
struct SceUserServiceLoginUserIdList {
  int32_t user_id[4];
};
int sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList* list);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int scePadClose(int32_t handle);
struct ScePadVibrationParam {
  uint8_t large_motor;
  uint8_t small_motor;
};
int scePadSetVibration(int32_t handle, const ScePadVibrationParam* param);
}

namespace rex::input::ps5 {

namespace {

constexpr int32_t kInvalidUser = -1;
constexpr int32_t kSystemUser = 0xFF;

// The head of ScePadData (the rest - touch, motion, ... - is unused).
struct ScePadDataHead {
  uint32_t buttons;
  uint8_t left_stick_x;
  uint8_t left_stick_y;
  uint8_t right_stick_x;
  uint8_t right_stick_y;
  uint8_t analog_l2;
  uint8_t analog_r2;
  uint16_t padding;
  uint8_t rest[240];  // room for the whole ScePadData
};

constexpr uint32_t kPadL3 = 0x00000002u;
constexpr uint32_t kPadR3 = 0x00000004u;
constexpr uint32_t kPadOptions = 0x00000008u;
constexpr uint32_t kPadUp = 0x00000010u;
constexpr uint32_t kPadRight = 0x00000020u;
constexpr uint32_t kPadDown = 0x00000040u;
constexpr uint32_t kPadLeft = 0x00000080u;
constexpr uint32_t kPadL1 = 0x00000400u;
constexpr uint32_t kPadR1 = 0x00000800u;
constexpr uint32_t kPadTriangle = 0x00001000u;
constexpr uint32_t kPadCircle = 0x00002000u;
constexpr uint32_t kPadCross = 0x00004000u;
constexpr uint32_t kPadSquare = 0x00008000u;
constexpr uint32_t kPadTouchpad = 0x00100000u;

// 0..255 (128 centre) to the Xbox's -32768..32767, Y flipped so up is +.
int16_t AxisToXbox(uint8_t value, bool flip) {
  int v = (int(value) - 128) * 256;
  if (flip) v = -v - 1;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return int16_t(v);
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

Ps5InputDriver::Ps5InputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

Ps5InputDriver::~Ps5InputDriver() {
  for (Pad& pad : pads_) {
    if (pad.handle >= 0) scePadClose(pad.handle);
  }
}

X_STATUS Ps5InputDriver::Setup() {
  sceUserServiceInitialize(nullptr);
  int rc = scePadInit();
  if (rc < 0) {
    REXLOG_ERROR("PS5 input: scePadInit failed ({:#x})", uint32_t(rc));
    return X_STATUS_UNSUCCESSFUL;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  RefreshPads();
  return X_STATUS_SUCCESS;
}

// Opens a pad for every logged-in user that hasn't one yet (users log in and
// controllers connect while the game runs). Called at most twice a second.
void Ps5InputDriver::RefreshPads() {
  SceUserServiceLoginUserIdList users;
  for (int32_t& u : users.user_id) u = kInvalidUser;
  if (sceUserServiceGetLoginUserIdList(&users) < 0) {
    int32_t fg = kInvalidUser;
    if (sceUserServiceGetForegroundUser(&fg) == 0) users.user_id[0] = fg;
  }
  for (int32_t user : users.user_id) {
    if (user == kInvalidUser || user == kSystemUser) continue;
    bool known = false;
    for (const Pad& pad : pads_) known |= pad.user_id == user && pad.handle >= 0;
    if (known) continue;
    for (Pad& pad : pads_) {
      if (pad.handle >= 0) continue;
      int32_t handle = scePadOpen(user, 0, 0, nullptr);
      if (handle < 0) handle = scePadGetHandle(user, 0, 0);
      if (handle < 0) break;
      pad.user_id = user;
      pad.handle = handle;
      pad.id = DeviceId(++next_device_id_);
      pad.packet_number = 0;
      REXLOG_INFO("PS5 input: DualSense for user {:#x} (handle {})", uint32_t(user), handle);
      break;
    }
  }
  last_refresh_ms_ = NowMs();
}

Ps5InputDriver::Pad* Ps5InputDriver::FindPad(DeviceId id) {
  for (Pad& pad : pads_) {
    if (pad.handle >= 0 && pad.id == id) return &pad;
  }
  return nullptr;
}

void Ps5InputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (NowMs() - last_refresh_ms_ > 500) RefreshPads();
  for (const Pad& pad : pads_) {
    if (pad.handle < 0) continue;
    DeviceInfo info;
    info.id = pad.id;
    info.name = "DualSense";
    info.guid = "ps5-dualsense";
    out.push_back(info);
  }
}

X_RESULT Ps5InputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  Pad* pad = FindPad(id);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  ScePadDataHead data;
  std::memset(&data, 0, sizeof(data));
  if (scePadReadState(pad->handle, &data) != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_state) return X_ERROR_SUCCESS;
  const uint32_t b = is_active() ? data.buttons : 0;
  uint16_t buttons = 0;
  if (b & kPadUp) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (b & kPadDown) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (b & kPadLeft) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (b & kPadRight) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (b & kPadOptions) buttons |= X_INPUT_GAMEPAD_START;
  if (b & kPadTouchpad) buttons |= X_INPUT_GAMEPAD_BACK;
  if (b & kPadL3) buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (b & kPadR3) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (b & kPadL1) buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (b & kPadR1) buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (b & kPadCross) buttons |= X_INPUT_GAMEPAD_A;
  if (b & kPadCircle) buttons |= X_INPUT_GAMEPAD_B;
  if (b & kPadSquare) buttons |= X_INPUT_GAMEPAD_X;
  if (b & kPadTriangle) buttons |= X_INPUT_GAMEPAD_Y;

  const bool active = is_active();
  X_INPUT_GAMEPAD& gp = out_state->gamepad;
  gp.buttons = buttons;
  gp.left_trigger = active ? data.analog_l2 : 0;
  gp.right_trigger = active ? data.analog_r2 : 0;
  gp.thumb_lx = active ? AxisToXbox(data.left_stick_x, false) : 0;
  gp.thumb_ly = active ? AxisToXbox(data.left_stick_y, true) : 0;
  gp.thumb_rx = active ? AxisToXbox(data.right_stick_x, false) : 0;
  gp.thumb_ry = active ? AxisToXbox(data.right_stick_y, true) : 0;
  // The packet number only moves when the state does, as XInput's does.
  if (std::memcmp(&gp, &pad->last_gamepad, sizeof(gp)) != 0) {
    ++pad->packet_number;
    std::memcpy(&pad->last_gamepad, &gp, sizeof(gp));
  }
  out_state->packet_number = pad->packet_number;
  return X_ERROR_SUCCESS;
}

X_RESULT Ps5InputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!FindPad(id)) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
    out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT Ps5InputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Pad* pad = FindPad(id);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (vibration) {
    ScePadVibrationParam param;
    param.large_motor = uint8_t(uint16_t(vibration->left_motor_speed) >> 8);
    param.small_motor = uint8_t(uint16_t(vibration->right_motor_speed) >> 8);
    scePadSetVibration(pad->handle, &param);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT Ps5InputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!FindPad(id)) return X_ERROR_DEVICE_NOT_CONNECTED;
  return X_ERROR_EMPTY;  // (keystroke events: not yet)
}

}  // namespace rex::input::ps5
