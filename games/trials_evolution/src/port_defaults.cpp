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
//   memexport_vertex_whole_stream = false
//                                        the particle vertex shaders export into 8 to 16 MB streams that also hold
//                                        menu and level textures; uploading and invalidating the whole stream for
//                                        every particle draw corrupted those textures (only a few KB of particle
//                                        vertex data is written)
//   readback_resolve_max_kb = 256        the CPU reads only small resolves (exposure up to 32 KB, page requests 180 KB,
//                                        occlusion 216 KB); the late copy of the 1 MB shadow maps into guest memory
//                                        replaced newer ones, and shadows vanished (and 30 MB a frame was copied)
//   readback_resolve_half_pixel_offset = true
//                                        above 720p the CPU's copy of a resolve takes the centre host pixel of each
//                                        guest pixel: the half pixel offset moves the picture by a host pixel, and
//                                        the top left one gave the tile lighting a neighbour's depth, so ground near
//                                        the camera got the wrong shadow cascade and went dark (29 Oct 2026)
//   render_target_path_vulkan = fbo      Linux: host render targets (faster on handheld graphics)
//   async_shader_compilation_no_pixel_shader = true
//                                        depth only and memory export pipelines are built in the background too
//                                        (on a new shader cache they froze the game up to 250 ms each)
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
  SetCvarDefault("memexport_vertex_whole_stream", "false");
  SetCvarDefault("readback_resolve_max_kb", "256");
  SetCvarDefault("readback_resolve_half_pixel_offset", "true");
#if defined(__linux__)
  SetCvarDefault("render_target_path_vulkan", "fbo");
#endif
}

}  // namespace trials
