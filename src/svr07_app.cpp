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
#include "hub/ps5_hub.h"
#include "pad_icons.h"

#include <rex/runtime.h>

namespace {

std::filesystem::path g_config_path;

// The settings a first start writes (svr07.toml beside the app; edit it over FTP).
constexpr char kConfigVersion[] = "(settings v2)";
constexpr char kDefaultConfig[] =
    "# WWE SmackDown vs. Raw 2007 - settings (settings v2)\n"
    "gpu_plugin = \"xenos\"\n"
    "input_backend = \"xinput\"\n"
    "# 1 = the Xbox 360's 720p, 2 = 1440p, 3 = 4K\n"
    "resolution_scale = 2\n"
    "anisotropic_override = 3\n"
    "vsync = true\n"
    "# (the PS5's GPU driver: one plain buffer for the Xbox 360's memory, no sparse binding)\n"
    "vulkan_sparse_shared_memory = false\n"
    "\n"
    "# test builds: more detail in the log\n"
    "[log.levels]\n"
    "gpu = \"debug\"\n"
    "fs = \"debug\"\n";

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
  // (a settings file from an earlier test build is replaced)
  bool current = false;
  {
    std::ifstream in(paths.config_path);
    std::string first;
    std::getline(in, first);
    current = first.find(kConfigVersion) != std::string::npos;
  }
  if (!current) std::ofstream(paths.config_path, std::ios::trunc) << kDefaultConfig;
  g_config_path = paths.config_path;
}

#if defined(__PROSPERO__)
std::optional<rex::PathConfig> Svr07App::OnFinalizePaths(const rex::PathConfig& defaults,
                                                         std::function<void(rex::PathConfig)> resume) {
  if (!imgui_drawer()) return defaults;
  const std::filesystem::path app = rex::filesystem::GetExecutableFolder();
  svr07::hub::Paths paths{app, g_config_path, app / "UserData"};
  svr07::hub::Show(imgui_drawer(), immediate_drawer(), paths, [this, defaults, resume] {
    // The hub's changes (svr07.toml) apply to this start.
    rex::cvar::LoadConfig(g_config_path);
    REXLOG_INFO("PS5 hub: START GAME");
    app_context().CallInUIThreadDeferred([defaults, resume] { resume(defaults); });
  });
  return std::nullopt;
}

void Svr07App::OnConfigureFonts(ImFontAtlas* atlas) { svr07::hub::ConfigureFonts(atlas); }
#else
std::optional<rex::PathConfig> Svr07App::OnFinalizePaths(const rex::PathConfig& defaults,
                                                         std::function<void(rex::PathConfig)>) {
  return defaults;
}
void Svr07App::OnConfigureFonts(ImFontAtlas*) {}
#endif

void Svr07App::OnPostLoadXexImage() {
  REXLOG_INFO("==== SmackDown vs. Raw 2007 PS5 port (test build) ====");
  REXLOG_INFO("game folder: {}", game_data_root().string());
  // PlayStation button pictures (before the game opens its archives)
  svr07::InstallPadIcons(runtime()->file_system(), game_data_root(), rex::filesystem::GetExecutableFolder());
}
