// Back to the collection launcher: holding Back and Start
// together on the controller for a second, or holding Esc for a second and a
// half, closes the game; the launcher comes back when the game exits. Only
// when the launcher started the game (it passes --trials_from_launcher=true).
// Called from the XInputGetState hook in input_options.cpp, which the game
// polls many times a second. On Linux the game's window is SDL's: Esc comes from SDL's keyboard state and the
// game is closed with an SDL quit event.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(trials_from_launcher, false, "Collection",
                    "Started by the collection launcher: hold Back and Start, or Esc, to return to it");

namespace trials {
namespace {

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
BOOL CALLBACK CloseOwnWindow(HWND hwnd, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
  return TRUE;
}

bool OwnWindowFocused() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

bool EscHeld() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && OwnWindowFocused(); }
void CloseGame() { EnumWindows(&CloseOwnWindow, 0); }
#else
bool EscHeld() {
  const bool* keys = SDL_GetKeyboardState(nullptr);
  return keys && keys[SDL_SCANCODE_ESCAPE] && SDL_GetKeyboardFocus();
}
void CloseGame() {
  SDL_Event e{};
  e.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&e);
}
#endif

// How long a condition has held, or zero when it is not held.
struct Held {
  bool down = false;
  Clock::time_point since;
  double Seconds(bool now_down) {
    const Clock::time_point now = Clock::now();
    if (now_down && !down) since = now;
    down = now_down;
    return down ? std::chrono::duration<double>(now - since).count() : 0.0;
  }
};

std::atomic<bool> g_closing{false};

void BackToLauncher(const char* how) {
  if (g_closing.exchange(true)) return;
  REXLOG_INFO("Trials: back to the launcher ({})", how);
  CloseGame();
}

}  // namespace

// `user` is the controller slot polled; `buttons` its physical buttons (before remapping), valid when `connected`.
void ReturnToLauncherTick(uint32_t user, bool connected, uint16_t buttons) {
  if (!REXCVAR_GET(trials_from_launcher) || g_closing) return;
  static Held pad[4], key;
  if (user < 4) {
    constexpr uint16_t kStartBack = 0x0010 | 0x0020;  // XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK
    if (pad[user].Seconds(connected && (buttons & kStartBack) == kStartBack) >= 1.0) BackToLauncher("controller");
  }
  if (user == 0 && key.Seconds(EscHeld()) >= 1.5) BackToLauncher("Esc");
}

// For games whose menus read XInputGetKeystroke (Trials HD): Back and Start as key events, so the hold also counts
// in menus that do not poll the controller state. `vk` is the key before remapping, `flags` its XINPUT_KEYSTROKE
// flags (1 down, 2 up, 4 repeat). Called on every keystroke poll, with vk 0 when there was no key.
void ReturnToLauncherKey(uint32_t user, uint16_t vk, uint16_t flags) {
  if (!REXCVAR_GET(trials_from_launcher) || g_closing) return;
  static uint16_t down[4];
  static Held held[4];
  if (vk && user < 4) {
    const uint16_t bit = vk == 0x5814 ? 0x0010 : vk == 0x5815 ? 0x0020 : 0;  // VK_PAD_START, VK_PAD_BACK
    if (bit && (flags & 1)) down[user] |= bit;
    if (bit && (flags & 2)) down[user] &= uint16_t(~bit);
  }
  for (uint32_t u = 0; u < 4; ++u) {
    if (held[u].Seconds(down[u] == 0x0030) >= 1.0) BackToLauncher("controller, menu keys");
  }
}

}  // namespace trials
