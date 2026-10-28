// Touch controls (the user's request, 2026-10-27: "I want all menu items clickable and tapping in levels shoots the
// ball in that direction"). The Switch's touch screen, and the mouse on the PC, become taps on the game.
//
// Zuma's framework is an early SexyAppFramework the Xbox 360 port cut down (a 39-slot Widget vtable, no RTTI names, no
// mouse handling left), so a tap goes the pad's way (input_script::Press, the touch controls' pad merged into player 1),
// on what the game itself keeps (docs/zuma-notes.md, "Touch controls"; found with zuma_diag_memdiff):
//   - the app is a static object at 0x823D13B8: +744 its WidgetManager, whose focus (+56) is the widget the pad drives;
//     +1620 the level's Board (vtable 0x82001220, built by sub_82077878 from sub_820C7F08), 0 outside a level;
//   - the Board's Gun at +600: +4 its angle in radians (the shot's direction is (sin a, cos a) on the screen, 0 down),
//     +8 / +12 its centre (int, the game's 640x480);
//   - a widget's x, y, width, height at +8 / +12 / +16 / +20 (Resize, sub_820DBF78); a menu's entries are focusable
//     widgets linked to their neighbours at +56 / +60 / +64 / +68 (the pad's directions; the main menu's handler,
//     sub_820A37F8, follows +64 / +68 for LEFT / RIGHT through WidgetManager::SetFocus, sub_820DE870).
// The game's 640x480 is drawn at 1.25x from (240, 60) of the 1280x720 screen (`memdiff3`: the frog's centre (327, 233)
// at (649, 351)).
// A tap:
//   - in a level (the Board has the focus): the Gun turned towards it, then A (the shot) when it ends; a held touch
//     turns the Gun after the finger without shooting, and the next tap then shoots where it points (LevelTouch);
//   - on a screen whose focused widget has neighbours (a menu): the entry under the tap focused (SetFocus), then A; a
//     tap beside the entries does nothing;
//   - on any other screen ("PRESS A TO START", the instructions, the adventure screen): A.
// The taps reach the game on its own thread, between frames (from the D3DDevice_Present hook, zuma_frame_hooks.cpp).

#include "zuma_touch.h"

#include "zuma_input_script.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(zuma_touch, true, "Zuma",
                    "Touch controls: the Switch's touch screen (the mouse on the PC) taps the game's menus and aims and "
                    "shoots in a level");
REXCVAR_DEFINE_BOOL(zuma_touch_log, false, "Zuma", "Log every tap handed to the game");
REXCVAR_DEFINE_STRING(zuma_touch_script, "", "Zuma",
                      "Test runs: scripted taps, comma-separated \"<frame>:<down|move|up>:<x>:<y>\" in 1280x720 screen "
                      "pixels, frames counted by this file's dispatch (one a frame); empty = off");

extern "C" REX_FUNC(__imp__sub_820DE870);  // WidgetManager::SetFocus(Widget*)

namespace {

constexpr uint32_t kApp = 0x823D13B8;
constexpr uint32_t kAppWidgetManager = 744;
constexpr uint32_t kAppBoard = 1620;
constexpr uint32_t kManagerFocus = 56;
constexpr uint32_t kBoardVtable = 0x82001220;
constexpr uint32_t kBoardGun = 600;
constexpr uint32_t kGunAngle = 4, kGunCentreX = 8, kGunCentreY = 12;
constexpr uint32_t kWidgetX = 8, kWidgetY = 12, kWidgetWidth = 16, kWidgetHeight = 20;
constexpr uint32_t kWidgetLinks = 56;  // four neighbours: +56 / +60 / +64 / +68
constexpr float kScreenLeft = 240.0f, kScreenTop = 60.0f, kScale = 1.25f;
constexpr uint16_t kPadA = 0x1000;  // XINPUT_GAMEPAD_A
constexpr float kPromptWidth = 100.0f;  // a button prompt's width, the game's units (it keeps 0)

enum class Phase : uint8_t { kBegan, kMoved, kEnded };
struct Touch {
  Phase phase;
  float x, y;  // 0..1 of the screen / window
};

std::mutex g_mutex;
std::deque<Touch> g_queue;
bool g_down = false;

void Push(Phase phase, float x, float y) {
  if (!REXCVAR_GET(zuma_touch)) return;
  x = std::clamp(x, 0.0f, 1.0f);
  y = std::clamp(y, 0.0f, 1.0f);
  std::lock_guard<std::mutex> lock(g_mutex);
  // Moves in a row: only the newest place matters.
  if (phase == Phase::kMoved && !g_queue.empty() && g_queue.back().phase == Phase::kMoved) {
    g_queue.back().x = x;
    g_queue.back().y = y;
    return;
  }
  if (g_queue.size() < 64) g_queue.push_back({phase, x, y});
}

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return __builtin_bswap32(be);
}
inline void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t be = __builtin_bswap32(value);
  std::memcpy(base + address, &be, 4);
}

// A touch in a level (the user, 2026-10-28: "if you do a tap held down you can rotate the frog/aiming reticle and when
// you tap again you fire"). A tap turns the frog to it and shoots (A) when it ends. A hold, a touch longer than
// kHoldFrames (0.2 s at 60 frames a second) or one that moves further than kHoldMove screen pixels, turns the frog after
// the finger and does not shoot; the frog stays aimed (`g_armed_board`), and the next tap on that Board shoots where it
// points, wherever it lands (a hold then aims again).
constexpr uint64_t kHoldFrames = 12;
constexpr float kHoldMove = 24.0f;
struct LevelTouch {
  bool active = false;  // a touch that began on the Board, which has the focus, is down
  bool hold = false;    // it is a hold
  uint64_t start = 0;   // the frame it began
  uint32_t board = 0, gun = 0;
  float start_x = 0, start_y = 0, x = 0, y = 0;  // 0..1 of the screen
};
LevelTouch g_level;
uint32_t g_armed_board = 0;

// The game's 640x480 coordinates of a place on the screen (0..1).
inline float GameX(float x) { return (x * 1280.0f - kScreenLeft) / kScale; }
inline float GameY(float y) { return (y * 720.0f - kScreenTop) / kScale; }

// A word that may hold a heap object's address: the game's objects are in the 0x40000000 heap (or static, as the app).
inline bool ObjectAddress(uint32_t a) {
  return !(a & 3) && ((a >= 0x40000000u && a < 0x50000000u) || (a >= 0x82390000u && a < 0x82492114u));
}
// A widget: an object whose first word is a vtable in the image's read-only data.
inline bool IsWidget(const uint8_t* base, uint32_t a) {
  if (!ObjectAddress(a)) return false;
  const uint32_t vtable = Load32(base, a);
  return vtable >= 0x82000400u && vtable < 0x82055300u;
}

// The frog turned to a place on the screen (0..1): Gun +4, the angle in radians, the shot's direction (sin a, cos a)
// on the screen (0 down), from its centre at +8 / +12. Returns the angle.
float TurnFrog(uint8_t* base, uint32_t gun, float x, float y) {
  const float cx = float(int32_t(Load32(base, gun + kGunCentreX)));
  const float cy = float(int32_t(Load32(base, gun + kGunCentreY)));
  const float angle = std::atan2(GameX(x) - cx, GameY(y) - cy);
  uint32_t bits;
  std::memcpy(&bits, &angle, 4);
  Store32(base, gun + kGunAngle, bits);
  return angle;
}

float FrogAngle(const uint8_t* base, uint32_t gun) {
  const uint32_t bits = Load32(base, gun + kGunAngle);
  float angle;
  std::memcpy(&angle, &bits, 4);
  return angle;
}

// The level a touch began on is still the one played (not ended, not under the pause menu).
bool LevelTouchValid(const uint8_t* base, const LevelTouch& t) {
  const uint32_t manager = Load32(base, kApp + kAppWidgetManager);
  return Load32(base, kApp + kAppBoard) == t.board && Load32(base, t.board) == kBoardVtable &&
         Load32(base, t.board + kBoardGun) == t.gun && ObjectAddress(manager) &&
         Load32(base, manager + kManagerFocus) == t.board;
}

// Test runs: zuma_touch_script, parsed once; the events due at each dispatch are queued as if touched.
struct ScriptedTouch {
  uint64_t frame;
  Phase phase;
  float x, y;
};

void PollScript(uint64_t frame) {
  static std::vector<ScriptedTouch> script;
  static size_t next = 0;
  static bool parsed = false;
  if (!parsed) {
    parsed = true;
    const std::string text = REXCVAR_GET(zuma_touch_script);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      unsigned long long when = 0;
      char kind[8] = {};
      float x = 0, y = 0;
      if (std::sscanf(item.c_str(), "%llu:%7[a-z]:%f:%f", &when, kind, &x, &y) == 4) {
        const std::string k = kind;
        const Phase phase = k == "down" ? Phase::kBegan : k == "move" ? Phase::kMoved : Phase::kEnded;
        script.push_back({when, phase, x / 1280.0f, y / 720.0f});
      } else {
        REXLOG_WARN("[touch] zuma_touch_script: \"{}\" is not <frame>:<down|move|up>:<x>:<y>", item);
      }
    }
    if (!script.empty()) REXLOG_INFO("[touch] scripted touches: {}", script.size());
  }
  while (next < script.size() && script[next].frame <= frame) {
    const ScriptedTouch& t = script[next++];
    Push(t.phase, t.x, t.y);
  }
}

#if REX_PLATFORM_SWITCH
// The Switch: the touch screen polled once a frame (1280x720 panel coordinates; docked, nothing touches it).
void PollSwitch() {
  static bool ready = false;
  static bool was_down = false;
  if (!ready) {
    hidInitializeTouchScreen();
    ready = true;
  }
  static float last_x = 0.0f, last_y = 0.0f;
  HidTouchScreenState state{};
  const bool down = hidGetTouchScreenStates(&state, 1) != 0 && state.count > 0;
  if (down) {
    last_x = float(state.touches[0].x) / 1280.0f;
    last_y = float(state.touches[0].y) / 720.0f;
    Push(was_down ? Phase::kMoved : Phase::kBegan, last_x, last_y);
  } else if (was_down) {
    Push(Phase::kEnded, last_x, last_y);
  }
  was_down = down;
}
#endif

}  // namespace

namespace zuma::touch {

void OnWindowMouse(MouseKind kind, float x, float y) {
  static bool down = false;  // the window's thread only
  switch (kind) {
    case MouseKind::kDown:
      down = true;
      Push(Phase::kBegan, x, y);
      break;
    case MouseKind::kMove:
      if (down) Push(Phase::kMoved, x, y);  // a level's hold follows them
      break;
    case MouseKind::kUp:
      down = false;
      Push(Phase::kEnded, x, y);
      break;
  }
}

void Dispatch(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(zuma_touch)) return;
  static uint64_t frame = 0;
  PollScript(++frame);
#if REX_PLATFORM_SWITCH
  PollSwitch();
#endif
  const bool log = REXCVAR_GET(zuma_touch_log);
  // A touch in a level, each frame: past kHoldFrames it is a hold. A hold turns the frog after the finger; a touch that
  // may still be a tap turns it too, unless a hold aimed it before (the tap then shoots where it points). The level
  // gone or paused, the touch ends.
  if (g_level.active) {
    if (!LevelTouchValid(base, g_level)) {
      g_level.active = false;
    } else {
      if (!g_level.hold && frame - g_level.start >= kHoldFrames) {
        g_level.hold = true;
        if (log) REXLOG_INFO("[touch] level: a hold, aiming");
      }
      if (g_level.hold || g_armed_board != g_level.board) TurnFrog(base, g_level.gun, g_level.x, g_level.y);
    }
  }
  std::deque<Touch> touches;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty()) return;
    touches.swap(g_queue);
  }
  for (const Touch& t : touches) {
    // A level's touch: its moves and its end.
    if (g_level.active && t.phase != Phase::kBegan) {
      g_level.x = t.x;
      g_level.y = t.y;
      if (std::fabs((t.x - g_level.start_x) * 1280.0f) > kHoldMove ||
          std::fabs((t.y - g_level.start_y) * 720.0f) > kHoldMove) {
        g_level.hold = true;
      }
      if (t.phase == Phase::kEnded) {
        g_level.active = false;
        g_down = false;
        if (!LevelTouchValid(base, g_level)) continue;
        if (g_level.hold) {
          // A hold: the frog stays where it points, and the next tap shoots there.
          g_armed_board = g_level.board;
          if (log) REXLOG_INFO("[touch] level: held, the frog at {:.3f}: the next tap shoots", FrogAngle(base, g_level.gun));
        } else {
          // A tap: the frog turned to it (unless a hold aimed it), then A.
          const bool armed = g_armed_board == g_level.board;
          const float angle = armed ? FrogAngle(base, g_level.gun) : TurnFrog(base, g_level.gun, t.x, t.y);
          zuma::input_script::Press(kPadA, 4);
          g_armed_board = 0;
          if (log) {
            REXLOG_INFO("[touch] level: tap at ({:.0f}, {:.0f}){}, the frog at {:.3f}: A", GameX(t.x), GameY(t.y),
                        armed ? " after a hold" : "", angle);
          }
        }
      }
      continue;
    }
    if (t.phase == Phase::kEnded) {
      g_down = false;
      continue;
    }
    if (t.phase != Phase::kBegan || g_down) continue;  // a tap acts once, where it starts
    g_down = true;
    // The game's 640x480 coordinates.
    const float gx = GameX(t.x), gy = GameY(t.y);
    const uint32_t manager = Load32(base, kApp + kAppWidgetManager);
    const uint32_t focus = ObjectAddress(manager) ? Load32(base, manager + kManagerFocus) : 0;
    const uint32_t board = Load32(base, kApp + kAppBoard);
    // A level: a touch of its own (LevelTouch above).
    if (board && ObjectAddress(board) && Load32(base, board) == kBoardVtable && focus == board) {
      const uint32_t gun = Load32(base, board + kBoardGun);
      if (ObjectAddress(gun)) {
        g_level = {true, false, frame, board, gun, t.x, t.y, t.x, t.y};
        if (g_armed_board != board) g_armed_board = 0;  // a hold on another level aims nothing here
        if (!g_armed_board) TurnFrog(base, gun, t.x, t.y);
        if (log) REXLOG_INFO("[touch] level: touch at ({:.0f}, {:.0f}){}", gx, gy, g_armed_board ? " (aimed by a hold)" : "");
      }
      continue;
    }
    // A menu: the focusable widgets linked from the focused one (breadth first over the four neighbours).
    std::vector<uint32_t> entries;
    if (IsWidget(base, focus)) {
      entries.push_back(focus);
      for (size_t i = 0; i < entries.size() && entries.size() < 32; ++i) {
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t n = Load32(base, entries[i] + kWidgetLinks + 4 * k);
          if (IsWidget(base, n) && std::find(entries.begin(), entries.end(), n) == entries.end()) entries.push_back(n);
        }
      }
    }
    if (entries.size() >= 2) {
      // An entry with no width is a button prompt ("A Play", "B Main Menu" on the adventure screen: 0 wide, 48 high at
      // the bottom corners); it takes a tap within 100 of its left edge. On a screen of prompts alone, a tap anywhere
      // else is A.
      uint32_t hit = 0;
      bool sized = false;
      for (const uint32_t w : entries) {
        const float x = float(int32_t(Load32(base, w + kWidgetX))), y = float(int32_t(Load32(base, w + kWidgetY)));
        float width = float(int32_t(Load32(base, w + kWidgetWidth)));
        const float height = float(int32_t(Load32(base, w + kWidgetHeight)));
        if (log) REXLOG_INFO("[touch]   entry 0x{:08X} at ({}, {}) {}x{}", w, x, y, width, height);
        if (width > 0.0f) sized = true; else width = kPromptWidth;
        if (gx >= x && gx < x + width && gy >= y && gy < y + height && width < 640.0f) hit = w;
      }
      if (!hit && !sized) {
        zuma::input_script::Press(kPadA, 4);
        if (log) REXLOG_INFO("[touch] prompts only at ({:.0f}, {:.0f}): A", gx, gy);
        continue;
      }
      if (hit) {
        if (hit != focus) {
          PPCContext call = ctx;
          call.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFu;
          call.r3.u64 = manager;
          call.r4.u64 = hit;
          __imp__sub_820DE870(call, base);
        }
        zuma::input_script::Press(kPadA, 4);
      }
      if (log) REXLOG_INFO("[touch] menu at ({:.0f}, {:.0f}): {} entries, entry 0x{:08X}", gx, gy, entries.size(), hit);
      continue;
    }
    // Any other screen: A.
    zuma::input_script::Press(kPadA, 4);
    if (log) REXLOG_INFO("[touch] screen (focus 0x{:08X}) at ({:.0f}, {:.0f}): A", focus, gx, gy);
  }
}

}  // namespace zuma::touch
