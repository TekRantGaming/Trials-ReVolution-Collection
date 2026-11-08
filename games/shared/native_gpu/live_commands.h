// Native renderer fed from the command stream (see live_commands.cpp).

#pragma once

#include <filesystem>

namespace plume {
struct RenderDevice;
}
namespace rex::memory {
class Memory;
}

void StartLiveCommands(plume::RenderDevice* device, rex::memory::Memory* memory,
                       const std::filesystem::path& cache_folder);
