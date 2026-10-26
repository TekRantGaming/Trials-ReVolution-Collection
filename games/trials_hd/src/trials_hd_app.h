// trials_hd - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#if defined(TH_DEV_TOOLS)
namespace th {
void StartDevTools(rex::Runtime* runtime, rex::ui::WindowedAppContext& app_context, uint32_t image_base,
                   uint32_t image_size);  // dev/dev_tools.cpp
}  // namespace th
#endif

class TrialsHdApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TrialsHdApp>(new TrialsHdApp(ctx, "trials_hd",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    if (!config.audio_factory)
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    // Arcade games run as their trial unless the license says the game was bought. Players install from their own
    // copy, so the full game is always unlocked.
    rex::cvar::SetFlagByName("license_mask", "1");
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    // GPU defaults (the GPU plugin's settings exist only from here on; a value in the settings file still wins).
    // From the reference port, snufflyyy/trials-hd-recompiled (MIT), and measured here: reading every memory export
    // back to the CPU held gameplay at 11 to 12 FPS, without it the game holds 60; the CPU written pages stay valid
    // between frames because writes are watched; the game measures exposure from small resolves on the CPU.
    auto port_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    };
    port_default("readback_memexport", "false");
    port_default("clear_memory_page_state", "false");
    port_default("readback_resolve", "fast");
#if defined(TH_DEV_TOOLS)
    th::StartDevTools(runtime(), app_context(), PPCImageConfig.image_base, PPCImageConfig.image_size);
#endif
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
