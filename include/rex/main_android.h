#pragma once
/**
 * [svr2011] Android process setup (after Xenia's xenia/base/main_android.h).
 */

#include <cstdint>

namespace rex {

// The device's API level (android_get_device_api_level).
int32_t GetAndroidApiLevel();

// Loads the functions newer API levels add (memory, threading, filesystem).
// Once, from the main thread, before the runtime starts.
void InitializeAndroid();
void ShutdownAndroid();

}  // namespace rex
