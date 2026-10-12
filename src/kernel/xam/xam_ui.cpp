/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

#include <rex/input/input.h>
#include <rex/input/input_system.h>

REXCVAR_DEFINE_BOOL(headless, false, "Kernel",
                    "Don't display any UI, using defaults for prompts as needed");
#include <rex/kernel/xam/private.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/pad_dialog_style.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).

extern std::atomic<int> xam_dialogs_shown_;

class XamDialog : public rex::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {}

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog, std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback]() -> X_RESULT {
    X_RESULT result;
    dialog->set_close_callback(
        [&dialog, &result, &close_callback]() { result = close_callback(dialog); });
    rex::thread::Fence fence;
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(T* dialog,
                               std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
                               uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    X_RESULT result;
    dialog->set_close_callback([&dialog, &result, &extended_error, &length, &close_callback]() {
      result = close_callback(dialog, extended_error, length);
    });
    rex::thread::Fence fence;
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  auto pre = []() {
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = true");
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = false");
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadlessEx(std::function<X_RESULT(uint32_t&, uint32_t&)> run_callback,
                                 uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run_callback(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

// [svr2011] Gamer cards: there are none on the PC (the game's button for
// them is removed from its menus, online.cpp); nothing to show.
u32 XamShowGamerCardUIForXUID_entry(u32 user_index, u64 xuid) {
  return X_ERROR_SUCCESS;
}

u32 XamShowGamerCardUI_entry(u32 user_index) {
  return X_ERROR_SUCCESS;
}

u32 XamIsUIActive_entry() {
  return xeXamIsUIActive();
}


namespace {
// [ps5 port] The controller drives the system dialogs (a console has no mouse
// or keyboard for them): Poll() gives the buttons pressed since the last frame,
// with hold-to-repeat on the d-pad / left stick. Buttons already held when the
// dialog opens (the press that opened it) don't count.
class DialogPad {
 public:
  uint16_t Poll() {
    using namespace rex::input;
    using clock = std::chrono::steady_clock;
    uint16_t now = 0;
    auto* is = static_cast<InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
    X_INPUT_STATE st{};
    if (is && is->GetState(0, &st) == X_ERROR_SUCCESS) {
      now = st.gamepad.buttons;
      const int lx = st.gamepad.thumb_lx, ly = st.gamepad.thumb_ly;
      if (lx < -16000) now |= X_INPUT_GAMEPAD_DPAD_LEFT;
      if (lx > 16000) now |= X_INPUT_GAMEPAD_DPAD_RIGHT;
      if (ly > 16000) now |= X_INPUT_GAMEPAD_DPAD_UP;
      if (ly < -16000) now |= X_INPUT_GAMEPAD_DPAD_DOWN;
    }
    const auto t = clock::now();
    uint16_t pressed = now & ~held_;
    constexpr uint16_t kDirs = 0x000F;
    if (now & kDirs) {
      if ((now & kDirs) != (held_ & kDirs)) {
        repeat_at_ = t + std::chrono::milliseconds(380);
      } else if (t >= repeat_at_) {
        pressed |= now & kDirs;
        repeat_at_ = t + std::chrono::milliseconds(85);
      }
    }
    if (first_) {
      pressed = 0;
      first_ = false;
    }
    held_ = now;
    return pressed;
  }

 private:
  uint16_t held_ = 0;
  bool first_ = true;
  std::chrono::steady_clock::time_point repeat_at_{};
};

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
                   kPadStart = 0x0010, kPadL1 = 0x0100, kPadR1 = 0x0200, kPadA = 0x1000,
                   kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;

// A button drawn highlighted when it has the controller's focus.
bool PadButton(const char* label, bool focused, const ImVec2& size = ImVec2(0, 0)) {
  if (focused) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
  }
  bool clicked = ImGui::Button(label, size);
  if (focused) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }
  return clicked;
}

// [ps5 port] The on-screen keyboard in the app's style (rex/ui/pad_dialog_style.h):
// a QWERTY board like the console's own, the focus gliding between the keys,
// and the controller's buttons shown for what they do. Laid out on a
// 1920 x 1080 page, scaled to the display.
class StyledPadKeyboard {
 public:
  StyledPadKeyboard(std::string title, std::string description, std::string text, size_t limit)
      : title_(std::move(title)), description_(std::move(description)), text_(std::move(text)), limit_(limit) {
    upper_ = text_.empty();
  }
  const std::string& text() const { return text_; }

  // One frame; `pressed`: the buttons pressed since the last (Xbox bits).
  // 0: still open, 1: done, -1: cancelled.
  int Draw(const rex::ui::PadDialogStyle& st, uint16_t pressed, float dt) {
    st_ = &st;
    dt_ = std::clamp(dt, 0.0f, 0.1f);
    time_ += dt_;
    const int result = closing_ ? 0 : Input(pressed);
    if (result) closing_ = result;
    Ease(in_, closing_ ? 0.0f : 1.0f, closing_ ? 18.0f : 12.0f);
    Ease(press_, 0, 10);
    Ease(value_in_, 1, 14);
    Render();
    return closing_ && in_ < 0.02f ? closing_ : 0;
  }

 private:
  static constexpr int kCols = 11;
  static constexpr const char* kRows[2][4] = {{"1234567890-", "QWERTYUIOP'", "ASDFGHJKL.!", "ZXCVBNM,?&_"},
                                              {"1234567890-", "qwertyuiop'", "asdfghjkl.!", "zxcvbnm,?&_"}};
  // the last row: SHIFT, SPACE, DELETE, DONE - their widths in columns
  static constexpr int kSpecialSpan[4] = {2, 5, 2, 2};
  static constexpr float kKeyW = 84, kKeyH = 70, kGap = 10;
  static constexpr float kW = 1920, kH = 1080, kPanelW = 1152;

  // ------------------------------------------------------------ the keys
  int SpecialAt(int col) const {  // the special key under a column
    int c = 0;
    for (int k = 0; k < 4; ++k) {
      c += kSpecialSpan[k];
      if (col < c) return k;
    }
    return 3;
  }
  int SpecialCol(int k) const {  // a special key's middle column
    int c = 0;
    for (int i = 0; i < k; ++i) c += kSpecialSpan[i];
    return c + kSpecialSpan[k] / 2;
  }
  // the focused key's box (page coordinates)
  void KeyBox(int row, int col, float* x, float* y, float* w) const {
    const float gx = (kW - (kCols * kKeyW + (kCols - 1) * kGap)) * 0.5f, gy = GridY();
    *y = gy + row * (kKeyH + kGap);
    if (row < 4) {
      *x = gx + col * (kKeyW + kGap);
      *w = kKeyW;
      return;
    }
    const int k = SpecialAt(col);
    int c0 = 0;
    for (int i = 0; i < k; ++i) c0 += kSpecialSpan[i];
    *x = gx + c0 * (kKeyW + kGap);
    *w = kSpecialSpan[k] * kKeyW + (kSpecialSpan[k] - 1) * kGap;
  }
  float PanelY() const { return 150 + (1 - Smooth(in_)) * 40; }
  float GridY() const { return PanelY() + (description_.empty() ? 250 : 290); }

  void Sound(int s) {
    if (st_->sound) st_->sound(s);
  }
  bool Full() const { return text_.size() >= limit_; }
  void Type(char c) {
    if (Full()) {
      Sound(7);
      return;
    }
    text_.push_back(c);
    value_in_ = 0;
    // (a capital first letter, then small ones - unless Shift is held down by the player)
    if (upper_ && !caps_ && std::isalpha(static_cast<unsigned char>(c))) upper_ = false;
    Sound(1);
  }
  void Erase() {
    if (text_.empty()) {
      Sound(7);
      return;
    }
    text_.pop_back();
    if (text_.empty() && !caps_) upper_ = true;
    Sound(2);
  }
  void Shift() {
    // Shift once: the next letter; twice: caps lock; again: off
    if (!upper_) upper_ = true, caps_ = false;
    else if (!caps_) caps_ = true;
    else upper_ = caps_ = false;
    Sound(3);
  }

  int Input(uint16_t p) {
    const int before_row = row_, before_col = col_;
    if (p & kPadLeft) {
      if (row_ < 4) col_ = (col_ + kCols - 1) % kCols;
      else col_ = SpecialCol((SpecialAt(col_) + 3) % 4);
    }
    if (p & kPadRight) {
      if (row_ < 4) col_ = (col_ + 1) % kCols;
      else col_ = SpecialCol((SpecialAt(col_) + 1) % 4);
    }
    if (p & kPadDown) {
      row_ = (row_ + 1) % 5;
      if (row_ == 4) col_ = SpecialCol(SpecialAt(col_));
    }
    if (p & kPadUp) {
      row_ = (row_ + 4) % 5;
      if (row_ == 4) col_ = SpecialCol(SpecialAt(col_));
    }
    if (row_ != before_row || col_ != before_col) Sound(0);
    if (p & kPadL1) Shift();
    if (p & kPadX) press_ = 0.6f, Erase();
    if (p & kPadY) press_ = 0.6f, Type(' ');
    if (p & kPadA) {
      press_ = 1;
      if (row_ < 4) {
        Type(kRows[upper_ ? 0 : 1][row_][col_]);
      } else {
        switch (SpecialAt(col_)) {
          case 0: Shift(); break;
          case 1: Type(' '); break;
          case 2: Erase(); break;
          case 3: Sound(4); return 1;
        }
      }
    }
    if (p & kPadStart) {
      Sound(4);
      return 1;
    }
    if (p & kPadB) {
      if (text_.empty()) {
        Sound(2);
        return -1;
      }
      Erase();
    }
    return 0;
  }

  // ------------------------------------------------------------ drawing
  static float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
  }
  void Ease(float& v, float target, float speed) {
    v = v < -0.5f ? target : v + (target - v) * (1.0f - std::exp(-dt_ * speed));
  }
  static ImU32 White(float a) { return IM_COL32(255, 255, 255, int(std::clamp(a, 0.0f, 1.0f) * 255)); }
  static ImU32 Black(float a) { return IM_COL32(0, 0, 0, int(std::clamp(a, 0.0f, 1.0f) * 255)); }
  static ImU32 Alpha(ImU32 c, float a) {
    const int base = int((c >> IM_COL32_A_SHIFT) & 0xFF);
    return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(int(base * a), 0, 255)) << IM_COL32_A_SHIFT);
  }
  ImVec2 P(float x, float y) const { return ImVec2(ox_ + x * s_, oy_ + y * s_); }
  float TextW(ImFont* f, float size, const std::string& t) const {
    return (f ? f : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  }
  void Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align = 0) {
    if (!f) f = ImGui::GetFont();
    dl_->AddText(f, size * s_, P(x - TextW(f, size, t) * align, y), col, t.c_str());
  }
  void Rect(float x, float y, float w, float h, ImU32 col, float r = 0) {
    dl_->AddRectFilled(P(x, y), P(x + w, y + h), col, r * s_);
  }
  // A controller button: the app's picture, else drawn.
  void Glyph(uint32_t button, float cx, float cy, ImU32 col) {
    if (auto it = st_->buttons.find(button); it != st_->buttons.end() && it->second) {
      const float h = 24;
      dl_->AddImage(it->second, P(cx - h, cy - h), P(cx + h, cy + h), ImVec2(0, 0), ImVec2(1, 1), col);
      return;
    }
    const char* label = button == kPadA ? "X" : button == kPadB ? "O" : button == kPadX ? "[]" : button == kPadY ? "/\\"
                        : button == kPadL1 ? "L1" : button == kPadStart ? "=" : "+";
    dl_->AddCircle(P(cx, cy), 15 * s_, col, 32, 1.6f * s_);
    Text(st_->bold, 15, cx, cy - 9, col, label, 0.5f);
  }
  float Hint(float right, float y, uint32_t button, const char* label) {
    const float tw = TextW(st_->bold, 20, label);
    const float x = right - tw;
    Text(st_->bold, 20, x, y - 13, White(0.78f), label);
    Glyph(button, x - 12 - 15, y, White(0.9f));
    return x - 12 - 30 - 36;
  }

  void Render() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    s_ = std::min(size.x / kW, size.y / kH);
    ox_ = (size.x - kW * s_) * 0.5f;
    oy_ = (size.y - kH * s_) * 0.5f;
    dl_ = ImGui::GetForegroundDrawList();
    const float a = Smooth(in_);
    dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.62f * a));
    const int v0 = dl_->VtxBuffer.Size;

    // the panel, with the accent at its left
    const float px = (kW - kPanelW) * 0.5f, py = PanelY();
    const float ph = GridY() - py + 5 * (kKeyH + kGap) + 96;
    Rect(px, py, kPanelW, ph, IM_COL32(24, 25, 29, 255), 16);
    dl_->AddRectFilled(P(px, py), P(px + 6, py + ph), st_->accent, 16 * s_, ImDrawFlags_RoundCornersLeft);
    float y = py + 40;
    Text(st_->title, 40, px + 64, y, IM_COL32(244, 245, 247, 255), title_);
    y += 70;
    if (!description_.empty()) {
      Text(st_->body, 21, px + 64, y, White(0.6f), description_);
      y += 40;
    }

    // the text so far, with a caret and the room left
    const float fx = px + 64, fw = kPanelW - 128, fh = 76;
    Rect(fx, y, fw, fh, White(0.07f), 12);
    dl_->AddRect(P(fx, y), P(fx + fw, y + fh), Alpha(st_->accent, 0.55f + 0.25f * std::sin(time_ * 3)), 12 * s_, 0,
                 2 * s_);
    const float tx = fx + 28, ty = y + 18;
    const float tw = TextW(st_->bold, 34, text_);
    // (the newest letter rises in)
    if (!text_.empty() && value_in_ < 0.999f) {
      const std::string head = text_.substr(0, text_.size() - 1), last = text_.substr(text_.size() - 1);
      Text(st_->bold, 34, tx, ty, IM_COL32(244, 245, 247, 255), head);
      const int l0 = dl_->VtxBuffer.Size;
      Text(st_->bold, 34, tx + TextW(st_->bold, 34, head), ty + (1 - Smooth(value_in_)) * 10,
           IM_COL32(244, 245, 247, 255), last);
      for (int i = l0; i < dl_->VtxBuffer.Size; ++i) dl_->VtxBuffer[i].col = Alpha(dl_->VtxBuffer[i].col, Smooth(value_in_));
    } else {
      Text(st_->bold, 34, tx, ty, IM_COL32(244, 245, 247, 255), text_);
    }
    if (!Full() && (static_cast<int>(time_ * 2) & 1) == 0) Rect(tx + tw + 3, ty + 4, 3, 36, st_->accent, 1.5f);
    char room[32];
    std::snprintf(room, sizeof(room), "%zu / %zu", text_.size(), limit_);
    Text(st_->body, 20, fx + fw - 24, y + 26, Full() ? Alpha(st_->accent, 1) : White(0.45f), room, 1);

    // the keys; the focus glides over them
    float kx, ky, kw;
    KeyBox(row_, col_, &kx, &ky, &kw);
    Ease(fx_, kx, 22), Ease(fy_, ky, 22), Ease(fw_, kw, 22);
    for (int r = 0; r < 5; ++r) {
      const int n = r < 4 ? kCols : 4;
      for (int i = 0; i < n; ++i) {
        float x, yy, w;
        KeyBox(r, r < 4 ? i : SpecialCol(i), &x, &yy, &w);
        const bool lit = r == 4 && ((i == 0 && upper_) || i == 3);
        const ImU32 key_col = lit && i == 3 ? Alpha(st_->accent, 0.85f) : White(lit ? 0.16f : 0.07f);
        Rect(x, yy, w, kKeyH, key_col, 10);
        if (r == 4 && i == 0 && caps_) Rect(x + 14, yy + kKeyH - 12, w - 28, 4, st_->accent, 2);  // (caps lock)
      }
    }
    // the focus: white, giving a little when pressed
    const float push = press_ * 5;
    Rect(fx_ + push, fy_ + push, fw_ - push * 2, kKeyH - push * 2, IM_COL32(244, 245, 247, 255), 10);
    for (int r = 0; r < 5; ++r) {
      const int n = r < 4 ? kCols : 4;
      for (int i = 0; i < n; ++i) {
        float x, yy, w;
        KeyBox(r, r < 4 ? i : SpecialCol(i), &x, &yy, &w);
        const float over = std::clamp(1 - std::max(std::fabs(fx_ - x) / (w * 0.7f), std::fabs(fy_ - yy) / (kKeyH * 0.7f)), 0.0f, 1.0f);
        const ImU32 ink = over > 0.5f ? IM_COL32(12, 13, 16, 255) : White(0.9f);
        if (r < 4) {
          Text(st_->bold, 30, x + w * 0.5f, yy + 17, ink, std::string(1, kRows[upper_ ? 0 : 1][r][i]), 0.5f);
        } else {
          static const char* kNames[4] = {"SHIFT", "SPACE", "DELETE", "DONE"};
          Text(st_->bold, 20, x + w * 0.5f, yy + 23, ink, kNames[i], 0.5f);
        }
      }
    }

    // what the buttons do
    const float hy = GridY() + 5 * (kKeyH + kGap) + 40;
    float r = px + kPanelW - 48;
    r = Hint(r, hy, kPadStart, "Done");
    r = Hint(r, hy, kPadB, text_.empty() ? "Cancel" : "Back");
    r = Hint(r, hy, kPadL1, caps_ ? "Caps Lock" : "Shift");
    r = Hint(r, hy, kPadY, "Space");
    r = Hint(r, hy, kPadX, "Delete");
    Hint(r, hy, kPadA, "Type");

    for (int i = v0; i < dl_->VtxBuffer.Size; ++i) dl_->VtxBuffer[i].col = Alpha(dl_->VtxBuffer[i].col, a);
  }

  const rex::ui::PadDialogStyle* st_ = nullptr;
  std::string title_, description_, text_;
  size_t limit_;
  int row_ = 1, col_ = 0;
  bool upper_ = true, caps_ = false;
  int closing_ = 0;
  float in_ = 0, press_ = 0, value_in_ = 1, time_ = 0, dt_ = 0;
  float fx_ = -1, fy_ = -1, fw_ = -1;
  float s_ = 1, ox_ = 0, oy_ = 0;
  ImDrawList* dl_ = nullptr;
};
}  // namespace

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title, std::string description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::Text("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      // [ps5 port] d-pad / stick picks a button, Cross presses it
      const uint16_t pad = pad_.Poll();
      const size_t count = buttons_.size();
      if (count) {
        if (focus_ >= count) focus_ = 0;
        if (pad & (kPadLeft | kPadUp)) focus_ = (focus_ + count - 1) % count;
        if (pad & (kPadRight | kPadDown)) focus_ = (focus_ + 1) % count;
      }
      for (size_t i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool focused = i == focus_;
        const bool clicked = PadButton(buttons_[i].c_str(), focused);
        ImGui::PopID();
        if (clicked || (focused && (pad & kPadA))) {
          chosen_button_ = static_cast<uint32_t>(i);
          ImGui::CloseCurrentPopup();
          Close();
          break;
        }
        ImGui::SameLine();
      }
      ImGui::NewLine();
      ImGui::TextDisabled("D-pad: choose    Cross: OK");
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
  size_t focus_ = default_button_;
  DialogPad pad_;
};

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
u32 XamShowMessageBoxUI_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                              u32 button_count, mapped_u32 button_ptrs, u32 active_button,
                              u32 flags, mapped_u32 result_ptr, mapped_void overlapped) {
  REXKRNL_DEBUG(
      "XamShowMessageBoxUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      uint32_t(user_index), title_ptr.guest_address(), text_ptr.guest_address(),
      uint32_t(button_count), button_ptrs.guest_address(), uint32_t(active_button), uint32_t(flags),
      result_ptr.guest_address(), overlapped.guest_address());
  std::string title;
  if (title_ptr) {
    title = rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(title_ptr.guest_address())));
  } else {
    title = "";  // TODO(gibbed): default title based on flags?
  }
  std::string text_str = text_ptr
                             ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                   REX_KERNEL_MEMORY()->TranslateVirtual(text_ptr.guest_address())))
                             : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(button_ptr));
    buttons.push_back(rex::string::to_utf8(button));
  }

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    // Auto-pick the focused button.
    auto run = [result_ptr, active_button]() -> X_RESULT {
      *result_ptr = static_cast<uint32_t>(active_button);
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    // TODO(benvanik): setup icon states.
    switch (flags & 0xF) {
      case 0:
        // config.pszMainIcon = nullptr;
        break;
      case 1:
        // config.pszMainIcon = TD_ERROR_ICON;
        break;
      case 2:
        // config.pszMainIcon = TD_WARNING_ICON;
        break;
      case 3:
        // config.pszMainIcon = TD_INFORMATION_ICON;
        break;
    }
    auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
      *result_ptr = dialog->chosen_button();
      return X_ERROR_SUCCESS;
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
    if (imgui_drawer) {
      result = xeXamDispatchDialog<MessageBoxDialog>(
          new MessageBoxDialog(imgui_drawer, title, text_str, buttons, active_button), close,
          overlapped.guest_address());
    } else {
      // Fallback to headless if no drawer available
      auto run = [result_ptr, active_button]() -> X_RESULT {
        *result_ptr = static_cast<uint32_t>(active_button);
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

class KeyboardInputDialog : public XamDialog {
 public:
  KeyboardInputDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title,
                      std::string description, std::string default_text, size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    rex::string::copy_truncating(text_buffer_.data(), default_text_, text_buffer_.size());
  }

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override {
#if REX_PLATFORM_PS5
    // [ps5 port] in the app's style, when it gave one (rex/ui/pad_dialog_style.h)
    if (const auto* style = rex::ui::GetPadDialogStyle()) {
      if (!styled_) styled_ = std::make_unique<StyledPadKeyboard>(title_, description_, text_, Limit());
      const int result = styled_->Draw(*style, pad_.Poll(), io.DeltaTime);
      if (result) {
        cancelled_ = result < 0;
        text_ = cancelled_ ? std::string() : styled_->text();
        Close();
      }
      return;
    }
#endif
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::TextWrapped("%s", description_.c_str());
      }
#if REX_PLATFORM_PS5
      (void)first_draw;
      DrawPadKeyboard();
      ImGui::EndPopup();
      return;
#endif
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        text_ = std::string(text_buffer_.data(), text_buffer_.size());
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        text_ = "";
        cancelled_ = true;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;

  // [ps5 port] An on-screen keyboard for the controller, in the console's
  // style: d-pad / stick move, Cross types, Square / Circle delete (Circle on
  // an empty name cancels), Triangle adds a space, L1 / R1 switch case,
  // Options finishes.
  static constexpr int kCols = 13;
  static constexpr int kSpecials = 5;  // CAPS, SPACE, DELETE, DONE, CANCEL
  int row_ = 0, col_ = 0;
  bool lower_ = false;
  DialogPad pad_;
  std::unique_ptr<StyledPadKeyboard> styled_;

  size_t Limit() const { return max_length_ > 2 ? max_length_ - 2 : 1; }
  void Type(char c) {
    if (text_.size() < Limit()) text_.push_back(c);
  }
  void Erase() {
    if (!text_.empty()) text_.pop_back();
  }
  void Finish(bool cancel) {
    cancelled_ = cancel;
    if (cancel) text_.clear();
    ImGui::CloseCurrentPopup();
    Close();
  }

  void DrawPadKeyboard() {
    static const char* kRows[2][3] = {{"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789-_."},
                                      {"abcdefghijklm", "nopqrstuvwxyz", "0123456789!?'"}};
    static const char* kSpecial[kSpecials] = {"CAPS", "SPACE", "DELETE", "DONE", "CANCEL"};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float key = ImGui::GetFrameHeight() * 1.5f;
    const float grid_w = kCols * key + (kCols - 1) * style.ItemSpacing.x;
    const float special_w = (grid_w - (kSpecials - 1) * style.ItemSpacing.x) / kSpecials;

    // the name so far, with a caret
    {
      std::string shown = text_;
      const bool caret = (static_cast<int>(ImGui::GetTime() * 2.0) & 1) == 0;
      if (text_.size() < Limit()) shown += caret ? "_" : " ";
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
      ImGui::BeginChild("##name", ImVec2(grid_w, ImGui::GetFrameHeight() + 4), false);
      ImGui::SetCursorPos(ImVec2(style.FramePadding.x, 2 + style.FramePadding.y));
      ImGui::TextUnformatted(shown.c_str());
      ImGui::EndChild();
      ImGui::PopStyleColor();
      ImGui::TextDisabled("%zu / %zu", text_.size(), Limit());
    }

    const uint16_t pad = pad_.Poll();
    if (pad & kPadLeft) col_ = (col_ + (row_ < 3 ? kCols : kSpecials) - 1) % (row_ < 3 ? kCols : kSpecials);
    if (pad & kPadRight) col_ = (col_ + 1) % (row_ < 3 ? kCols : kSpecials);
    if (pad & kPadDown) {
      if (row_ == 2) col_ = std::min(kSpecials - 1, col_ * kSpecials / kCols);
      else if (row_ == 3) col_ = std::min(kCols - 1, col_ * kCols / kSpecials + 1);
      row_ = (row_ + 1) % 4;
    }
    if (pad & kPadUp) {
      if (row_ == 0) col_ = std::min(kSpecials - 1, col_ * kSpecials / kCols);
      else if (row_ == 3) col_ = std::min(kCols - 1, col_ * kCols / kSpecials + 1);
      row_ = (row_ + 3) % 4;
    }
    if (pad & (kPadL1 | kPadR1)) lower_ = !lower_;

    int pressed_row = -1, pressed_col = -1;
    for (int r = 0; r < 3; ++r) {
      const char* keys = kRows[lower_ ? 1 : 0][r];
      for (int c = 0; c < kCols; ++c) {
        char label[2] = {keys[c], 0};
        ImGui::PushID(r * 32 + c);
        if (PadButton(label, r == row_ && c == col_, ImVec2(key, key))) {
          pressed_row = r;
          pressed_col = c;
        }
        ImGui::PopID();
        if (c + 1 < kCols) ImGui::SameLine();
      }
    }
    for (int c = 0; c < kSpecials; ++c) {
      const char* label = (c == 0 && lower_) ? "caps" : kSpecial[c];
      if (PadButton(label, row_ == 3 && c == col_, ImVec2(special_w, key))) {
        pressed_row = 3;
        pressed_col = c;
      }
      if (c + 1 < kSpecials) ImGui::SameLine();
    }
    ImGui::TextDisabled(
        "Cross: type   Square: delete   Triangle: space   L1: caps   Options: done   Circle: back");

    if (pad & kPadA) {
      pressed_row = row_;
      pressed_col = col_;
    }
    if (pressed_row >= 0) {
      if (pressed_row < 3) {
        Type(kRows[lower_ ? 1 : 0][pressed_row][pressed_col]);
      } else {
        switch (pressed_col) {
          case 0: lower_ = !lower_; break;
          case 1: Type(' '); break;
          case 2: Erase(); break;
          case 3: Finish(false); return;
          case 4: Finish(true); return;
        }
      }
    }
    if (pad & kPadX) Erase();
    if (pad & kPadY) Type(' ');
    if (pad & kPadB) {
      if (text_.empty()) {
        Finish(true);
        return;
      }
      Erase();
    }
    if (pad & kPadStart) Finish(false);
  }
};

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, mapped_wstring default_text,
                            mapped_wstring title, mapped_wstring description, mapped_wstring buffer,
                            u32 buffer_length, mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(flags), default_text.guest_address(),
                title.guest_address(), description.guest_address(), buffer.guest_address(),
                uint32_t(buffer_length), overlapped.guest_address());
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_true(overlapped.guest_address() != 0);

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
      // Redirect default_text back into the buffer.
      if (!default_text) {
        std::memset(buffer, 0, buffer_size);
      } else {
        rex::string::copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
      }
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    auto close = [buffer, buffer_length](KeyboardInputDialog* dialog, uint32_t& extended_error,
                                         uint32_t& length) -> X_RESULT {
      if (dialog->cancelled()) {
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return X_ERROR_SUCCESS;
      } else {
        // Zero the output buffer.
        auto text = rex::string::to_utf16(dialog->text());
        rex::string::copy_and_swap_truncating(buffer, text, buffer_length);
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        return X_ERROR_SUCCESS;
      }
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    // Read and convert title/description/default_text from guest memory as utf16 to utf8 strings
    std::string title_str = title
                                ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                      REX_KERNEL_MEMORY()->TranslateVirtual(title.guest_address())))
                                : "";
    std::string desc_str =
        description ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                          REX_KERNEL_MEMORY()->TranslateVirtual(description.guest_address())))
                    : "";
    std::string def_text_str =
        default_text ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                           REX_KERNEL_MEMORY()->TranslateVirtual(default_text.guest_address())))
                     : "";

    if (imgui_drawer) {
      uint32_t buffer_length_safe = buffer_length + 1;  // +1 for null terminator, just in case
      result = xeXamDispatchDialogEx<KeyboardInputDialog>(
          new KeyboardInputDialog(imgui_drawer, title_str, desc_str, def_text_str,
                                  buffer_length_safe),
          close, overlapped.guest_address());
    } else {
      // Fallback to headless
      auto run = [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
        if (!default_text) {
          std::memset(buffer, 0, buffer_size);
        } else {
          rex::string::copy_and_swap_truncating(buffer, default_text.value(), buffer_length);
        }
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, mapped_u32 device_id_ptr,
                                  mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(content_type), uint32_t(content_flags),
                uint64_t(total_requested), device_id_ptr.guest_address(),
                overlapped.guest_address());
  return xeXamDispatchHeadless(
      [device_id_ptr]() -> X_RESULT {
        // NOTE: 0x00000001 is our dummy device ID from xam_content.cc
        *device_id_ptr = 0x00000001;
        return X_ERROR_SUCCESS;
      },
      overlapped.guest_address());
}

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
  REXKRNL_ERROR("XamShowDirtyDiscErrorUI called! user_index={}", uint32_t(user_index));
  REXKRNL_ERROR("This indicates a disc/file read error - check that all game files exist");

  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (imgui_drawer) {
    xeXamDispatchDialog<MessageBoxDialog>(
        new MessageBoxDialog(imgui_drawer, "Disc Read Error",
                             "There's been an issue reading content from the game disc.\nThis is "
                             "likely caused by bad or unimplemented file IO calls.",
                             {"OK"}, 0),
        [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  } else {
    // No UI available - log prominently and pause to let user see the error
    REXKRNL_ERROR("===========================================");
    REXKRNL_ERROR("FATAL: Disc Read Error (no UI to display)");
    REXKRNL_ERROR("Check that all game content files are present");
    REXKRNL_ERROR("Missing files or bad mounts cause this error");
    REXKRNL_ERROR("===========================================");
  }
  // This is death, and should never return.
  // TODO(benvanik): cleaner exit.
  exit(1);
}

u32 XamShowPartyUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamShowCommunitySessionsUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

// [svr07] XamShowMessageBoxUIEx takes XamShowMessageBoxUI's arguments
// (rexglue-sdk-yukes). Only with xam_message_box_ex: SmackDown vs. Raw 2011
// also imports it and is tuned for the stub.
REXCVAR_DEFINE_BOOL(xam_message_box_ex, false, "Kernel",
                    "XamShowMessageBoxUIEx shows the message box (SmackDown vs. Raw 2007)");

u32 XamShowMessageBoxUIEx_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                                u32 button_count, mapped_u32 button_ptrs, u32 active_button, u32 flags,
                                mapped_u32 result_ptr, mapped_void overlapped) {
  if (REXCVAR_GET(xam_message_box_ex))
    return XamShowMessageBoxUI_entry(user_index, title_ptr, text_ptr, button_count, button_ptrs,
                                     active_button, flags, result_ptr, overlapped);
  static bool warned = false;
  if (!warned) {
    REXKRNL_WARN("[STUB] XamShowMessageBoxUIEx - not implemented");
    warned = true;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamIsUIActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT(__imp__XamShowMessageBoxUI, rex::kernel::xam::XamShowMessageBoxUI_entry)
REX_EXPORT(__imp__XamShowGamerCardUIForXUID, rex::kernel::xam::XamShowGamerCardUIForXUID_entry)
REX_EXPORT(__imp__XamShowGamerCardUI, rex::kernel::xam::XamShowGamerCardUI_entry)
REX_EXPORT(__imp__XamShowKeyboardUI, rex::kernel::xam::XamShowKeyboardUI_entry)
REX_EXPORT(__imp__XamShowDeviceSelectorUI, rex::kernel::xam::XamShowDeviceSelectorUI_entry)
REX_EXPORT(__imp__XamShowDirtyDiscErrorUI, rex::kernel::xam::XamShowDirtyDiscErrorUI_entry)
REX_EXPORT(__imp__XamShowPartyUI, rex::kernel::xam::XamShowPartyUI_entry)
REX_EXPORT(__imp__XamShowCommunitySessionsUI, rex::kernel::xam::XamShowCommunitySessionsUI_entry)
REX_EXPORT(__imp__XamShowMessageBoxUIEx, rex::kernel::xam::XamShowMessageBoxUIEx_entry)

REX_EXPORT_STUB(__imp__XamIsGuideDisabled);
REX_EXPORT_STUB(__imp__XamIsMessageBoxActive);
REX_EXPORT_STUB(__imp__XamIsNuiUIActive);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByTitle);
REX_EXPORT_STUB(__imp__XamIsSysUiInvokedByXenonButton);
REX_EXPORT_STUB(__imp__XamIsUIThread);
REX_EXPORT_STUB(__imp__XamNavigate);
REX_EXPORT_STUB(__imp__XamNavigateBack);
REX_EXPORT_STUB(__imp__XamOverrideHudOpenType);
REX_EXPORT_STUB(__imp__XamShowAchievementDetailsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowAchievementsUIEx);
REX_EXPORT_STUB(__imp__XamShowAndWaitForMessageBoxEx);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardGamesUI);
REX_EXPORT_STUB(__imp__XamShowAvatarAwardsUI);
REX_EXPORT_STUB(__imp__XamShowAvatarMiniCreatorUI);
REX_EXPORT_STUB(__imp__XamShowBadDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowBeaconsUI);
REX_EXPORT_STUB(__imp__XamShowBrandedKeyboardUI);
REX_EXPORT_STUB(__imp__XamShowChangeGamerTileUI);
REX_EXPORT_STUB(__imp__XamShowComplaintUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUI);
REX_EXPORT_STUB(__imp__XamShowCreateProfileUIEx);
REX_EXPORT_STUB(__imp__XamShowCsvTransitionUI);
REX_EXPORT_STUB(__imp__XamShowCustomMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowCustomPlayerListUI);
REX_EXPORT_STUB(__imp__XamShowDirectAcquireUI);
REX_EXPORT_STUB(__imp__XamShowEditProfileUI);
REX_EXPORT_STUB(__imp__XamShowFirstRunWelcomeUI);
REX_EXPORT_STUB(__imp__XamShowFitnessBodyProfileUI);
REX_EXPORT_STUB(__imp__XamShowFitnessClearUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutPrivacyUI);
REX_EXPORT_STUB(__imp__XamShowFitnessWarnAboutTimeUI);
REX_EXPORT_STUB(__imp__XamShowFofUI);
REX_EXPORT_STUB(__imp__XamShowForcedNameChangeUI);
REX_EXPORT_STUB(__imp__XamShowFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUI);
REX_EXPORT_STUB(__imp__XamShowFriendsUIp);
REX_EXPORT_STUB(__imp__XamShowGameInviteUI);
REX_EXPORT_STUB(__imp__XamShowGameVoiceChannelUI);
REX_EXPORT_STUB(__imp__XamShowGamerCardUIForXUIDp);
REX_EXPORT_STUB(__imp__XamShowGamesUI);
REX_EXPORT_STUB(__imp__XamShowGenericOnlineAppUI);
REX_EXPORT_STUB(__imp__XamShowGoldUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowGraduateUserUI);
REX_EXPORT_STUB(__imp__XamShowGuideUI);
REX_EXPORT_STUB(__imp__XamShowJoinPartyUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionByIdInProgressUI);
REX_EXPORT_STUB(__imp__XamShowJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowKeyboardUIMessenger);
REX_EXPORT_STUB(__imp__XamShowLiveSignupUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUI);
REX_EXPORT_STUB(__imp__XamShowLiveUpsellUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceGetOrderReceipts);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUI);
REX_EXPORT_STUB(__imp__XamShowMarketplacePurchaseOrderUIEx);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowMarketplaceUIEx);
REX_EXPORT_STUB(__imp__XamShowMessageBox);
REX_EXPORT_STUB(__imp__XamShowMessageComposeUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUI);
REX_EXPORT_STUB(__imp__XamShowMessagesUIEx);
REX_EXPORT_STUB(__imp__XamShowMessengerUI);
REX_EXPORT_STUB(__imp__XamShowMultiplayerUpgradeUI);
REX_EXPORT_STUB(__imp__XamShowNetworkStorageSyncUI);
REX_EXPORT_STUB(__imp__XamShowNuiAchievementsUI);
REX_EXPORT_STUB(__imp__XamShowNuiCommunitySessionsUI);
REX_EXPORT_STUB(__imp__XamShowNuiControllerRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiDeviceSelectorUI);
REX_EXPORT_STUB(__imp__XamShowNuiDirtyDiscErrorUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendRequestUI);
REX_EXPORT_STUB(__imp__XamShowNuiFriendsUI);
REX_EXPORT_STUB(__imp__XamShowNuiGameInviteUI);
REX_EXPORT_STUB(__imp__XamShowNuiGamerCardUIForXUID);
REX_EXPORT_STUB(__imp__XamShowNuiGamesUI);
REX_EXPORT_STUB(__imp__XamShowNuiGuideUI);
REX_EXPORT_STUB(__imp__XamShowNuiHardwareRequiredUI);
REX_EXPORT_STUB(__imp__XamShowNuiJoinSessionInProgressUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceDownloadItemsUI);
REX_EXPORT_STUB(__imp__XamShowNuiMarketplaceUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessageBoxUI);
REX_EXPORT_STUB(__imp__XamShowNuiMessagesUI);
REX_EXPORT_STUB(__imp__XamShowNuiPartyUI);
REX_EXPORT_STUB(__imp__XamShowNuiSigninUI);
REX_EXPORT_STUB(__imp__XamShowNuiVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUI);
REX_EXPORT_STUB(__imp__XamShowOptionalMediaUpdateRequiredUIEx);
REX_EXPORT_STUB(__imp__XamShowOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPamUI);
REX_EXPORT_STUB(__imp__XamShowPartyInviteUI);
REX_EXPORT_STUB(__imp__XamShowPartyJoinInProgressUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUI);
REX_EXPORT_STUB(__imp__XamShowPasscodeVerifyUIEx);
REX_EXPORT_STUB(__imp__XamShowPaymentOptionsUI);
REX_EXPORT_STUB(__imp__XamShowPersonalizationUI);
REX_EXPORT_STUB(__imp__XamShowPlayerReviewUI);
REX_EXPORT_STUB(__imp__XamShowPlayersUI);
REX_EXPORT_STUB(__imp__XamShowPrivateChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUI);
REX_EXPORT_STUB(__imp__XamShowQuickChatUIp);
REX_EXPORT_STUB(__imp__XamShowQuickLaunchUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUI);
REX_EXPORT_STUB(__imp__XamShowRecentMessageUIEx);
REX_EXPORT_STUB(__imp__XamShowReputationUI);
REX_EXPORT_STUB(__imp__XamShowSigninUIEx);
REX_EXPORT_STUB(__imp__XamShowSigninUIp);
REX_EXPORT_STUB(__imp__XamShowSignupCreditCardUI);
REX_EXPORT_STUB(__imp__XamShowSocialPostUI);
REX_EXPORT_STUB(__imp__XamShowStorePickerUI);
REX_EXPORT_STUB(__imp__XamShowTFAUI);
REX_EXPORT_STUB(__imp__XamShowTermsOfUseUI);
REX_EXPORT_STUB(__imp__XamShowUpdaterUI);
REX_EXPORT_STUB(__imp__XamShowVideoChatInviteUI);
REX_EXPORT_STUB(__imp__XamShowVideoRichPresenceUI);
REX_EXPORT_STUB(__imp__XamShowVoiceMailUI);
REX_EXPORT_STUB(__imp__XamShowVoiceSettingsUI);
REX_EXPORT_STUB(__imp__XamShowWhatsOnUI);
REX_EXPORT_STUB(__imp__XamShowWordRegisterUI);
REX_EXPORT_STUB(__imp__XamSysUiDisableAutoClose);

namespace rex::ui {
namespace {
PadDialogStyle g_pad_dialog_style;
bool g_pad_dialog_style_set = false;
}  // namespace
void SetPadDialogStyle(PadDialogStyle style) {
  g_pad_dialog_style = std::move(style);
  g_pad_dialog_style_set = g_pad_dialog_style.title && g_pad_dialog_style.body && g_pad_dialog_style.bold;
}
const PadDialogStyle* GetPadDialogStyle() { return g_pad_dialog_style_set ? &g_pad_dialog_style : nullptr; }
}  // namespace rex::ui
