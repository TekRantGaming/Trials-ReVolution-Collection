// trials_evolution - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/overlay/achievement_notification.h>
#include <rex/ui/overlay/debug_overlay.h>

namespace rex::ui {
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace trials {
void InstallFpeGuard();                     // fpe_guard.cpp
void ApplyPortDefaults();                   // port_defaults.cpp
rex::ui::FrameStats GuestFrameStats();      // frame_stats.cpp
void ApplyRenderQuality(rex::ui::Window* window);  // render_quality.cpp
std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementToast(
    rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime);  // achievement_toast.cpp
void ExportAchievements(rex::Runtime* runtime, const std::filesystem::path& game_data_root);  // achievements_export.cpp
}  // namespace trials

#if defined(TE_DEV_TOOLS)
namespace th {
void StartDevTools(rex::Runtime* runtime, rex::ui::WindowedAppContext& app_context, uint32_t image_base,
                   uint32_t image_size);  // dev/dev_tools.cpp
}  // namespace th
#endif

class TrialsEvolutionApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TrialsEvolutionApp>(new TrialsEvolutionApp(ctx, "trials_evolution",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(TE_DEV_TOOLS)
    // Developer aid: TH_DEV_VULKAN=1 runs the GPU plugin's Vulkan backend instead of Direct3D 12.
    if (!config.graphics && std::getenv("TH_DEV_VULKAN")) config.graphics = rex::system::LoadGpuPlugin("xenos", "vulkan");
#endif
    if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    if (!config.audio_factory)
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    // Arcade games run as their trial unless the license says the game was bought. Players install from their own
    // copy, so the full game is always unlocked.
    rex::cvar::SetFlagByName("license_mask", "1");
  }

  // After the GPU plugin and the window exist, before the GPU starts: the render scale from the upscaler mode and
  // the 1280 x 720 guest video mode (render_quality.cpp).
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    trials::ApplyRenderQuality(window());
    return defaults;
  }

#if defined(__linux__)
  // On Linux the program sits in the read only AppImage: its settings file (which the launcher writes) and its logs
  // go in the game's own user folder (XDG_DATA_HOME/<game>, where its saves are).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    paths.config_path = paths.user_data_root / (std::string(GetName()) + ".toml");
    std::error_code ec;
    std::filesystem::create_directories(paths.user_data_root / "logs", ec);
    if (rex::cvar::GetFlagByName("log_file").empty())
      rex::cvar::SetFlagByName("log_file", (paths.user_data_root / "logs" / (std::string(GetName()) + ".log")).string());
  }
#endif

  void OnPostSetup() override {
    trials::InstallFpeGuard();  // after the runtime's own signal handlers
    trials::ApplyPortDefaults();
    SetGuestFrameStats(trials::GuestFrameStats);  // the F3 overlay's frame rate
    trials::ExportAchievements(runtime(), game_data_root());
#if defined(TE_DEV_TOOLS)
    th::StartDevTools(runtime(), app_context(), PPCImageConfig.image_base, PPCImageConfig.image_size);
#endif
  }

  // Achievement notifications in the TRG ports' style (achievement_toast.cpp).
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    return trials::CreateAchievementToast(imgui_drawer(), immediate_drawer(), runtime());
  }
};
