// WWE SmackDown vs. Raw 2007 - PS5 port: the app (HollywoodAkeem's
// SVR07-Recomp on the ReXGlue runtime's PS5 platform layer).
#pragma once

#include <functional>
#include <optional>

#include <rex/rex_app.h>

class Svr07App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr07App>(new Svr07App(ctx, "svr07", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostLoadXexImage() override;
  // [ps5 port] The hub (src/hub) first; START GAME continues the start.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
};
