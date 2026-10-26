// The collection's defaults (they change only the default, so the settings file the launcher writes and the command
// line still win). GPU plugin settings exist only once the plugin is loaded, so this runs in OnPostSetup.
//   swap_post_effect = fxaa              anti aliasing on by default
//   readback_memexport = false           reading every memory export back to the CPU held races at 11 to 12 FPS;
//                                        without it the game holds 60 (from the reference port,
//                                        snufflyyy/trials-hd-recompiled, MIT, and measured here 26 Oct 2026)
//   clear_memory_page_state = false      the CPU written pages stay valid between frames (writes are watched)
//   readback_resolve = fast              the game measures exposure from small resolves on the CPU
//   render_target_path_vulkan = fbo      Linux: host render targets (faster on handheld graphics)
// Fullscreen stays the SDK's default (on).

#include <string_view>

#include <rex/cvar.h>

namespace trials {

void SetCvarDefault(std::string_view name, std::string_view value) {
  for (auto& e : rex::cvar::GetRegistry()) {
    if (e.name != name) continue;
    e.default_value = value;
    if (e.source == rex::cvar::Source::kDefault) e.setter(value);  // config and command line still win
    return;
  }
}

void ApplyPortDefaults() {
  SetCvarDefault("swap_post_effect", "fxaa");
  SetCvarDefault("readback_memexport", "false");
  SetCvarDefault("clear_memory_page_state", "false");
  SetCvarDefault("readback_resolve", "fast");
#if defined(__linux__)
  SetCvarDefault("render_target_path_vulkan", "fbo");
#endif
}

}  // namespace trials
