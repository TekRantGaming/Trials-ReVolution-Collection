// Creates the plume device for the native renderer (see live_commands.cpp).

#include <cstdio>
#include <filesystem>
#include <memory>

#include <plume_render_interface.h>
#ifdef __ANDROID__
#include <dlfcn.h>

#include <volk.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "live_commands.h"

REXCVAR_DECLARE(bool, native_gpu_render);

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

#ifdef __ANDROID__
namespace rex::ui::vulkan {
void* GetLoaderLibraryOverride();
}
#endif

namespace {
std::unique_ptr<plume::RenderInterface> g_interface;
std::unique_ptr<plume::RenderDevice> g_device;
}  // namespace

void StartNativeGpu(rex::Runtime* runtime) {
  if (!REXCVAR_GET(native_gpu_render)) return;
#ifdef __ANDROID__
  // plume reports on stderr, which goes nowhere on Android.
  if (std::filesystem::path log = runtime->cache_root() / "native_stderr.txt"; true) {
    std::freopen(log.c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IONBF, 0);
  }
  // A custom driver (adrenotools) is only reachable through the SDK's handle.
  if (void* driver = rex::ui::vulkan::GetLoaderLibraryOverride()) {
    if (auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver, "vkGetInstanceProcAddr"))) {
      volkInitializeCustom(proc);
    }
  }
#endif
  g_interface = plume::CreateVulkanInterface();
  if (!g_interface) {
    REXLOG_ERROR("native_gpu: no Vulkan interface");
    return;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXLOG_ERROR("native_gpu: no Vulkan device");
    return;
  }
  StartLiveCommands(g_device.get(), runtime->memory(), runtime->cache_root());
}
