/**
 * [ps5 port] How the console's system dialogs look. A console has no mouse or
 * keyboard: its on-screen keyboard (XamShowKeyboardUI) is driven by the
 * controller. An app with a menu of its own (its fonts, its button pictures,
 * its menu sounds) gives them here, and the keyboard is drawn in that style;
 * unset, it is a plain ImGui window.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>

#include <imgui.h>

namespace rex::ui {

struct PadDialogStyle {
  ImFont* title = nullptr;  // headings
  ImFont* body = nullptr;   // text
  ImFont* bold = nullptr;   // keys, labels
  // The controller's button pictures, by Xbox button bit: 0x1000 Cross (A),
  // 0x2000 Circle (B), 0x4000 Square (X), 0x8000 Triangle (Y), 0x0100 L1,
  // 0x0200 R1, 0x0010 Options (Start), 0x000C the d-pad. (Missing: drawn.)
  std::map<uint32_t, ImTextureID> buttons;
  // Sounds: 0 move, 1 select, 2 back, 3 page, 4 on, 5 off, 6 start, 7 error.
  std::function<void(int)> sound;
  ImU32 accent = IM_COL32(226, 36, 46, 255);
};

// Sets the style (from the UI thread, before a dialog opens).
void SetPadDialogStyle(PadDialogStyle style);
// The style, or null when none was set (or it has no fonts).
const PadDialogStyle* GetPadDialogStyle();

}  // namespace rex::ui
