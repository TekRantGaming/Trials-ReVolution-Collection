// The collection's defaults (they change only the default, so the settings file the launcher writes and the command
// line still win). GPU plugin settings exist only once the plugin is loaded, so this runs in OnPostSetup.
//   swap_post_effect = fxaa              anti aliasing on by default (it takes effect from render_quality.cpp,
//                                        set before the GPU starts; this one only keeps the settings file in step)
//   readback_memexport = false           reading every memory export back to the CPU held races at 11 to 12 FPS;
//                                        without it the game holds 60 (the same as Trials HD: from
//                                        snufflyyy/trials-hd-recompiled, MIT, and measured here 26 Oct 2026)
//   clear_memory_page_state = false      the CPU written pages stay valid between frames (writes are watched)
//   readback_resolve = fast              the game reads small resolves on the CPU: exposure, and the page request
//                                        image that picks the texture pages to stream (without it at 2x the
//                                        surfaces stayed flat and untextured)
//   render_target_path_vulkan = fbo      Linux: host render targets (faster on handheld graphics)
//   async_shader_compilation_no_pixel_shader = true
//                                        depth only and memory export pipelines are built in the background too
//                                        (on a new shader cache they froze the game up to 250 ms each)
//   readback_resolve_max_kb = 256        only small resolves are read on the CPU; the late copy of the shadow maps
//                                        replaced newer ones
//   readback_resolve_half_pixel_offset = true
//                                        above 720p the CPU's copy of a resolve takes the centre host pixel of each
//                                        guest pixel, which the half pixel offset makes the right one (in Trials
//                                        Evolution the top left one darkened the ground near the camera)
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
  SetCvarDefault("async_shader_compilation_no_pixel_shader", "true");
  SetCvarDefault("readback_resolve_max_kb", "256");
  SetCvarDefault("readback_resolve_half_pixel_offset", "true");
  // Above 720p every host pixel of a guest pixel gets that pixel's position: the color grading table is drawn from
  // the pixel position, and positions between guest pixels made the whole picture pink (SDK patch 0013).
  SetCvarDefault("ps_position_guest_pixels", "true");
  // The pass that draws the coarse levels of the virtual texture page table (the same code as Trials Evolution's) is
  // drawn as the console draws it: above 720p it moved those levels by a host pixel (SDK patch 0020).
  SetCvarDefault("ps_guest_pixel_center_shaders", "95BB846480BFCC1C");
#if defined(__linux__)
  SetCvarDefault("render_target_path_vulkan", "fbo");
#endif
}

}  // namespace trials
