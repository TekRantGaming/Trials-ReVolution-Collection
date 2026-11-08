// Native renderer for titles whose state lives in the command stream.
//
// Skate's native layer reads the D3D device at hooked calls. Fusion's engine sends shaders and
// constants itself, so here the state of each draw comes from the GPU plugin's register file
// (rex/graphics/native_guest_renderer.h), laid out as the device snapshot the renderer reads.
// See docs/native_port.md.

#include "live_commands.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dlfcn.h>
#include <link.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "renderer.h"
#include "shader_cache.h"

// Generated from the shader dump: containers by the GPU plugin's microcode hash.
struct NativeContainer {
  uint64_t hash;
  const uint8_t* data;
  uint32_t size;
};
extern const NativeContainer g_nativeContainers[];
extern const size_t g_nativeContainerCount;

REXCVAR_DEFINE_BOOL(native_gpu_render, false, "GPU",
                    "Render with the native renderer, fed from the command stream");
REXCVAR_DEFINE_BOOL(native_gpu_present, true, "GPU", "Show the native renderer's frames");
REXCVAR_DEFINE_BOOL(native_gpu_present_early, false, "GPU",
                    "Show a frame as soon as the GPU has finished it");
REXCVAR_DEFINE_INT32(native_gpu_stats_seconds, 5, "GPU", "Log the native renderer's rate (0 = off)");
REXCVAR_DEFINE_STRING(native_gpu_dump_addresses, "", "GPU",
                      "Resolved textures to write with the traced frame, comma separated hexadecimal");
REXCVAR_DEFINE_BOOL(native_gpu_observe, false, "GPU", "Leave rendering to the emulated GPU, for trace comparisons");
REXCVAR_DEFINE_INT32(native_gpu_frame_limit, 60, "GPU", "Frames per second at most, 0 for none");
REXCVAR_DEFINE_BOOL(native_gpu_memexport_plugin, false, "GPU",
                    "Leave memory export draws to the emulated GPU, after rendering the frame so far for it");
REXCVAR_DEFINE_INT32(native_gpu_trace_frame, 0, "GPU", "Log the commands of this frame");
REXCVAR_DEFINE_STRING(native_gpu_dump_path, "native_frame.ppm", "GPU",
                      "Creating <path>.trigger writes the next front buffer here, <path>.trace traces and dumps the next frame");

namespace {

rex::memory::Memory* g_memory = nullptr;
std::unique_ptr<native_gpu::Renderer> g_renderer;

// Write watching through the physical memory access callbacks (the emulated
// GPU's shared memory uses the same ones): one dirty bit per 4 KB page.
class PhysicalWriteWatcher : public native_gpu::WriteWatcher {
 public:
  explicit PhysicalWriteWatcher(rex::memory::Memory* memory) : memory_(memory) {
    handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  }
  ~PhysicalWriteWatcher() override { memory_->UnregisterPhysicalMemoryInvalidationCallback(handle_); }

  uint32_t Watch(uint32_t address, uint32_t size) override {
    // Writes before this stamp are in what the caller reads next; later ones
    // fault (the pages are protected first) and get a newer sequence.
    const uint32_t stamp = sequence_.load(std::memory_order_acquire);
    memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    return stamp;
  }

  bool Dirty(uint32_t address, uint32_t size, uint32_t stamp) override {
    if (!size) return false;
    const uint32_t first = (address & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((address & 0x1FFFFFFF) + size - 1) >> 12);
    for (uint32_t page = first; page <= last;) {
      // Blocks of 64 pages keep their latest write, to skip unwritten ones.
      if (blocks_[page >> 6].load(std::memory_order_relaxed) <= stamp) {
        page = (page | 63) + 1;
        continue;
      }
      if (pages_[page].load(std::memory_order_relaxed) > stamp) return true;
      ++page;
    }
    return false;
  }

 private:
  static constexpr uint32_t kPages = 0x20000000 >> 12;

  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<PhysicalWriteWatcher*>(context);
    // What to stop watching: (0, UINT32_MAX) would unwatch the surrounding 4 MB
    // and with it everything else converted there.
    if (!length) return {start, 0};
    const uint32_t sequence = self->sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t first = (start & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((start & 0x1FFFFFFF) + length - 1) >> 12);
    for (uint32_t page = first; page <= last; ++page) {
      self->pages_[page].store(sequence, std::memory_order_relaxed);
      self->blocks_[page >> 6].store(sequence, std::memory_order_relaxed);
    }
    return {start, length};
  }

  rex::memory::Memory* memory_;
  void* handle_ = nullptr;
  std::atomic<uint32_t> sequence_{1};
  // The sequence of the last write to each page, and to each 64 pages.
  std::array<std::atomic<uint32_t>, kPages> pages_{};
  std::array<std::atomic<uint32_t>, kPages / 64> blocks_{};
};
std::unique_ptr<PhysicalWriteWatcher> g_watcher;

// Native frames for the presenter (A2B10G10R10, the guest output format),
// handed from the render thread to the GPU command thread at each guest swap.
// The small queue lets a late frame be shown late rather than repeat one frame
// and skip the next, which reads as a stutter at a steady 30 FPS.
struct PresentFrame {
  std::vector<uint32_t> pixels;
  uint32_t width = 0, height = 0;
  std::chrono::steady_clock::time_point swap_time;  // for the latency in the log
};
constexpr size_t kMaxPendingFrames = 2;
std::mutex g_frame_mutex;
std::deque<PresentFrame> g_pending_frames;
PresentFrame g_frame, g_frame_recycled;
uint32_t g_frames_repeated = 0, g_frames_dropped = 0;
uint32_t g_calm_frames = 0;
double g_shown_ms = 0.0;
uint32_t g_shown = 0;
// Presenter-side upload buffer and the guest output images already written.
rex::graphics::nrhi::Buffer* g_upload = nullptr;
uint64_t g_upload_size = 0;
std::unordered_set<rex::graphics::nrhi::Texture*> g_written_outputs;

// Native guest output renderer (rex/graphics/native_guest_renderer.h): copies
// the last native frame into the presenter's guest output image.
bool PresentNativeFrame(const rex::graphics::NativeGuestOutputRenderContext& context, void*);

// The registry is in the runtime-loaded GPU plugin: find its export among
// the loaded libraries once the plugin is up (first swap).
void (*g_set_output_size)(uint32_t, uint32_t) = nullptr;
bool (*g_get_gamma_ramp)(uint32_t*) = nullptr;

bool RegisterPresenter() {
  using SetRenderer = void (*)(rex::graphics::NativeGuestOutputRenderer, void*);
  SetRenderer set = nullptr;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        if (!info->dlpi_name || !std::strstr(info->dlpi_name, "rexgpu")) return 0;
        if (void* handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD)) {
          *static_cast<SetRenderer*>(data) =
              reinterpret_cast<SetRenderer>(dlsym(handle, "rex_gpu_set_native_guest_output_renderer"));
          g_set_output_size =
              reinterpret_cast<void (*)(uint32_t, uint32_t)>(dlsym(handle, "rex_gpu_set_native_guest_output_size"));
          g_get_gamma_ramp = reinterpret_cast<bool (*)(uint32_t*)>(dlsym(handle, "rex_gpu_get_native_gamma_ramp"));
          dlclose(handle);
        }
        return *static_cast<SetRenderer*>(data) ? 1 : 0;
      },
      &set);
  if (!set) {
    REXLOG_ERROR("native_gpu: the GPU plugin has no native guest output entry point");
    return false;
  }
  set(&PresentNativeFrame, nullptr);
  REXLOG_INFO("native_gpu: presenting native frames");
  return true;
}

bool PresentNativeFrame(const rex::graphics::NativeGuestOutputRenderContext& context, void*) {
  namespace nrhi = rex::graphics::nrhi;
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  // Oldest frame first; none ready means the renderer is late, show the last again.
  if (!g_pending_frames.empty()) {
    // A frame in hand absorbs a late one but shows everything a frame later. After
    // a while without a late frame, give it up for the latency; a late frame
    // brings it back by itself.
    constexpr uint32_t kCalmFramesBeforeCatchUp = 300;
    if (REXCVAR_GET(native_gpu_present_early) && g_pending_frames.size() > 1 &&
        ++g_calm_frames >= kCalmFramesBeforeCatchUp) {
      g_pending_frames.pop_front();
      ++g_frames_dropped;
      g_calm_frames = 0;
    }
    g_frame_recycled = std::move(g_frame);
    g_frame = std::move(g_pending_frames.front());
    g_pending_frames.pop_front();
    g_shown_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_frame.swap_time).count();
    ++g_shown;
  } else {
    ++g_frames_repeated;
    g_calm_frames = 0;
  }
  if (g_frame.pixels.empty() || !context.guest_output) return false;
  const uint32_t width = std::min(g_frame.width, context.guest_output->width());
  const uint32_t height = std::min(g_frame.height, context.guest_output->height());
  const uint32_t row_pitch = (width * 4 + nrhi::kRowPitchAlignment - 1) & ~(nrhi::kRowPitchAlignment - 1);
  const uint64_t size = uint64_t(row_pitch) * height;
  if (g_upload_size < size) {
    if (g_upload) context.device->DestroyDeferred(g_upload);
    nrhi::BufferDesc desc;
    desc.size = size;
    desc.heap = nrhi::HeapKind::kUpload;
    desc.bind_class = nrhi::BufferBindClass::kCopySrc;
    g_upload = context.device->CreateBuffer(desc);
    g_upload_size = size;
  }
  auto* dst = static_cast<uint8_t*>(context.device->Map(g_upload));
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + size_t(y) * row_pitch, &g_frame.pixels[size_t(y) * g_frame.width], width * 4);
  }
  context.device->Unmap(g_upload);
  const nrhi::ResourceState initial = g_written_outputs.insert(context.guest_output).second
                                          ? nrhi::ResourceState::kCommon
                                          : nrhi::ResourceState::kGuestOutput;
  context.cmd->Barrier(context.guest_output, initial, nrhi::ResourceState::kCopyDest);
  context.cmd->FlushBarriers();
  context.cmd->CopyBufferToTexture(context.guest_output, 0, 0, g_upload, 0, row_pitch, width, height, 1);
  context.cmd->Barrier(context.guest_output, nrhi::ResourceState::kCopyDest, nrhi::ResourceState::kGuestOutput);
  context.cmd->FlushBarriers();
  return true;
}
void WritePpm(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  // Resolved 8_8_8_8 textures are in memory order (blue first).
  for (size_t p = 0; p < size_t(width) * height; ++p) {
    const uint8_t rgb[3] = {rgba[p * 4 + 2], rgba[p * 4 + 1], rgba[p * 4]};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}


// Renderer thread time, for the stats line.
void RendererThread();
// The frame's calls, rendered at Swap: on the console the GPU only reads
// vertices, indices and textures when it runs the frame, and the game may
// fill them after issuing a draw (the skater's cloth). Each call keeps a copy
// of the device state it was issued with (registers and constants are set
// at that point).
constexpr uint32_t kStateSize = 0x3400;
struct DeferredCall {
  enum class Kind { kDraw, kClear, kResolve } kind;
  size_t state;  // offset in the frame's arena
  native_gpu::DrawCall draw;
  native_gpu::ClearCall clear;
  native_gpu::ResolveCall resolve;
  size_t inline_vertices = SIZE_MAX;  // copied into the arena (the command buffer is reused)
  // Copied too: D3D patches it for each vertex layout during the frame.
  size_t vs_physical = SIZE_MAX;
};
// A recorded frame, rendered by the renderer thread.
struct Frame {
  std::vector<DeferredCall> calls;
  std::vector<uint8_t> arena;
  uint32_t front_buffer = 0;
  bool dump = false;
  bool sync = false;  // read back to guest memory before the game goes on
  bool resolves = false;  // holds resolves the game's memory exports may read
  std::vector<uint32_t> dump_addresses;
};
Frame g_recording;
// The snapshot of the last draw recorded in this frame, for the next to compare with.
size_t g_previous_draw = SIZE_MAX;
void RenderDeferredCalls(Frame& frame) {
  for (DeferredCall& call : frame.calls) {
    const xenos::DeviceState state{frame.arena.data() + call.state};
    switch (call.kind) {
      case DeferredCall::Kind::kDraw:
        call.draw.state = state;
        if (call.inline_vertices != SIZE_MAX) {
          call.draw.inline_vertices = frame.arena.data() + call.inline_vertices;
        }
        if (call.vs_physical != SIZE_MAX) call.draw.vs_physical = frame.arena.data() + call.vs_physical;
        g_renderer->Draw(call.draw);
        break;
      case DeferredCall::Kind::kClear:
        call.clear.state = state;
        g_renderer->Clear(call.clear);
        break;
      case DeferredCall::Kind::kResolve:
        call.resolve.state = state;
        g_renderer->Resolve(call.resolve);
        break;
    }
  }
}

// The renderer thread: renders, submits and reads back each recorded frame
// while the game records the next one (as the console's GPU runs a frame
// behind the CPU). One frame is handed over at a time.
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::optional<Frame> g_queued;
bool g_busy = false;
std::chrono::steady_clock::duration g_thread_time{};
bool g_present = false;

// Hands pixels to the presenter's queue.
void QueuePresentFrame(const uint32_t* pixels, uint32_t width, uint32_t height,
                       std::chrono::steady_clock::time_point swap_time) {
  if (g_set_output_size) g_set_output_size(width, height);
  std::lock_guard<std::mutex> lock(g_frame_mutex);
  if (g_pending_frames.size() >= kMaxPendingFrames) {
    g_pending_frames.pop_front();
    ++g_frames_dropped;
  }
  PresentFrame next = std::move(g_frame_recycled);
  next.width = width;
  next.height = height;
  next.swap_time = swap_time;
  next.pixels.assign(pixels, pixels + size_t(width) * height);
  g_pending_frames.push_back(std::move(next));
}

// Reads each submitted frame as soon as the GPU is done with it, while the
// renderer thread already renders the next one. One at a time.
struct EarlyPresent {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<native_gpu::Renderer::Submitted> job;
  std::chrono::steady_clock::time_point swap_time;
};
EarlyPresent g_early;

void EarlyPresentThread() {
  for (;;) {
    native_gpu::Renderer::Submitted job;
    std::chrono::steady_clock::time_point swap_time;
    {
      std::unique_lock<std::mutex> lock(g_early.mutex);
      g_early.cv.wait(lock, [] { return g_early.job.has_value(); });
      job = *g_early.job;
      swap_time = g_early.swap_time;
    }
    g_renderer->WaitSubmitted(job);
    QueuePresentFrame(job.pixels, job.width, job.height, swap_time);
    {
      std::lock_guard<std::mutex> lock(g_early.mutex);
      g_early.job.reset();
    }
    g_early.cv.notify_all();
  }
}

void WaitEarlyPresent() {
  std::unique_lock<std::mutex> lock(g_early.mutex);
  g_early.cv.wait(lock, [] { return !g_early.job.has_value(); });
}

void RenderFrame(Frame& frame) {
  const auto start = std::chrono::steady_clock::now();
  static double calls_ms = 0.0;  // recording the frame's draws, clears and resolves
  static uint32_t rendered_frames = 0;
  const bool chatty = ++rendered_frames <= 6;
  if (chatty) REXLOG_INFO("native_gpu: frame {} begins, {} calls", rendered_frames, frame.calls.size());
  uint32_t gamma_ramp[256];
  if (g_get_gamma_ramp && g_get_gamma_ramp(gamma_ramp)) g_renderer->SetGammaRamp(gamma_ramp);
  g_renderer->BeginFrame();
  const auto calls_start = std::chrono::steady_clock::now();
  RenderDeferredCalls(frame);
  calls_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - calls_start).count();
  if (chatty) REXLOG_INFO("native_gpu: frame {} recorded", rendered_frames);
  // The last frame's pixels go with this EndFrame.
  WaitEarlyPresent();
  if (chatty) REXLOG_INFO("native_gpu: frame {} early present done", rendered_frames);
  g_renderer->EndFrame(g_present ? frame.front_buffer : 0);
  if (chatty) REXLOG_INFO("native_gpu: frame {} submitted", rendered_frames);
  const auto copy_start = std::chrono::steady_clock::now();
  if (g_present && REXCVAR_GET(native_gpu_present_early)) {
    static const bool started = [] {
      std::thread(EarlyPresentThread).detach();
      return true;
    }();
    (void)started;
    const native_gpu::Renderer::Submitted submitted = g_renderer->submitted();
    if (submitted.pixels) {
      {
        std::lock_guard<std::mutex> lock(g_early.mutex);
        g_early.job = submitted;
        g_early.swap_time = start;
      }
      g_early.cv.notify_all();
    }
  } else if (const uint32_t* pixels = g_present ? g_renderer->presented() : nullptr) {
    QueuePresentFrame(pixels, g_renderer->presented_width(), g_renderer->presented_height(), start);
  }
  const auto copy_time = std::chrono::steady_clock::now() - copy_start;
  if (frame.dump) {
    g_renderer->WaitIdle();
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0, format = 0;
    std::vector<uint32_t> addresses = frame.dump_addresses;
    addresses.push_back(frame.front_buffer);
    {
      const std::string list = REXCVAR_GET(native_gpu_dump_addresses);
      for (size_t at = 0; at < list.size();) {
        size_t used = 0;
        addresses.push_back(uint32_t(std::stoul(list.substr(at), &used, 16)));
        at += used + 1;
      }
    }
    for (uint32_t address : addresses) {
      if (!g_renderer->ReadResolved(address, rgba, width, height, &format)) {
        REXLOG_INFO("native_gpu: nothing resolved at {:08X}", address);
        continue;
      }
      const std::string path = fmt::format("{}.{:08X}_{}x{}_{}.raw", REXCVAR_GET(native_gpu_dump_path), address,
                                           width, height, format);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(rgba.data(), 1, rgba.size(), f);
        std::fclose(f);
      }
    }
    g_renderer->WriteTraceDumps(REXCVAR_GET(native_gpu_dump_path));
  }
  const auto spent = std::chrono::steady_clock::now() - start;
  g_thread_time += spent;
  static double worst_ms = 0.0, copy_ms = 0.0;
  static uint32_t frames = 0;
  worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(spent).count());
  copy_ms += std::chrono::duration<double, std::milli>(copy_time).count();
  if (++frames == 300) {
    g_renderer->SavePipelineCache();
    const native_gpu::Renderer::Timings t = g_renderer->TakeTimings();
    REXLOG_INFO("native_gpu: renderer, ms/frame: GPU {:.1f}, GPU wait {:.1f}, calls {:.1f}, read back {:.1f}, textures {:.1f} ({} uploaded, {} unchanged, {} deferred, worst frame {:.1f}, watch {:.0f} ms, create {:.0f} ms), "
                "buffers {:.1f} (worst frame {:.1f}), submit {:.1f} (worst {:.1f}), present copy {:.1f}; "
                "worst frame {:.1f}",
                t.gpu_ms / frames, t.gpu_wait_ms / frames, calls_ms / frames, t.read_back_ms / frames, t.texture_ms / frames, t.textures, t.textures_unchanged, t.textures_deferred, t.worst_texture_frame_ms,
                t.texture_watch_ms, t.texture_create_ms,
                t.buffer_ms / frames, t.worst_buffer_frame_ms, t.submit_ms / frames, t.worst_submit_ms,
                copy_ms / frames, worst_ms);
    frames = 0;
    worst_ms = copy_ms = calls_ms = 0.0;
  }
}

// Logs what the renderer could not do, first time and when it grows tenfold,
// so a player's log tells which draws went missing.
void LogRendererIssues() {
  static std::map<std::string, uint32_t> logged;
  for (const auto& [what, count] : g_renderer->stats()) {
    if (what.find("skipped") == std::string::npos && what.find("vertex element") == std::string::npos &&
        what.find("heap full") == std::string::npos) {
      continue;
    }
    uint32_t& last = logged[what];
    if (count >= last * 10 + 1) {
      last = count;
      REXLOG_WARN("native_gpu: {} ({}x)", what, count);
    }
  }
}

void RendererThread() {
  for (;;) {
    Frame frame;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return g_queued.has_value(); });
      frame = std::move(*g_queued);
      g_queued.reset();
      g_busy = true;
    }
    RenderFrame(frame);
    if (frame.sync) g_renderer->WaitIdle();
    static uint32_t rendered = 0;
    if (++rendered % 300 == 0) LogRendererIssues();
    {
      std::lock_guard<std::mutex> lock(g_queue_mutex);
      g_busy = false;
    }
    g_queue_cv.notify_all();
  }
}



// Device snapshot offsets for what Skate's device does not hold.
constexpr uint32_t kLoopConstants = 0x33F0;

void StoreBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

constexpr uint32_t kRegFetchConstants = 0x4800;
constexpr uint32_t kRegFloatConstants = 0x4000;
constexpr uint32_t kRegBoolConstants = 0x4900;
constexpr uint32_t kRegLoopConstants = 0x4908;

// A 4x surface holds the same EDRAM samples as the 1x surface of twice the size: everything is
// drawn at 1x, with the viewport, scissors and pitch of those draws doubled.
bool Surface4x(const uint32_t* regs) { return ((regs[xenos::reg::RB_SURFACE_INFO] >> 16) & 3) == 2; }

void PutReg(uint8_t* state, uint32_t reg, uint32_t value) {
  if (const uint32_t offset = xenos::RegisterOffset(reg)) StoreBE32(state + offset, value);
}

void DoubleFloat(uint8_t* state, const uint32_t* regs, uint32_t reg) {
  float value;
  std::memcpy(&value, &regs[reg], 4);
  value *= 2.0f;
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PutReg(state, reg, bits);
}

// Two 14 or 15 bit fields at bits 0 and 16, clamped to what they hold.
uint32_t DoublePair(uint32_t value, uint32_t keep_mask) {
  const uint32_t x = std::min<uint32_t>((value & 0x3FFF) * 2, 0x3FFF);
  const uint32_t y = std::min<uint32_t>(((value >> 16) & 0x3FFF) * 2, 0x3FFF);
  return (value & keep_mask) | x | (y << 16);
}

size_t SnapshotState(const uint32_t* regs) {
  std::vector<uint8_t>& arena = g_recording.arena;
  const size_t offset = arena.size();
  arena.resize(offset + kStateSize);
  uint8_t* state = arena.data() + offset;
  for (const xenos::RegisterGroup& group : xenos::kRegisterGroups) {
    for (uint32_t i = 0; i < group.count; ++i) {
      StoreBE32(state + group.offset + 4 * i, regs[group.first + i]);
    }
  }
  for (uint32_t i = 0; i < 192; ++i) {
    StoreBE32(state + xenos::device::kFetchConstants + 4 * i, regs[kRegFetchConstants + i]);
  }
  // The float constants stay in the host's order: the renderer uploads them as they are.
  std::memcpy(state + xenos::device::kVertexConstants, regs + kRegFloatConstants, 256 * 16);
  std::memcpy(state + xenos::device::kPixelConstants, regs + kRegFloatConstants + 256 * 4, 256 * 16);
  for (uint32_t i = 0; i < 4; ++i) {
    StoreBE32(state + xenos::device::kVertexBoolConstants + 4 * i, regs[kRegBoolConstants + i]);
    StoreBE32(state + xenos::device::kPixelBoolConstants + 4 * i, regs[kRegBoolConstants + 4 + i]);
  }
  StoreBE32(state + kLoopConstants, regs[kRegLoopConstants + 16]);
  StoreBE32(state + kLoopConstants + 4, regs[kRegLoopConstants + 17]);
  if (Surface4x(regs)) {
    const uint32_t info = regs[xenos::reg::RB_SURFACE_INFO];
    PutReg(state, xenos::reg::RB_SURFACE_INFO, (info & ~uint32_t(0x33FFF)) | ((info & 0x3FFF) * 2));
    for (uint32_t reg : {xenos::reg::PA_CL_VPORT_XSCALE, xenos::reg::PA_CL_VPORT_XOFFSET,
                         xenos::reg::PA_CL_VPORT_YSCALE, xenos::reg::PA_CL_VPORT_YOFFSET}) {
      DoubleFloat(state, regs, reg);
    }
  }
  return offset;
}

struct KnownShader {
  uint64_t key = 0;  // 0: not in the shader cache
  bool memexport = false;
  uint32_t constants = 256;  // float constants (from c0) its ALU instructions read
  std::vector<uint32_t> streams;  // float constants the shader's memory export streams come from
  std::vector<uint8_t> big_endian;
};

// True when the microcode allocates memory export (alloc type 3 among its control flow
// instructions, which come before the first clause they execute).
bool WritesMemory(const uint32_t* ucode, uint32_t dwords) {
  uint32_t limit = dwords / 3 * 2;
  for (uint32_t i = 0; i < limit; ++i) {
    const uint32_t* triple = ucode + i / 2 * 3;
    const uint32_t word0 = i % 2 ? triple[1] >> 16 | triple[2] << 16 : triple[0];
    const uint32_t word1 = i % 2 ? triple[2] >> 16 : triple[1] & 0xFFFF;
    const uint32_t opcode = word1 >> 12;
    if (opcode == 12 && ((word1 >> 9) & 3) == 3) return true;
    if (opcode >= 1 && opcode <= 7) limit = std::min(limit, (word0 & 0x1FF) * 2);
  }
  return false;
}
// The float constants memory export streams are set from: the ALU instructions of the exec clauses
// that write the export address with `mad eA, r#, c0100, c#`.
std::vector<uint32_t> MemoryExportStreams(const uint32_t* ucode, uint32_t dwords) {
  std::vector<uint32_t> streams;
  uint32_t limit = dwords / 3 * 2;
  for (uint32_t i = 0; i < limit; ++i) {
    const uint32_t* triple = ucode + i / 2 * 3;
    const uint32_t word0 = i % 2 ? triple[1] >> 16 | triple[2] << 16 : triple[0];
    const uint32_t word1 = i % 2 ? triple[2] >> 16 : triple[1] & 0xFFFF;
    const uint32_t opcode = word1 >> 12;
    if (opcode >= 1 && opcode <= 7) limit = std::min(limit, (word0 & 0x1FF) * 2);
    if ((opcode < 1 || opcode > 6) && opcode != 13 && opcode != 14) continue;
    const uint32_t address = word0 & 0xFFF, count = (word0 >> 12) & 7, sequence = (word0 >> 16) & 0xFFF;
    for (uint32_t j = 0; j < count; ++j) {
      if ((sequence >> (2 * j)) & 1 || (address + j) * 3 + 2 >= dwords) continue;  // a fetch
      const uint32_t* alu = ucode + (address + j) * 3;
      constexpr uint32_t kExportAddress = 32, kMad = 11;
      const bool exports = (alu[0] >> 15) & 1, constant = !((alu[2] >> 29) & 1);
      if (exports && (alu[0] & 0x3F) == kExportAddress && ((alu[2] >> 24) & 0x1F) == kMad && constant) {
        const uint32_t index = alu[2] & 0xFF;
        if (std::find(streams.begin(), streams.end(), index) == streams.end()) streams.push_back(index);
      }
    }
  }
  return streams;
}

// How many float constants, from c0, the ALU instructions of the exec clauses read; all 256 when one of them
// addresses constants relative to a register. A constant operand takes the register field of its source.
uint32_t ConstantCount(const uint32_t* ucode, uint32_t dwords) {
  uint32_t used = 0;
  uint32_t limit = dwords / 3 * 2;
  for (uint32_t i = 0; i < limit; ++i) {
    const uint32_t* triple = ucode + i / 2 * 3;
    const uint32_t word0 = i % 2 ? triple[1] >> 16 | triple[2] << 16 : triple[0];
    const uint32_t word1 = i % 2 ? triple[2] >> 16 : triple[1] & 0xFFFF;
    const uint32_t opcode = word1 >> 12;
    if (opcode >= 1 && opcode <= 7) limit = std::min(limit, (word0 & 0x1FF) * 2);
    if ((opcode < 1 || opcode > 6) && opcode != 13 && opcode != 14) continue;
    const uint32_t address = word0 & 0xFFF, count = (word0 >> 12) & 7, sequence = (word0 >> 16) & 0xFFF;
    for (uint32_t j = 0; j < count; ++j) {
      if ((sequence >> (2 * j)) & 1 || (address + j) * 3 + 2 >= dwords) continue;  // a fetch
      const uint32_t* alu = ucode + (address + j) * 3;
      bool reads = false;
      for (uint32_t source = 0; source < 3; ++source) {  // src3, src2, src1
        if ((alu[2] >> (29 + source)) & 1) continue;  // a register
        reads = true;
        used = std::max(used, ((alu[2] >> (8 * source)) & 0xFF) + 1);
      }
      if (reads && (alu[1] >> 30)) return 256;  // const_0_rel_abs, const_1_rel_abs
    }
  }
  return used;
}

std::unordered_map<uint64_t, KnownShader> g_shaders;
std::mutex g_container_mutex;
std::unordered_map<uint64_t, const uint8_t*> g_containers;

const KnownShader& FindShader(uint64_t hash, const uint32_t* ucode, uint32_t dwords) {
  auto [it, inserted] = g_shaders.try_emplace(hash);
  if (inserted) {
    KnownShader& shader = it->second;
    shader.big_endian.resize(dwords * 4);
    for (uint32_t i = 0; i < dwords; ++i) StoreBE32(&shader.big_endian[i * 4], ucode[i]);
    shader.constants = ConstantCount(ucode, dwords);
    shader.memexport = WritesMemory(ucode, dwords);
    if (shader.memexport) shader.streams = MemoryExportStreams(ucode, dwords);
    const uint64_t key = XXH3_64bits(shader.big_endian.data(), shader.big_endian.size());
    const ShaderCacheEntry* begin = g_shaderCacheEntries;
    const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
    const ShaderCacheEntry* entry = std::lower_bound(
        begin, end, key,
        [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
    if (entry != end && entry->hash == key) shader.key = key;
    for (size_t i = 0; i < g_nativeContainerCount; ++i) {
      if (g_nativeContainers[i].hash == hash) {
        std::lock_guard<std::mutex> lock(g_container_mutex);
        g_containers[key] = g_nativeContainers[i].data;
      }
    }
    if (!shader.key) REXLOG_WARN("native_gpu: shader {:016X} is not in the cache", hash);
  }
  return it->second;
}

const uint8_t* FindContainer(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_container_mutex);
  const auto it = g_containers.find(key);
  return it == g_containers.end() ? nullptr : it->second;
}

// Memory export draws run on the plugin, which reads their inputs from guest memory: what the
// renderer resolved so far has to be there first, so the frame so far is rendered and read back.
void SyncRenderer() {
  if (!g_recording.resolves) return;
  Frame frame = std::move(g_recording);
  g_recording = Frame{};
  g_previous_draw = SIZE_MAX;
  frame.sync = true;
  std::unique_lock<std::mutex> lock(g_queue_mutex);
  g_queue_cv.wait(lock, [] { return !g_queued.has_value(); });
  g_queued = std::move(frame);
  g_queue_cv.notify_all();
  g_queue_cv.wait(lock, [] { return !g_queued.has_value() && !g_busy; });
}

// Bytes of one element a memory export stream writes in a colour format, 0 for those it does not take.
uint32_t ExportElementBytes(uint32_t format) {
  switch (format) {
    case 2: case 8: case 9: return 1;
    case 3: case 4: case 5: case 10: case 15: case 24: case 30: return 2;
    case 6: case 7: case 14: case 16: case 17: case 25: case 31: case 36: case 50: case 54: case 55: case 56: return 4;
    case 26: case 32: case 37: return 8;
    case 38: return 16;
    default: return 0;
  }
}

// The guest ranges a draw's memory export streams write (draw_util::AddMemExportRanges).
void AddExportRanges(const KnownShader& shader, const uint32_t* constants, native_gpu::DrawCall& call) {
  for (uint32_t index : shader.streams) {
    const uint32_t* stream = constants + 4 * index;
    // The hardware validates the upper bits of the stream.
    if ((stream[0] >> 30) != 1 || (stream[2] >> 20) != 0x4B0 || (stream[3] >> 23) != 0x96) continue;
    const uint32_t count = stream[3] & 0x7FFFFF, bytes = ExportElementBytes((stream[2] >> 8) & 0x3F);
    if (!count || !bytes || call.export_count == native_gpu::DrawCall::kMaxExports) continue;
    call.exports[call.export_count++] = {(stream[0] << 2) & 0x1FFFFFFF, count * bytes};
  }
}

// The first and one past the last float4 of 256 that differ between two snapshots' constants.
std::pair<uint32_t, uint32_t> ChangedConstants(const uint8_t* before, const uint8_t* now) {
  uint32_t first = 0, last = 256;
  while (first < last && !std::memcmp(before + first * 16, now + first * 16, 16)) ++first;
  while (last > first && !std::memcmp(before + (last - 1) * 16, now + (last - 1) * 16, 16)) --last;
  return {first, last};
}

uint32_t g_trace_index = 0;
bool OnDraw(const rex::graphics::NativeCommandEvent& event) {
  if (!event.vs_ucode) return true;
  const KnownShader& vertex = FindShader(event.vs_hash, event.vs_ucode, event.vs_dwords);
  const KnownShader* pixel = event.ps_ucode ? &FindShader(event.ps_hash, event.ps_ucode, event.ps_dwords) : nullptr;
  // Memory export writes into the renderer's own buffer; the plugin's way needs the frame so far in guest memory.
  if ((vertex.memexport || (pixel && pixel->memexport)) &&
      (REXCVAR_GET(native_gpu_memexport_plugin) || !vertex.key || (pixel && !pixel->key))) {
    SyncRenderer();
    return false;
  }
  if (!vertex.key || (pixel && !pixel->key)) return false;
  native_gpu::DrawCall call;
  call.vs_key = vertex.key;
  call.vs_constants = vertex.constants;
  if (pixel) {
    call.ps_key = pixel->key;
    call.ps_constants = pixel->constants;
  }
  call.vs_physical = vertex.big_endian.data();
  call.vs_physical_size = uint32_t(vertex.big_endian.size());
  call.primitive = event.primitive;
  call.scissor_tl = event.registers[0x2081];  // PA_SC_WINDOW_SCISSOR_TL
  call.scissor_br = event.registers[0x2082];
  call.surface_4x = Surface4x(event.registers);
  if (call.surface_4x) {
    call.scissor_tl = DoublePair(call.scissor_tl, 0x80000000u);
    call.scissor_br = DoublePair(call.scissor_br, 0x80000000u);
  }
  call.count = event.count;
  AddExportRanges(vertex, event.registers + kRegFloatConstants, call);
  if (pixel) AddExportRanges(*pixel, event.registers + kRegFloatConstants + 256 * 4, call);
  call.base_vertex = event.registers[0x2102];  // VGT_INDX_OFFSET
  if (event.indexed) {
    call.indices_address = event.index_address;
    call.indices_size = event.index_size;
    call.indices_32bit = event.indices_32bit;
    call.indices_endian = event.index_endian;
  }
  const size_t draw_state = SnapshotState(event.registers);
  // What the previous draw of this frame left as it was: the renderer uploads and rebinds only what changed.
  if (g_previous_draw != SIZE_MAX) {
    const uint8_t* now = g_recording.arena.data() + draw_state;
    const uint8_t* before = g_recording.arena.data() + g_previous_draw;
    std::tie(call.vs_dirty_begin, call.vs_dirty_end) =
        ChangedConstants(before + xenos::device::kVertexConstants, now + xenos::device::kVertexConstants);
    std::tie(call.ps_dirty_begin, call.ps_dirty_end) =
        ChangedConstants(before + xenos::device::kPixelConstants, now + xenos::device::kPixelConstants);
    call.fetch_changed = std::memcmp(before + xenos::device::kFetchConstants, now + xenos::device::kFetchConstants, 16 * 24) != 0;
  }
  g_previous_draw = draw_state;
  DeferredCall deferred{DeferredCall::Kind::kDraw, draw_state, call, {}, {}};
  g_recording.calls.push_back(deferred);
  return true;
}

bool OnCopy(const rex::graphics::NativeCommandEvent& event) {
  const uint32_t* regs = event.registers;
  const uint32_t control = regs[0x2318];  // RB_COPY_CONTROL
  const uint32_t source = control & 7;
  const bool depth = source >= 4;
  const uint32_t command = (control >> 20) & 3;
  const bool color_clear = (control >> 8) & 1, depth_clear = (control >> 9) & 1;
  // The resolve rectangle is the vertices of the draw: three pairs of floats.
  const uint32_t fetch = regs[kRegFetchConstants];
  const uint8_t* vertices = g_memory->TranslatePhysical(fetch & 0xFFFFFFFC);
  int32_t rect[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
  for (int v = 0; v < 3; ++v) {
    const int32_t x = int32_t(xenos::LoadBEFloat(vertices + 8 * v));
    const int32_t y = int32_t(xenos::LoadBEFloat(vertices + 8 * v + 4));
    rect[0] = std::min(rect[0], x);
    rect[1] = std::min(rect[1], y);
    rect[2] = std::max(rect[2], x);
    rect[3] = std::max(rect[3], y);
  }
  const int scale = Surface4x(regs) ? 2 : 1;
  if (((regs[0x231B] >> 7) & 0x3F) == 3 && (rect[2] - rect[0] > 256 || event.ps_hash == 0x0D26ADDF22CBC739ull)) {
    REXLOG_INFO("BIGTABLE ps={:016X} dest={:08X} pitch={:08X} rect {} {} {} {} surf={:08X} c0={:08X}", event.ps_hash, regs[0x2319],
                regs[0x231A], rect[0], rect[1], rect[2], rect[3], regs[0x2000], regs[0x2001]);
  }
  const size_t state = SnapshotState(regs);
  // The renderer resolves RB_COLOR_INFO: put the selected target there.
  if (!depth && source) {
    static constexpr uint32_t kColorInfo[] = {0x2001, 0x2003, 0x2004, 0x2005};
    PutReg(g_recording.arena.data() + state, 0x2001, regs[kColorInfo[source & 3]]);
  }
  if (command != 3) {
    native_gpu::ResolveCall call;
    call.flags = depth ? 4 : 0;
    call.has_rect = true;
    std::copy(rect, rect + 4, call.rect);
    call.source_scale = scale;
    call.second_target = !depth && source;
    call.trace_index = g_trace_index ? g_trace_index++ : 0;
    if (call.trace_index) g_recording.dump = true;
    DeferredCall deferred{DeferredCall::Kind::kResolve, state, {}, {}, call};
    g_recording.calls.push_back(deferred);
    g_recording.resolves = true;
  }
  if (color_clear || depth_clear) {
    native_gpu::ClearCall call;
    call.flags = (color_clear ? 0xF : 0) | (depth_clear ? 0x30 : 0);
    call.has_rect = true;
    call.raw_color = true;
    for (int i = 0; i < 4; ++i) call.rect[i] = rect[i] * scale;
    const uint32_t value = regs[0x231E];  // RB_COLOR_CLEAR, ARGB
    for (int c = 0; c < 4; ++c) call.color[c] = float((value >> ((c + 1) % 4 ? 24 - 8 * (c + 1) : 24)) & 0xFF) / 255.0f;
    const uint32_t z = regs[0x231D];  // RB_DEPTH_CLEAR
    call.depth = float(z >> 8) / 16777215.0f;
    call.stencil = z & 0xFF;
    DeferredCall deferred{DeferredCall::Kind::kClear, state, {}, call, {}};
    g_recording.calls.push_back(deferred);
  }
  return true;
}

uint32_t g_frame_number = 0;
void OnSwap(const rex::graphics::NativeCommandEvent& event) {
  ++g_frame_number;
  if (g_frame_number % 600 == 0 && g_renderer) {
    for (const auto& [what, count] : g_renderer->stats()) REXLOG_INFO("native_gpu: stat {} {}", count, what);
  }
  g_recording.front_buffer = event.front_buffer & 0x1FFFFFFF;
  static const bool registered = REXCVAR_GET(native_gpu_present) && RegisterPresenter();
  g_present = registered;
  const std::string trigger = REXCVAR_GET(native_gpu_dump_path) + ".trigger";
  std::error_code error;
  if (std::filesystem::remove(trigger, error)) g_recording.dump = true;
  static auto stats_start = std::chrono::steady_clock::now();
  static uint32_t stats_frames = 0;
  ++stats_frames;
  const auto now = std::chrono::steady_clock::now();
  const int32_t stats_seconds = REXCVAR_GET(native_gpu_stats_seconds);
  if (stats_seconds > 0 && now - stats_start >= std::chrono::seconds(stats_seconds)) {
    const double seconds = std::chrono::duration<double>(now - stats_start).count();
    REXLOG_INFO("native_gpu: {:.1f} FPS, {:.1f} ms/frame on the renderer thread", stats_frames / seconds,
                std::chrono::duration<double, std::milli>(g_thread_time).count() / stats_frames);
    g_thread_time = {};
    stats_start = now;
    stats_frames = 0;
  }
  // The console's swap waits for a vertical blank; nothing here does.
  if (const int32_t limit = REXCVAR_GET(native_gpu_frame_limit); limit > 0) {
    static auto next = std::chrono::steady_clock::now();
    const auto interval = std::chrono::nanoseconds(1000000000 / limit);
    next = std::max(next + interval, std::chrono::steady_clock::now() - interval);
    std::this_thread::sleep_until(next);
  }
  const size_t recorded_calls = g_recording.calls.size();
  const size_t recorded_bytes = g_recording.arena.size();
  if (g_frame_number <= 6) REXLOG_INFO("native_gpu: swap {} queued, {} calls", g_frame_number, recorded_calls);
  {
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    g_queue_cv.wait(lock, [] { return !g_queued.has_value(); });
    g_queued = std::move(g_recording);
  }
  g_queue_cv.notify_all();
  g_recording = Frame{};
  g_previous_draw = SIZE_MAX;
  g_recording.calls.reserve(recorded_calls + recorded_calls / 4);
  g_recording.arena.reserve(recorded_bytes + recorded_bytes / 4);
}

// One frame's commands in the log, for debugging: native_gpu_trace_frame = N.
void TraceCommand(const rex::graphics::NativeCommandEvent& event, bool handled) {
  const uint32_t* r = event.registers;
  std::string textures;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t* t = r + kRegFetchConstants + 6 * i;
    if ((t[0] & 3) == 2 && t[1]) {
      textures += fmt::format(" t{}:{:08X}f{}", i, t[1] & 0xFFFFF000, t[1] & 0x3F);
    }
  }
  {
    static std::unordered_set<uint32_t> seen;
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t* t = r + kRegFetchConstants + 6 * i;
      const uint32_t address = t[1] & 0xFFFFF000;
      if ((t[0] & 3) == 2 && address && seen.insert(address).second) {
        const uint8_t* p = g_memory->TranslatePhysical(address);
        uint32_t nonzero = 0;
        const uint32_t width = (t[2] & 0x1FFF) + 1, height = ((t[2] >> 13) & 0x1FFF) + 1;
        const uint32_t bytes = width * height;  // at least half the texture for every format used here
        uint32_t samples = 0;
        for (uint32_t k = 0; k + 1 < bytes; k += std::max(bytes / 2048, 2u), ++samples) {
          nonzero += p[k] != 0 || p[k + 1] != 0;
        }
        REXLOG_INFO("TRACETEX {:08X} fmt={} {}x{} nonzero={}/{}", address, t[1] & 0x3F, width, height, nonzero, samples);
        if ((t[1] & 0x3F) == 3) {
          const uint32_t mips = t[5] & 0xFFFFF000;
          const auto count = [](const uint8_t* q, uint32_t size) {
            uint32_t n = 0;
            for (uint32_t k = 0; k + 4 <= size; k += 4) n += (q[k] | q[k + 1] | q[k + 2] | q[k + 3]) != 0;
            return n;
          };
          REXLOG_INFO("TRACETEX table words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} base nonzero {} mips {:08X} nonzero {}",
                      t[0], t[1], t[2], t[3], t[4], t[5], count(p, 8 << 20), mips,
                      mips ? count(g_memory->TranslatePhysical(mips), 3 << 20) : 0);
        }
      }
    }
  }
  REXLOG_INFO("TRACE {} prim={} n={} idx={} vs={:016X} ps={:016X} surf={:08X} c0={:08X} c1={:08X} d={:08X} mask={:08X} mode={:X} dc={:08X} handled={}{}",
              event.kind == rex::graphics::NativeCommandEvent::Kind::kCopy ? "COPY" : "DRAW", event.primitive,
              event.count, event.indexed, event.vs_hash, event.ps_hash, r[0x2000], r[0x2001], r[0x2003], r[0x2002],
              r[0x2104], r[0x2208], r[0x2200], handled, textures);
  if (event.kind == rex::graphics::NativeCommandEvent::Kind::kCopy) {
    // What guest memory holds at the destination, from the last time this ran.
    const uint32_t pitch = r[0x231A] & 0x3FFF, rows = (r[0x231A] >> 16) & 0x3FFF;
    const uint32_t bytes = std::min<uint32_t>(pitch * rows * 2, 1 << 20);
    const uint8_t* m = g_memory->TranslatePhysical(r[0x2319] & 0xFFFFF000);
    uint32_t nonzero = 0, words = 0;
    std::string sample;
    for (uint32_t k = 0; k + 4 <= bytes; k += 4, ++words) {
      if (m[k] | m[k + 1] | m[k + 2] | m[k + 3]) {
        if (nonzero++ < 4) sample += fmt::format(" {:02X}{:02X}{:02X}{:02X}", m[k], m[k + 1], m[k + 2], m[k + 3]);
      }
    }
    REXLOG_INFO("TRACE   guest {:08X} nonzero {}/{}{}", r[0x2319], nonzero, words, sample);
    const uint8_t* v = g_memory->TranslatePhysical(r[kRegFetchConstants] & 0xFFFFFFFC);
    REXLOG_INFO("TRACE   copy control={:08X} dest={:08X} pitch={:08X} info={:08X} v {} {} {} {} {} {} woff {:08X}", r[0x2318], r[0x2319], r[0x231A], r[0x231B],
                xenos::LoadBEFloat(v), xenos::LoadBEFloat(v + 4), xenos::LoadBEFloat(v + 8), xenos::LoadBEFloat(v + 12), xenos::LoadBEFloat(v + 16), xenos::LoadBEFloat(v + 20), r[0x2080]);
  }
}

// What the emulated GPU left at the front buffer in guest memory (1024x608, 8_8_8_8, tiled), untiled,
// for comparison with the native frame's dump. Needs the plugin's resolves read back to memory.
void DumpGuestFrontBuffer(uint32_t address) {
  constexpr uint32_t kWidth = 1024, kHeight = 608;
  const uint8_t* source = g_memory->TranslatePhysical(address);
  std::vector<uint8_t> rgba(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const uint8_t* pixel = source + xenos::TiledOffset2D(int32_t(x), int32_t(y), kWidth, 2);
      uint8_t* out = &rgba[(y * kWidth + x) * 4];
      // The resolve has no swap bit and memory holds R, G, B, A, but the game presents it through a swizzle of z, y, x
      // (fetch constant 0 at the swap), so what is shown is B, G, R.
      out[0] = pixel[2];
      out[1] = pixel[1];
      out[2] = pixel[0];
      out[3] = 0xFF;
    }
  }
  const std::string path = fmt::format("{}.{:08X}_{}x{}_20.raw", REXCVAR_GET(native_gpu_dump_path), address, kWidth, kHeight);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(rgba.data(), 1, rgba.size(), f);
    std::fclose(f);
  }
}

// The virtual texture table (2048x2048 1_5_5_5 with 8 levels, the CPU's view of it): found among the fetch constants.
uint32_t g_table_mips = 0, g_table_base = 0;

void FindTable(const uint32_t* registers) {
  for (uint32_t i = 0; i < 16 && !g_table_mips; ++i) {
    const uint32_t* t = registers + kRegFetchConstants + 6 * i;
    if ((t[0] & 3) == 2 && (t[1] & 0x3F) == 3 && (t[2] & 0x1FFF) == 2047 && t[5]) {
      g_table_mips = t[5] & 0xFFFFF000;
      g_table_base = t[1] & 0xFFFFF000;
    }
  }
}

// Guest memory's table as the CPU reads it: its mips, then its base.
void DumpGuestTable(const std::string& path) {
  if (!g_table_mips) return;
  const size_t bytes = size_t(g_table_base - g_table_mips) + 2048 * 2048 * 2;
  if (FILE* f = std::fopen((path + ".table.bin").c_str(), "wb")) {
    std::fwrite(g_memory->TranslatePhysical(g_table_mips), 1, bytes, f);
    std::fclose(f);
  }
}

bool OnCommand(const rex::graphics::NativeCommandEvent& event, void*) {
  using Kind = rex::graphics::NativeCommandEvent::Kind;
  if (event.kind != Kind::kSwap) FindTable(event.registers);
  // The emulated GPU renders; the trace only looks, for a reference to compare with.
  if (REXCVAR_GET(native_gpu_observe)) {
    static uint32_t heavy = 0, draws = 0, frame = 0, last_count = 0;
    static std::map<std::string, uint32_t> recent;
    if (event.kind != Kind::kSwap) {
      const uint32_t* r = event.registers;
      recent[fmt::format("{} vs={:016X} ps={:016X} m={:X}", event.kind == Kind::kCopy ? "COPY" : "DRAW", event.vs_hash, event.ps_hash,
                         event.kind == Kind::kCopy ? r[0x2319] >> 16 : r[0x2104])]++;
    } else {
      if (g_table_mips && ++frame % 10 == 0) {
        const uint8_t* q = g_memory->TranslatePhysical(g_table_mips);
        uint32_t n = 0;
        for (uint32_t k = 0; k + 4 <= (3u << 20); k += 4) n += (q[k] | q[k + 1] | q[k + 2] | q[k + 3]) != 0;
        if (n != last_count && (last_count == 0 || frame < 40)) {
          REXLOG_INFO("TABLE frame {} mips {:08X} nonzero {} -> {}", frame, g_table_mips, last_count, n);
          for (const auto& [what, count] : recent) REXLOG_INFO("TABLE   {} x{}", what, count);
        }
        last_count = n;
        recent.clear();
      }
    }
    static bool tracing = false;
    struct Resolve {
      uint32_t address, pitch, rows, format;
    };
    static std::vector<Resolve> resolves;
    if (event.kind == Kind::kSwap) {
      // The same frame as the native trace's (native_gpu_trace_frame = -1, or <dump>.trace), with its front
      // buffer, like the native dump's, and what each resolve left in guest memory; the front buffer on request.
      std::error_code error;
      const std::string path = REXCVAR_GET(native_gpu_dump_path);
      if (std::filesystem::remove(path + ".trigger", error) || tracing) {
        DumpGuestFrontBuffer(event.front_buffer & 0x1FFFFFFF);
      }
      if (tracing) DumpGuestTable(path);
      for (size_t i = 0; i < resolves.size(); ++i) {
        const Resolve& resolve = resolves[i];
        const xenos::TextureBlock block = xenos::GetTextureBlock(resolve.format);
        const size_t bytes = size_t(resolve.pitch) * resolve.rows * block.bytes;
        if (!bytes || bytes > (64u << 20)) continue;
        const std::string file = fmt::format("{}.{:03}.{:08X}_{}x{}_guest{}.bin", path, i + 1, resolve.address, resolve.pitch,
                                             resolve.rows, resolve.format);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
          std::fwrite(g_memory->TranslatePhysical(resolve.address), 1, bytes, f);
          std::fclose(f);
        }
      }
      resolves.clear();
      tracing = (REXCVAR_GET(native_gpu_trace_frame) == -1 && draws > 800 && heavy++ == 240) ||
                std::filesystem::remove(path + ".trace", error);
      draws = 0;
    } else {
      draws += event.kind == Kind::kDraw;
      if (tracing) {
        TraceCommand(event, false);
        const uint32_t* r = event.registers;
        if (event.kind == Kind::kCopy) {
          resolves.push_back({r[0x2319] & 0xFFFFF000, r[0x231A] & 0x3FFF, (r[0x231A] >> 16) & 0x3FFF, (r[0x231B] >> 7) & 0x3F});
        }
      }
    }
    return false;
  }
  static uint32_t draws_this_frame = 0, traced = 0;
  static bool tracing = false;
  if (event.kind == Kind::kDraw) ++draws_this_frame;
  if (event.kind == Kind::kSwap) {
    if (tracing) DumpGuestTable(REXCVAR_GET(native_gpu_dump_path));
    std::error_code error;
    tracing = (REXCVAR_GET(native_gpu_trace_frame) == -1 && draws_this_frame > 800 && traced++ == 240) ||
              std::filesystem::remove(REXCVAR_GET(native_gpu_dump_path) + ".trace", error);
    draws_this_frame = 0;
    g_trace_index = tracing ? 1 : 0;
  }
  if ((tracing || int32_t(g_frame_number) == REXCVAR_GET(native_gpu_trace_frame)) && event.kind != Kind::kSwap) {
    bool handled = event.kind == Kind::kCopy ? OnCopy(event) : OnDraw(event);
    TraceCommand(event, handled);
    if (event.kind == Kind::kCopy && ((event.registers[0x2318] >> 20) & 3) != 3 &&
        (event.registers[0x2319] & 0xFF000000) != 0x1E000000) {
      g_recording.dump_addresses.push_back(event.registers[0x2319] & 0xFFFFF000);
      g_recording.dump = true;
    }
    return handled;
  }
  switch (event.kind) {
    case Kind::kDraw:
      return OnDraw(event);
    case Kind::kCopy:
      return OnCopy(event);
    case Kind::kSwap:
      OnSwap(event);
      return true;
  }
  return false;
}

}  // namespace

void StartLiveCommands(plume::RenderDevice* device, rex::memory::Memory* memory,
                       const std::filesystem::path& cache_folder) {
  g_memory = memory;
  g_watcher = std::make_unique<PhysicalWriteWatcher>(memory);
  g_renderer = std::make_unique<native_gpu::Renderer>();
  if (!g_renderer->Initialize(device, memory->TranslatePhysical(0), FindContainer, g_watcher.get())) {
    REXLOG_ERROR("native_gpu: renderer initialization failed");
    g_renderer.reset();
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(cache_folder, error);
  g_renderer->LoadPipelineCache((cache_folder / "native_pipelines.bin").string());
  std::thread(RendererThread).detach();
  using SetSink = void (*)(rex::graphics::NativeCommandSink, void*);
  SetSink set = nullptr;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        if (!info->dlpi_name || !std::strstr(info->dlpi_name, "rexgpu")) return 0;
        if (void* handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD)) {
          *static_cast<SetSink*>(data) = reinterpret_cast<SetSink>(dlsym(handle, "rex_gpu_set_native_command_sink"));
          dlclose(handle);
        }
        return *static_cast<SetSink*>(data) ? 1 : 0;
      },
      &set);
  if (!set) {
    REXLOG_ERROR("native_gpu: the GPU plugin has no command sink");
    return;
  }
  set(&OnCommand, nullptr);
  REXLOG_INFO("native_gpu: rendering from the command stream, {} shaders", g_shaderCacheEntryCount);
}
