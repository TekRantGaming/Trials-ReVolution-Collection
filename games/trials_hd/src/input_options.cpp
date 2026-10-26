// Controller options set from the Trials The Recompilation Collection launcher (deadzone, vibration and button
// remap, the same in all three games), and the controller's part in returning to the launcher and in the game
// window.
//
// sub_82124678 is the title's XInputGetState (r3 user, r4 XINPUT_STATE; it calls XamInputGetState with flags 1),
// sub_82124670 is XamInputGetState itself (r3 user, r4 flags, r5 XINPUT_STATE) and sub_82124688 its XInputSetState
// (r3 user, r4 XINPUT_VIBRATION). After the game's own read, the state is changed here:
//   trials_remap_<button>  the button (or trigger) a physical button acts as
//   trials_deadzone        percent of each stick's travel that is ignored
//   trials_vibration       rumble strength in percent (the game's own values are put back after the call)
// With every option at its default the state is left exactly as the game read it.
//
// XINPUT_STATE (big endian): +0 packet number, +4 buttons, +6 left trigger,
// +7 right trigger, +8/+10 left stick x/y, +12/+14 right stick x/y.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(trials_deadzone, 0, "Collection", "Stick deadzone in percent");
REXCVAR_DEFINE_INT32(trials_vibration, 100, "Collection", "Vibration strength in percent");
REXCVAR_DEFINE_STRING(trials_remap_a, "a", "Collection", "A button acts as");
REXCVAR_DEFINE_STRING(trials_remap_b, "b", "Collection", "B button acts as");
REXCVAR_DEFINE_STRING(trials_remap_x, "x", "Collection", "X button acts as");
REXCVAR_DEFINE_STRING(trials_remap_y, "y", "Collection", "Y button acts as");
REXCVAR_DEFINE_STRING(trials_remap_lb, "lb", "Collection", "Left bumper acts as");
REXCVAR_DEFINE_STRING(trials_remap_rb, "rb", "Collection", "Right bumper acts as");
REXCVAR_DEFINE_STRING(trials_remap_lt, "lt", "Collection", "Left trigger acts as");
REXCVAR_DEFINE_STRING(trials_remap_rt, "rt", "Collection", "Right trigger acts as");
REXCVAR_DEFINE_STRING(trials_remap_start, "start", "Collection", "Start acts as");
REXCVAR_DEFINE_STRING(trials_remap_back, "back", "Collection", "Back acts as");
REXCVAR_DEFINE_STRING(trials_remap_ls, "ls", "Collection", "Left stick click acts as");
REXCVAR_DEFINE_STRING(trials_remap_rs, "rs", "Collection", "Right stick click acts as");

namespace trials {

namespace {

uint16_t Load16(const uint8_t* base, uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
void Store16(uint8_t* base, uint32_t a, uint16_t v) {
  base[a] = uint8_t(v >> 8);
  base[a + 1] = uint8_t(v);
}

struct Button {
  const char* name;
  uint16_t mask;  // 0 for the triggers
  int trigger;    // -1, or 0 left / 1 right
};
const Button kButtons[] = {
    {"a", 0x1000, -1},     {"b", 0x2000, -1},    {"x", 0x4000, -1},  {"y", 0x8000, -1},
    {"lb", 0x0100, -1},    {"rb", 0x0200, -1},   {"lt", 0, 0},       {"rt", 0, 1},
    {"start", 0x0010, -1}, {"back", 0x0020, -1}, {"ls", 0x0040, -1}, {"rs", 0x0080, -1},
};

const std::string& Remap(int i) {
  switch (i) {
    case 0: return REXCVAR_GET(trials_remap_a);
    case 1: return REXCVAR_GET(trials_remap_b);
    case 2: return REXCVAR_GET(trials_remap_x);
    case 3: return REXCVAR_GET(trials_remap_y);
    case 4: return REXCVAR_GET(trials_remap_lb);
    case 5: return REXCVAR_GET(trials_remap_rb);
    case 6: return REXCVAR_GET(trials_remap_lt);
    case 7: return REXCVAR_GET(trials_remap_rt);
    case 8: return REXCVAR_GET(trials_remap_start);
    case 9: return REXCVAR_GET(trials_remap_back);
    case 10: return REXCVAR_GET(trials_remap_ls);
    default: return REXCVAR_GET(trials_remap_rs);
  }
}

void ApplyRemap(uint8_t* base, uint32_t state) {
  bool identity = true;
  for (int i = 0; i < 12; ++i) identity = identity && Remap(i) == kButtons[i].name;
  if (identity) return;
  const uint16_t in_buttons = Load16(base, state + 4);
  const uint8_t in_trig[2] = {base[state + 6], base[state + 7]};
  uint16_t out_buttons = in_buttons & 0x000F;  // the D pad is not remapped
  uint8_t out_trig[2] = {0, 0};
  for (int i = 0; i < 12; ++i) {
    const Button& src = kButtons[i];
    const uint8_t amount = src.trigger >= 0 ? in_trig[src.trigger] : ((in_buttons & src.mask) ? 255 : 0);
    if (!amount) continue;
    for (const Button& dst : kButtons) {
      if (Remap(i) != dst.name) continue;
      if (dst.trigger >= 0) {
        out_trig[dst.trigger] = std::max(out_trig[dst.trigger], amount);
      } else if (amount > 30) {  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
        out_buttons |= dst.mask;
      }
    }
  }
  Store16(base, state + 4, out_buttons);
  base[state + 6] = out_trig[0];
  base[state + 7] = out_trig[1];
}

// The same remap for a menu key (XINPUT_KEYSTROKE VirtualKey at +0). False when the button is set to nothing.
bool RemapKeystroke(uint8_t* base, uint32_t key) {
  static const uint16_t kVk[12] = {0x5800, 0x5801, 0x5802, 0x5803, 0x5805, 0x5804,
                                   0x5806, 0x5807, 0x5814, 0x5815, 0x5816, 0x5817};
  const uint16_t vk = Load16(base, key);
  for (int i = 0; i < 12; ++i) {
    if (kVk[i] != vk) continue;
    if (Remap(i) == kButtons[i].name) return true;
    for (int j = 0; j < 12; ++j) {
      if (Remap(i) == kButtons[j].name) {
        Store16(base, key, kVk[j]);
        return true;
      }
    }
    return false;  // "none"
  }
  return true;  // the D pad and the sticks' directions are not remapped
}

void ApplyDeadzone(uint8_t* base, uint32_t x_addr, int percent) {
  if (percent <= 0) return;
  const float x = int16_t(Load16(base, x_addr)), y = int16_t(Load16(base, x_addr + 2));
  const float mag = std::sqrt(x * x + y * y) / 32767.0f, dz = std::min(percent, 90) / 100.0f;
  float scale = 0.0f;
  if (mag > dz) scale = std::min(1.0f, (mag - dz) / (1.0f - dz)) / mag;
  Store16(base, x_addr, uint16_t(int16_t(std::clamp(x * scale, -32768.0f, 32767.0f))));
  Store16(base, x_addr + 2, uint16_t(int16_t(std::clamp(y * scale, -32768.0f, 32767.0f))));
}

}  // namespace

void ReturnToLauncherTick(uint32_t user, bool connected, uint16_t buttons);  // return_to_launcher.cpp
void ReturnToLauncherKey(uint32_t user, uint16_t vk, uint16_t flags);       // return_to_launcher.cpp
void GameWindowTick(bool pad_used);                                         // game_window.cpp
bool PadChanged(uint32_t user, const uint8_t* state16);                     // game_window.cpp

// After the game's own read of one controller slot.
void AfterGetState(uint8_t* base, uint32_t user, uint32_t state, bool ok) {
  const bool connected = ok && state;
  ReturnToLauncherTick(user, connected, connected ? Load16(base, state + 4) : 0);
  GameWindowTick(connected && PadChanged(user, base + state));  // front, cursor
  if (!connected) return;  // no pad in this slot
#if defined(TH_DEV_TOOLS) || defined(TE_DEV_TOOLS) || defined(TF_DEV_TOOLS)
  const uint16_t in_buttons = Load16(base, state + 4), in_lx = Load16(base, state + 8);
#endif
  ApplyRemap(base, state);
  ApplyDeadzone(base, state + 8, REXCVAR_GET(trials_deadzone));
  ApplyDeadzone(base, state + 12, REXCVAR_GET(trials_deadzone));
#if defined(TH_DEV_TOOLS) || defined(TE_DEV_TOOLS) || defined(TF_DEV_TOOLS)
  // Developer aid: what the options changed, when the input changes.
  static uint32_t last = 0xFFFFFFFF;
  const uint32_t now = uint32_t(in_buttons) << 16 | in_lx;
  if (now != last) {
    last = now;
    REXLOG_INFO("Trials dev input: buttons {:04X} -> {:04X}, triggers {} {}, left x {} -> {}", in_buttons,
                Load16(base, state + 4), base[state + 6], base[state + 7], int16_t(in_lx),
                int16_t(Load16(base, state + 8)));
  }
#endif
}

}  // namespace trials

#if defined(TH_DEV_TOOLS)
namespace th {
bool DevPadCaps(PPCContext& ctx, uint8_t* base);   // dev/dev_pad.cpp: the scripted test controller
bool DevPadState(PPCContext& ctx, uint8_t* base);
bool DevPadKeystroke(PPCContext& ctx, uint8_t* base);
}  // namespace th
REX_EXTERN(__imp__sub_82124668);
REX_HOOK_RAW(sub_82124668) {
  if (th::DevPadCaps(ctx, base)) return;
  __imp__sub_82124668(ctx, base);
}
#endif

REX_EXTERN(__imp__sub_82124678);
REX_HOOK_RAW(sub_82124678) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
#if defined(TH_DEV_TOOLS)
  if (!th::DevPadState(ctx, base))
#endif
    __imp__sub_82124678(ctx, base);
  trials::AfterGetState(base, user, state, ctx.r3.u32 == 0);
}

REX_EXTERN(__imp__sub_82124670);
REX_HOOK_RAW(sub_82124670) {
  const uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
#if defined(TH_DEV_TOOLS)
  const uint64_t flags = ctx.r4.u64;
  ctx.r4.u64 = ctx.r5.u64;
  const bool dev = th::DevPadState(ctx, base);
  if (!dev) ctx.r4.u64 = flags;
  if (!dev)
#endif
    __imp__sub_82124670(ctx, base);
  trials::AfterGetState(base, user, state, ctx.r3.u32 == 0);
}

REX_EXTERN(__imp__sub_82124688);
REX_HOOK_RAW(sub_82124688) {
  const uint32_t vib = ctx.r4.u32;
  const int percent = REXCVAR_GET(trials_vibration);
  if (!vib || percent == 100) {
    __imp__sub_82124688(ctx, base);
    return;
  }
  const uint16_t left = uint16_t(base[vib] << 8 | base[vib + 1]), right = uint16_t(base[vib + 2] << 8 | base[vib + 3]);
  const float k = std::clamp(percent, 0, 100) / 100.0f;
  auto store = [base](uint32_t a, uint16_t v) {
    base[a] = uint8_t(v >> 8);
    base[a + 1] = uint8_t(v);
  };
  store(vib, uint16_t(left * k));
  store(vib + 2, uint16_t(right * k));
#if defined(TH_DEV_TOOLS) || defined(TE_DEV_TOOLS) || defined(TF_DEV_TOOLS)
  if (left || right) REXLOG_INFO("Trials dev vibration: {} {} -> {} {}", left, right, uint16_t(left * k), uint16_t(right * k));
#endif
  __imp__sub_82124688(ctx, base);
  store(vib, left);  // the game's own values back
  store(vib + 2, right);
}

// The menus read XInputGetKeystroke (sub_82124750: r3 user or 255, r4 flags, r5 keystroke), so menu keys follow the
// button remapping too.
REX_EXTERN(__imp__sub_82124750);
REX_HOOK_RAW(sub_82124750) {
  const uint32_t key = ctx.r5.u32;
#if defined(TH_DEV_TOOLS)
  if (!th::DevPadKeystroke(ctx, base))
#endif
    __imp__sub_82124750(ctx, base);
  const bool got = ctx.r3.u32 == 0 && key;
  // Back and Start held in a menu return to the launcher too (the key before remapping, and its user index at +6).
  trials::ReturnToLauncherKey(got ? base[key + 6] : 0, got ? uint16_t(base[key] << 8 | base[key + 1]) : 0,
                              got ? uint16_t(base[key + 4] << 8 | base[key + 5]) : 0);
  trials::GameWindowTick(got);  // a menu key from the controller hides the cursor too
  if (got && !trials::RemapKeystroke(base, key)) ctx.r3.u64 = 0x10D2;  // ERROR_EMPTY
}
