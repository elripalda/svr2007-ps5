// WWE SmackDown vs. Raw 2007 - PS5 port: where things are, and the settings
// the game needs from the runtime.
#include "generated/default/svr07_init.h"
#include "svr07_app.h"

#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "crash_report.h"

namespace {

// The settings a first start writes (svr07.toml beside the app; edit it over FTP).
constexpr char kDefaultConfig[] =
    "# WWE SmackDown vs. Raw 2007 - settings\n"
    "gpu_plugin = \"xenos\"\n"
    "# 1 = the Xbox 360's 720p, 2 = 1440p, 3 = 4K\n"
    "resolution_scale = 2\n"
    "anisotropic_override = 3\n"
    "vsync = true\n";

}  // namespace

void Svr07App::OnConfigurePaths(rex::PathConfig& paths) {
  // What SmackDown vs. Raw 2007 needs from the kernel (rexglue-sdk-yukes):
  // its save system, and its message boxes.
  rex::cvar::SetFlagByName("xam_content_keep_existing", "true");
  rex::cvar::SetFlagByName("xam_message_box_ex", "true");

  const std::filesystem::path app = rex::filesystem::GetExecutableFolder();
  // The disc files: <app>/Assets (as SVR07-Recomp keeps them on a PC), else
  // <app>/assets, else beside the app.
  std::error_code ec;
  if (paths.game_data_root.empty()) {
    if (std::filesystem::exists(app / "Assets" / "default.xex", ec)) paths.game_data_root = app / "Assets";
    else if (std::filesystem::exists(app / "assets" / "default.xex", ec)) paths.game_data_root = app / "assets";
    else paths.game_data_root = app;
  }
  // A title update, if there is one (<disc files>/1_update).
  if (std::filesystem::is_directory(paths.game_data_root / "1_update", ec))
    paths.update_data_root = paths.game_data_root / "1_update";
  else
    paths.update_data_root.clear();
  paths.user_data_root = app / "UserData";
  paths.cache_root = app / "UserData" / "cache";
  svr07::InstallCrashReporter(app / "UserData" / "crashes");
  if (!std::filesystem::exists(paths.config_path, ec)) std::ofstream(paths.config_path) << kDefaultConfig;
}

void Svr07App::OnPostLoadXexImage() {
  REXLOG_INFO("==== SmackDown vs. Raw 2007 PS5 port (test build) ====");
  REXLOG_INFO("game folder: {}", game_data_root().string());
}
