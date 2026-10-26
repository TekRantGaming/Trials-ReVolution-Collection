// Trials HD v0.0.1.2: run the original updates independently of real rendering, so the game draws at any frame rate
// while its physics keeps the original fixed 60 updates a second; above 60, each drawn frame places the camera and
// models between the last two updates.
//
// From Trials HD Recompiled by snufflyyy (MIT, see THIRD_PARTY_NOTICES.md): src/fps_unlock.cpp. Changes for the
// collection: the rate is the launcher's trials_frame_rate (0 is no limit), the unlocked loop runs only when that is
// not 60 (at 60 the original loop runs untouched), VSync's refresh rate cap reads the presenter's own setting, and
// indirect calls resolve through the SDK's function table.
#include <array>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include "flat_hash_map.h"
#include "frame_clock.h"
#include "render_pose.h"

REXCVAR_DEFINE_INT32(trials_frame_rate, 60, "Collection",
                     "Frame rate cap (60 is the original, 0 is no cap); the physics stays at 60 updates a second");
REXCVAR_DEFINE_BOOL(log_simulation_rate, false, "Display",
                    "Log fixed updates and real render frames once per second");

REX_EXTERN(__imp__sub_82664F98);
REX_EXTERN(sub_82668BA8);
REX_EXTERN(sub_825CB980);
REX_EXTERN(sub_82124BF0);
REX_EXTERN(sub_82124660);
REX_EXTERN(sub_822F65A8);
REX_EXTERN(__imp__sub_825EBF48);
REX_EXTERN(__imp__sub_825E9220);
REX_EXTERN(__imp__sub_825E7FC0);
REX_EXTERN(__imp__sub_8210CAA8);
REX_EXTERN(__imp__sub_8210CBE8);

namespace trials_hd {
// With the presenter's VSync on (tearing not allowed), the refresh rate of the main display; 0 with VSync off.
float VsyncRefreshRate() {
  // The setting belongs to the GPU plugin, so it is read by name.
  if (rex::cvar::GetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing") != "false") return 0.0f;
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
    return float(mode.dmDisplayFrequency);
#endif
  return 60.0f;
}
}  // namespace trials_hd

namespace {
constexpr uint32_t kApplication = 0x827683A0;
constexpr uint32_t kTickCount = 0x8276A400;
constexpr uint32_t kRenderCount = 0x8276A404;
constexpr uint32_t kFloatConstants = 0x820BD300;
constexpr uint32_t kScrollPhase = 0x8276AC60;
constexpr uint32_t kScrollInitialized = 0x8276AC68;

float Float(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<float>(base + address);
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  rex::memory::store_and_swap<float>(base + address, value);
}

std::atomic_bool g_unlocked_loop{false};
bool Unlocked() { return g_unlocked_loop.load(); }
std::atomic<float> g_render_alpha{1.f};
thread_local uint32_t g_draw_renderer = 0;
thread_local uint32_t g_draw_frame = 0, g_pose_log_frames = 0, g_pose_log_second = 0;
thread_local std::array<uint32_t, 4> g_pose_scene{};
thread_local trials_hd::FlatHashMap<uint64_t, trials_hd::PoseHistory> g_model_poses;
thread_local trials_hd::FlatHashMap<uint64_t, trials_hd::PoseTracks> g_camera_poses;
thread_local bool g_logged_rigid = false, g_logged_skin = false;

uint32_t Word(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(base + address);
}
trials_hd::RenderMatrix Matrix(uint8_t* base, uint32_t address) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  trials_hd::RenderMatrix matrix;
  const __m128i mask = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const uint8_t* ptr = base + address;
  for (int i = 0; i < 4; ++i) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i * 16));
    v = _mm_shuffle_epi8(v, mask);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&matrix[i * 4]), v);
  }
  return matrix;
#else
  trials_hd::RenderMatrix matrix;
  for (uint32_t i = 0; i < 16; ++i) matrix[i] = Float(base, address + i * 4);
  return matrix;
#endif
}
void StoreMatrix(uint8_t* base, uint32_t address, const trials_hd::RenderMatrix& matrix) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  const __m128i mask = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  uint8_t* ptr = base + address;
  for (int i = 0; i < 4; ++i) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&matrix[i * 4]));
    v = _mm_shuffle_epi8(v, mask);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + i * 16), v);
  }
#else
  for (uint32_t i = 0; i < 16; ++i) StoreFloat(base, address + i * 4, matrix[i]);
#endif
}

// The camera part of DrawInterpolated is the same for every node drawn with
// one renderer and camera in a frame: computed once and reused while the
// game's camera matrices (restored after each draw) and the tick / alpha stay
// the same.
struct CameraUploadCache {
  bool valid = false;
  uint32_t renderer = 0, camera_id = 0, pass = 0, tick = 0, alpha_bits = 0;
  // renderer + 144: view, view x projection, projection, camera, +400, +464.
  std::array<uint8_t, 384> raw{};
  std::array<uint8_t, 384> uploaded{};
  // camera + 48.
  std::array<uint8_t, 12> camera_position{};
};
thread_local CameraUploadCache g_camera_upload;

void DrawInterpolated(PPCContext& ctx, uint8_t* base, void (*draw)(PPCContext&, uint8_t*)) {
  const auto renderer = ctx.r3.u32, node = ctx.r4.u32;
  const float alpha = g_render_alpha.load();
  if (!Unlocked() || renderer != g_draw_renderer || alpha >= 1) {
    draw(ctx, base);
    return;
  }
  const auto count = Word(base, renderer + 1064);
  if (count == 0 || count > 30) {
    draw(ctx, base);
    return;
  }
  std::array<uint32_t, 30> pointers{};
  for (uint32_t i = 0; i < count; ++i) {
    pointers[i] = Word(base, renderer + 944 + i * 4);
    if (!pointers[i]) {
      draw(ctx, base);
      return;
    }
  }
  const auto tick = Word(base, kTickCount);
  const auto camera_id = Word(base, renderer + 28);
  if (!camera_id) {
    draw(ctx, base);
    return;
  }
  // The pose log (log_simulation_rate, a few frames per second) needs the full
  // computation.
  const bool logging = REXCVAR_GET(log_simulation_rate) && g_pose_log_frames <= 4;
  uint32_t alpha_bits;
  std::memcpy(&alpha_bits, &alpha, sizeof(alpha_bits));
  CameraUploadCache& cache = g_camera_upload;
  const uint32_t pass = Word(base, renderer + 624);
  const bool cached = !logging && cache.valid && cache.renderer == renderer &&
                      cache.camera_id == camera_id && cache.pass == pass && cache.tick == tick &&
                      cache.alpha_bits == alpha_bits &&
                      !std::memcmp(cache.raw.data(), base + renderer + 144, cache.raw.size());
  trials_hd::RenderMatrix projection{}, camera{}, camera_pose{};
  uint64_t camera_key = 0;
  if (!cached) {
    std::memcpy(cache.raw.data(), base + renderer + 144, cache.raw.size());
    projection = Matrix(base, renderer + 272);
    const auto raw_view = Matrix(base, renderer + 144);
    camera_key = trials_hd::CameraPoseKey(camera_id, pass, raw_view, projection);
    camera = trials_hd::InverseView(raw_view);
    // Orthographic passes (shadow maps) keep their tick camera: the game samples them through
    // light matrices computed at tick time. Their models are still interpolated below.
    camera_pose = projection[11] != 0 ? g_camera_poses[camera_key].Sample(tick, camera, alpha)
                                      : camera;
  }
  trials_hd::ScopedRenderUpload upload(base, renderer, camera_id, ctx.r1.u32);
  if (cached) {
    std::memcpy(base + renderer + 144, cache.uploaded.data(), cache.uploaded.size());
    std::memcpy(base + camera_id + 48, cache.camera_position.data(), cache.camera_position.size());
  } else {
    for (uint32_t i = 0; i < 3; ++i) StoreFloat(base, camera_id + 48 + i * 4, camera_pose[12 + i]);
    const auto view = trials_hd::InverseView(camera_pose);
    StoreMatrix(base, renderer + 144, view);
    StoreMatrix(base, renderer + 208, trials_hd::Multiply(view, projection));
    StoreMatrix(base, renderer + 336, camera_pose);
    StoreMatrix(base, renderer + 400, trials_hd::Multiply(Matrix(base, renderer + 464), camera_pose));
    std::memcpy(cache.uploaded.data(), base + renderer + 144, cache.uploaded.size());
    std::memcpy(cache.camera_position.data(), base + camera_id + 48, cache.camera_position.size());
    cache.renderer = renderer;
    cache.camera_id = camera_id;
    cache.pass = pass;
    cache.tick = tick;
    cache.alpha_bits = alpha_bits;
    cache.valid = true;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const auto current = Matrix(base, pointers[i]);
    const auto key = (uint64_t(node) << 32) | pointers[i];
    const auto pose = g_model_poses[key].Sample(tick, current, alpha);
    const auto scratch = upload.scratch() + i * 64;
    StoreMatrix(base, scratch, pose);
    rex::memory::store_and_swap<uint32_t>(base + renderer + 944 + i * 4, scratch);
    auto& logged = count > 1 ? g_logged_skin : g_logged_rigid;
    if (logging && projection[11] != 0 && !logged && pose != current) {
      uint32_t component = 0;
      for (uint32_t j = 1; j < 16; ++j)
        if (std::abs(pose[j] - current[j]) > std::abs(pose[component] - current[component])) component = j;
      REXLOG_INFO("render pose: frame={} tick={} alpha={:.6f} node={:08X} matrices={} component={} raw={:.7f} rendered={:.7f} camera={:08X} camera_view={:016X} camera_raw_x={:.7f} camera_rendered_x={:.7f}",
                  g_draw_frame, tick, alpha, node, count, component, current[component], pose[component], camera_id, camera_key, camera[12], camera_pose[12]);
      logged = true;
    }
  }
  draw(ctx, base);
}

uint32_t Game(uint8_t* base) {
  return rex::memory::load_and_swap<uint32_t>(base + kApplication);
}

inline PPCFunc* LookupDirectFunction(uint8_t* base, uint32_t target) {
  (void)base;
  return rex::runtime::ResolveIndirectFunction(target);
}

void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot,
                 uint32_t argument, uint32_t return_address) {
  if (!object) return;
  const auto table = rex::memory::load_and_swap<uint32_t>(base + object);
  const auto function = rex::memory::load_and_swap<uint32_t>(base + table + slot);
  ctx.r3.u64 = object;
  ctx.r4.u64 = argument;
  ctx.lr = return_address;
  LookupDirectFunction(base, function)(ctx, base);
}

bool Running(uint8_t* base) {
  return rex::memory::load<uint8_t>(base + Game(base) + 4) != 0;
}

uint32_t Scene(uint8_t* base) {
  return rex::memory::load_and_swap<uint32_t>(base + Game(base) + 20);
}

// The two +20 workers also advance animation/particle time (825F2118).
// Keep all four update workers at fixed rate; only +28 submits real drawing.
void Dispatch(PPCContext& ctx, uint8_t* base, bool render) {
  constexpr uint32_t updates[] = {0x827683B8, 0x827683C4, 0x827683BC, 0x827683C0};
  constexpr uint32_t update_done[] = {0x827683CC, 0x827683D8, 0x827683D0, 0x827683D4};
  constexpr uint32_t renders[] = {0x827683B4};
  constexpr uint32_t render_done[] = {0x827683C8};
  auto call = [&](uint32_t global, uint32_t slot) {
    CallVirtual(ctx, base, rex::memory::load_and_swap<uint32_t>(base + global),
                slot, 0, 0x82663808);
  };
  if (render) {
    for (auto global : renders) call(global, 4);
    for (auto global : render_done) call(global, 0);
    const auto count = rex::memory::load_and_swap<uint32_t>(base + kRenderCount);
    rex::memory::store_and_swap<uint32_t>(base + kRenderCount, count + 1);
  } else {
    for (auto global : updates) call(global, 4);
    for (auto global : update_done) call(global, 0);
  }
}

// Temporal work embedded in 825EBF48/825E7FC0 must also run between low-FPS renders.
void TickVisuals(PPCContext& ctx, uint8_t* base) {
  const auto scene = Scene(base);
  if (!rex::memory::load_and_swap<uint32_t>(base + scene + 12)) return;
  const auto world = rex::memory::load_and_swap<uint32_t>(base + scene + 20);
  const auto mode = rex::memory::load_and_swap<uint32_t>(base + world + 115564);
  if (mode != 3 && !rex::memory::load_and_swap<uint32_t>(base + world + 115448) &&
      rex::memory::load_and_swap<uint32_t>(base + world + 115064) != 2) {
    const float amount = Float(base, kFloatConstants + 224);
    const auto filtered = world + 248760;
    StoreFloat(base, filtered, float(std::fma(double(Float(base, filtered)),
               double(Float(base, kFloatConstants + 784)), double(amount))));
    ctx.r3.u64 = world;
    ctx.f1.f64 = amount;
    ctx.lr = 0x825EC160;
    __imp__sub_825E9220(ctx, base);
  }
  if (mode >= 1 && mode <= 3) {
    auto flags = rex::memory::load_and_swap<uint32_t>(base + kScrollInitialized);
    if (!(flags & 1)) {
      StoreFloat(base, kScrollPhase, 0);
      StoreFloat(base, kScrollPhase + 4, 0);
      rex::memory::store_and_swap<uint32_t>(base + kScrollInitialized, flags | 1);
      ctx.r3.u64 = 0x8269D8C8;
      ctx.lr = 0x825E80E8;
      sub_822F65A8(ctx, base);  // Preserve the guest's static-destructor registration.
    }
    for (uint32_t i = 0; i < 2; ++i) {
      const auto address = kScrollPhase + i * 4;
      StoreFloat(base, address, trials_hd::AdvanceScrollPhase(
          Float(base, address), Float(base, kFloatConstants + 264 + i * 4)));
    }
  }
}

void Tick(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = Scene(base);
  ctx.lr = 0x826650B8;
  sub_82668BA8(ctx, base);
  auto scene = Scene(base);
  auto state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 36, 0, 0x826650E4);
  scene = Scene(base);
  state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 36, 1, 0x82665110);
  scene = Scene(base);
  state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 40, 0, 0x82665138);
  ctx.r3.u64 = rex::memory::load_and_swap<uint32_t>(base + scene + 96);
  ctx.lr = 0x82665140;
  sub_825CB980(ctx, base);
}
}  // namespace

// Preserve fixed visual state while the original function submits every real frame.
extern "C" REX_FUNC(sub_825EBF48) {
  if (!Unlocked()) {
    __imp__sub_825EBF48(ctx, base);
    return;
  }
  const auto world = ctx.r3.u32;
  const std::array<uint32_t, 4> scene = {world, Word(base, world + 12),
                                      Word(base, world + 115072), Word(base, world + 115564)};
  if (scene != g_pose_scene || g_render_alpha.load() >= 1) {
    g_model_poses.clear();
    g_camera_poses.clear();
    g_camera_upload.valid = false;
    g_pose_scene = scene;
    g_pose_log_frames = 0;
  }
  const auto tick = Word(base, kTickCount);
  std::erase_if(g_model_poses, [tick](const auto& entry) { return uint32_t(tick - entry.second.tick()) > 2; });
  g_draw_frame = Word(base, kRenderCount);
  if (tick / 60 != g_pose_log_second) {
    g_pose_log_second = tick / 60;
    g_pose_log_frames = 0;
  }
  ++g_pose_log_frames;
  g_logged_rigid = g_logged_skin = false;
  g_draw_renderer = Word(base, world + 232);
  const auto filtered = world + 248760;
  const float value = Float(base, filtered);
  __imp__sub_825EBF48(ctx, base);
  g_draw_renderer = 0;
  StoreFloat(base, filtered, value);
}

// The normal and batched mesh paths upload all rigid/skinned matrices through 8210D468.
extern "C" REX_FUNC(sub_8210CAA8) {
  DrawInterpolated(ctx, base, __imp__sub_8210CAA8);
}
extern "C" REX_FUNC(sub_8210CBE8) {
  DrawInterpolated(ctx, base, __imp__sub_8210CBE8);
}

extern "C" REX_FUNC(sub_825E9220) {
  // The other caller uses alpha=1 for initialization; do not suppress it.
  if (!Unlocked() || ctx.lr != 0x825EC160)
    __imp__sub_825E9220(ctx, base);
}

extern "C" REX_FUNC(sub_825E7FC0) {
  if (!Unlocked() ||
      !(rex::memory::load_and_swap<uint32_t>(base + kScrollInitialized) & 1)) {
    __imp__sub_825E7FC0(ctx, base);
    return;
  }
  float phases[2];
  for (uint32_t i = 0; i < 2; ++i) {
    phases[i] = Float(base, kScrollPhase + i * 4);
    // Cancel its legacy per-render advance so drawing reads the fixed-tick phase.
    StoreFloat(base, kScrollPhase + i * 4,
               phases[i] - Float(base, kFloatConstants + 264 + i * 4));
  }
  __imp__sub_825E7FC0(ctx, base);
  for (uint32_t i = 0; i < 2; ++i) StoreFloat(base, kScrollPhase + i * 4, phases[i]);
}

inline void AccurateWaitUntil(std::chrono::steady_clock::time_point deadline) {
  auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return;

  // Phase 1: Coarse sleep when remaining time > 1.5ms.
  // Sleep until 1ms before deadline to ensure we never oversleep past the deadline due to OS scheduler granularity.
  while (deadline - now > std::chrono::microseconds(1500)) {
    std::this_thread::sleep_until(deadline - std::chrono::microseconds(1000));
    now = std::chrono::steady_clock::now();
  }

  // Phase 2: Hybrid yield / _mm_pause spin-wait for sub-millisecond precision.
  while (now < deadline) {
    if (deadline - now > std::chrono::microseconds(200)) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
      for (int i = 0; i < 8; ++i) {
        _mm_pause();
      }
#endif
      std::this_thread::yield();
    } else {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
      _mm_pause();
#else
      std::this_thread::yield();
#endif
    }
    now = std::chrono::steady_clock::now();
  }
}

// Strong override: generated code and address registration both resolve here.
extern "C" REX_FUNC(sub_82664F98) {
  // Latch the applied startup mode, not fps_unlock's pending restart-required value.
  g_unlocked_loop.store(REXCVAR_GET(trials_frame_rate) != 60);
  if (!Unlocked()) {
    __imp__sub_82664F98(ctx, base);
    return;
  }
  const PPCContext saved = ctx;
  // Reserve the same ABI frame: callees use r1+80.. for their parameter area.
  ctx.r1.u32 -= 240;
  rex::memory::store_and_swap<uint32_t>(base + ctx.r1.u32, saved.r1.u32);
  trials_hd::FrameClock clock;
  auto previous = std::chrono::steady_clock::now();
  auto report_time = previous;
  auto next_render = previous;
  auto query_rate = []() -> double {
    double r = std::max(REXCVAR_GET(trials_frame_rate), 0);
    const double vsync_refresh = trials_hd::VsyncRefreshRate();
    if (vsync_refresh > 0 && (r == 0 || r > vsync_refresh)) {
      r = vsync_refresh;
    }
    return r;
  };
  double rate = query_rate();
  double last_rate = -1;
  uint32_t rate_counter = 0;
  uint32_t ticks = 0, renders = 0;
  // Where the loop's time goes (log_simulation_rate): pacing wait, fixed
  // updates, render dispatch, then the two calls that end the frame.
  using Seconds = std::chrono::duration<double>;
  Seconds t_wait{}, t_update{}, t_render{}, t_present{}, t_end{};
  uint32_t last_state_table = 0;
  bool initialized = false;
  REXLOG_INFO("FPS unlock: real rendering, original 16,666 us fixed updates");
  while (Running(base)) {
    // The SDK's cap skips presentation, not rendering. Pace the real work here.
    // With VSync, the presenter shows only the newest frame at each refresh, so
    // rendering faster than the display would show an uneven subset of the
    // interpolated frames: cap at the refresh rate too.
    if (++rate_counter >= 32) {
      rate_counter = 0;
      rate = query_rate();
    }
    if (rate != last_rate) {
      next_render = std::chrono::steady_clock::now();
      last_rate = rate;
      REXLOG_INFO("FPS unlock: rendering {}", rate > 0 ? fmt::format("at up to {:.2f} FPS", rate) : "uncapped");
    }
    const auto wait_start = std::chrono::steady_clock::now();
    if (rate > 0 && initialized) {
      AccurateWaitUntil(std::min(next_render, previous + clock.UntilNextStep()));
    }
    const auto now = std::chrono::steady_clock::now();
    t_wait += now - wait_start;
    const auto steps = clock.Advance(now - previous);
    previous = now;
    for (uint32_t i = 0; i < steps && Running(base); ++i) {
      Tick(ctx, base);
      Dispatch(ctx, base, false);
      if (Running(base)) TickVisuals(ctx, base);
      const auto state = rex::memory::load_and_swap<uint32_t>(base + Scene(base) + 12);
      const auto table = state ? rex::memory::load_and_swap<uint32_t>(base + state) : 0;
      if (REXCVAR_GET(log_simulation_rate) && table && table != last_state_table) {
        REXLOG_INFO("FPS state table {:08X}: update {:08X}, jobs {:08X}, prepare {:08X}, draw {:08X}",
                    table, rex::memory::load_and_swap<uint32_t>(base + table + 16),
                    rex::memory::load_and_swap<uint32_t>(base + table + 20),
                    rex::memory::load_and_swap<uint32_t>(base + table + 24),
                    rex::memory::load_and_swap<uint32_t>(base + table + 28));
        last_state_table = table;
      }
      auto count = rex::memory::load_and_swap<uint32_t>(base + kTickCount);
      rex::memory::store_and_swap<uint32_t>(base + kTickCount, count + 1);
      ++ticks;
      initialized = true;
    }
    if (!Running(base)) break;
    const auto render_start = std::chrono::steady_clock::now();
    t_update += render_start - now;
    if (initialized && (rate == 0 || now >= next_render)) {
      // At or below the 60 Hz tick rate, every frame shows a whole tick as the original did.
      g_render_alpha.store(rate == 0 || rate > 61 ? clock.InterpolationFraction() : 1.f);
      Dispatch(ctx, base, true);
      ++renders;
      if (rate > 0) {
        const auto period = std::chrono::nanoseconds(int64_t(1e9 / rate));
        next_render = now - next_render > period ? now + period : next_render + period;
      }
    }
    const auto present_start = std::chrono::steady_clock::now();
    t_render += present_start - render_start;
    ctx.r3.u64 = ctx.r1.u32 + 96;
    ctx.lr = 0x8266518C;
    sub_82124BF0(ctx, base);
    const auto end_start = std::chrono::steady_clock::now();
    t_present += end_start - present_start;
    if (now - report_time >= std::chrono::seconds(1)) {
      const double seconds = std::chrono::duration<double>(now - report_time).count();
      if (REXCVAR_GET(log_simulation_rate)) {
        REXLOG_INFO("fixed updates/sec: {:.1f} real renders/sec: {:.1f}",
                    ticks / seconds, renders / seconds);
        const double ms = 1000.0 / std::max<uint32_t>(renders, 1);
        REXLOG_INFO("frame loop ms/render: wait {:.2f} update {:.2f} render {:.2f} "
                    "end1 {:.2f} end2 {:.2f}",
                    t_wait.count() * ms, t_update.count() * ms, t_render.count() * ms,
                    t_present.count() * ms, t_end.count() * ms);
      }
      t_wait = t_update = t_render = t_present = t_end = {};
      ticks = renders = 0;
      report_time = now;
    }
    ctx.r3.u64 = 0;
    ctx.lr = 0x826651A0;
    sub_82124660(ctx, base);
    t_end += std::chrono::steady_clock::now() - end_start;
  }
  g_unlocked_loop.store(false);
  ctx = saved;
}
