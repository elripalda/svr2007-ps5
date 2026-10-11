// [svr2011] See rex/main_android.h.

#include <rex/main_android.h>

#include <android/api-level.h>

#include <rex/filesystem.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace rex {

int32_t GetAndroidApiLevel() {
  static const int32_t level = android_get_device_api_level();
  return level;
}

void InitializeAndroid() {
  memory::AndroidInitialize();
  thread::AndroidInitialize();
  filesystem::AndroidInitialize();
}

void ShutdownAndroid() {
  filesystem::AndroidShutdown();
  thread::AndroidShutdown();
  memory::AndroidShutdown();
}

}  // namespace rex

// [svr2011] The port reads its files by path (the phone's games folder), not
// through content:// URIs (Xenia's filesystem_android.cpp resolved those with
// the Java ContentResolver), so these are the minimal versions.
namespace rex::filesystem {

void AndroidInitialize() {}
void AndroidShutdown() {}

bool IsAndroidContentUri(const std::string_view source) { return source.starts_with("content://"); }

int OpenAndroidContentFileDescriptor(const std::string_view, const char*) { return -1; }

}  // namespace rex::filesystem
