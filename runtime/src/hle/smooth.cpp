// In-game motion at the display rate.
//
// BDETECT's walking loop (0x436460) originally ran every 62 ms. Game logic is a set of object scripts
// that the scheduler (0x424c50) runs, and many of them reschedule themselves "next pass" (now + 1),
// so walking and animation advance once per loop pass. The loop now runs every 10 ms
// (recomp/src/patches.ts) and the scheduler called from it sees a clock that advances in 62 ms
// steps, so the scripts run exactly as often as in the original game.
//
// Drawing alone then changes only every 62 ms (16 fps). To show motion at the display rate, every
// pass draws each object, and the camera, blended between the last two logic steps by the time
// elapsed since the newer one. Everything is shown one step behind the logic, the same delay
// between Barbie and the camera that the original game had. The scheduler draws all objects from
// their records (x, y, z words at +0x14) right after running the scripts. Two patched stubs call
// timeGetTime just before that draw (the records get blended positions) and just after it (true
// positions come back before any other code reads them), so game logic never sees a blended value.
// Moves larger than kSnapDistance in one step are teleports (scene changes, respawns) and are drawn
// without blending.
//
// The in-game cursor object (script 0x43dd00) is not blended: it gets the live mouse position on
// every pass, clamped the way its script clamps it while the area scrolls at the screen edges.
// Writing the raw position made the pointer jump between the edge and the clamp on alternate frames.
#include "smooth.h"

#include <cstdlib>

#include "../core/rt.h"
#include "../platform/platform.h"

namespace smooth {
namespace {

using rt::kCameraPos;

// Return addresses of the timeGetTime calls this module handles.
constexpr uint32_t kLoopTop = 0x004364a6;        // walk loop: start of a pass, before the camera
constexpr uint32_t kSchedulerNow = 0x00424c8b;   // scheduler: before the scripts run
constexpr uint32_t kBeforeDraw = 0x004247dc;     // scheduler: patched stub before the object draw
constexpr uint32_t kAfterDraw = 0x004247f7;      // walk loop: patched stub after the scheduler
constexpr uint32_t kLoopEnd = 0x00436568;        // walk loop: frame-cap check
constexpr uint32_t kWalkSchedulerCall = 0x0043652e;  // return address of the walk loop's scheduler call
constexpr uint32_t kSchedulerFrameNow = 0x134;   // scheduler's return address, seen from kSchedulerNow
constexpr uint32_t kSchedulerFrameDraw = 0x138;  // ... and from the before-draw stub

constexpr uint32_t kStepMs = 62;  // the walk loop's original frame time

constexpr uint32_t kObjects = 0x0047fc50, kObjectStride = 0x38, kMaxObjects = 250;
constexpr uint32_t kObjectCount = 0x0047fc48;  // highest object index in use
constexpr uint32_t kScript = 0x08, kPos = 0x14;
constexpr uint32_t kBarbiePos = kObjects + kPos;  // object 0
constexpr uint32_t kCursorScript = 0x0043dd00;
constexpr uint32_t kEdgeScroll = 0x004758f8;  // signed byte: negative while the area scrolls at the edges
constexpr int kSnapDistance = 48;

struct Pos {
  int16_t v[3];
  bool operator==(const Pos& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
};

struct Slot {
  uint32_t script = 0;
  Pos prev{}, cur{}, shown{};
  bool has_cur = false, has_prev = false, is_shown = false;
};

Slot g_slots[kMaxObjects];
uint32_t g_step = 0;          // clock seen by the walk loop's scheduler, in kStepMs steps
bool g_step_pass = false;     // the current pass runs a logic step
int g_alpha = 256;            // blend for the current pass, 0..256 (256 = newest step)
Pos g_cam_prev{}, g_cam_cur{};
bool g_cam_valid = false;
bool g_any_shown = false;
const bool g_disabled = getenv("DETECTIVE_NO_SMOOTH") != nullptr;

Pos read_pos(uint32_t addr) {
  Pos p;
  for (int a = 0; a < 3; a++) p.v[a] = (int16_t)ld16(addr + 2 * a);
  return p;
}

void write_pos(uint32_t addr, const Pos& p) {
  for (int a = 0; a < 3; a++) st16(addr + 2 * a, (uint16_t)p.v[a]);
}

Pos blend(const Pos& from, const Pos& to, int alpha) {
  Pos out = to;
  for (int a = 0; a < 3; a++) {
    const int d = to.v[a] - from.v[a];
    if (d > kSnapDistance || d < -kSnapDistance) return to;
    out.v[a] = (int16_t)(from.v[a] + (d * alpha + (d >= 0 ? 128 : -128)) / 256);
  }
  return out;
}

uint32_t object_count() {
  const uint32_t n = ld32(kObjectCount) + 1;
  return n < kMaxObjects ? n : kMaxObjects;
}

// Put back true positions wherever the record still holds the blended value written for the draw.
void restore() {
  if (!g_any_shown) return;
  for (uint32_t i = 0; i < kMaxObjects; i++) {
    Slot& s = g_slots[i];
    if (!s.is_shown) continue;
    s.is_shown = false;
    const uint32_t rec = kObjects + i * kObjectStride;
    if (ld32(rec + kScript) != s.script) continue;
    for (int a = 0; a < 3; a++) {
      const uint32_t addr = rec + kPos + 2 * a;
      if ((int16_t)ld16(addr) == s.shown.v[a]) st16(addr, (uint16_t)s.cur.v[a]);
    }
  }
  g_any_shown = false;
}

void place_cursor(uint32_t rec) {
  int x = 0, y = 0;
  plat::mouse_pos(&x, &y);
  if ((int8_t)*rt::host(kEdgeScroll) < 0) {  // same bounds as script 0x43dd00
    x = x < 30 ? 30 : x > 610 ? 610 : x;
    y = y < 30 ? 30 : y > 450 ? 450 : y;
  }
  st16(rec + kPos, (uint16_t)x);
  st16(rec + kPos + 2, (uint16_t)y);
}

void loop_top(uint32_t now) {
  restore();
  const Pos barbie = read_pos(kBarbiePos);
  const uint32_t step = now - now % kStepMs;
  if (step != g_step) {
    g_step = step;
    g_step_pass = true;
    g_cam_prev = g_cam_valid ? g_cam_cur : barbie;
    g_cam_cur = barbie;
    g_cam_valid = true;
  }
  g_alpha = (int)((now - g_step) * 256 / kStepMs);
  if (g_alpha > 256) g_alpha = 256;
  write_pos(kCameraPos, g_disabled ? g_cam_cur : blend(g_cam_prev, g_cam_cur, g_alpha));
}

void before_draw() {
  const uint32_t n = object_count();
  for (uint32_t i = 0; i < n; i++) {
    Slot& s = g_slots[i];
    const uint32_t rec = kObjects + i * kObjectStride;
    const uint32_t script = ld32(rec + kScript);
    const Pos p = read_pos(rec + kPos);
    if (script != s.script) {
      s.script = script;
      s.has_cur = s.has_prev = false;
    }
    if (g_step_pass && s.has_cur) {
      s.prev = s.cur;
      s.has_prev = true;
    }
    s.cur = p;
    s.has_cur = true;
    if (script == kCursorScript) {
      place_cursor(rec);
      s.has_cur = false;  // live, never blended
      continue;
    }
    if (script <= 1 || !s.has_prev || g_disabled) continue;
    const Pos shown = blend(s.prev, s.cur, g_alpha);
    if (shown == s.cur) continue;
    write_pos(rec + kPos, shown);
    s.shown = shown;
    s.is_shown = true;
    g_any_shown = true;
  }
  for (uint32_t i = n; i < kMaxObjects; i++) g_slots[i].has_cur = g_slots[i].has_prev = false;
  static const bool trace = getenv("DETECTIVE_SMOOTH_TRACE") != nullptr;
  if (trace) {
    const Pos b = read_pos(kBarbiePos), cam = read_pos(kCameraPos);
    rt::log(rt::Log::Info, "SMOOTH step=%d a=%d bx=%d by=%d bz=%d cx=%d cz=%d", g_step_pass, g_alpha, b.v[0], b.v[1], b.v[2],
            cam.v[0], cam.v[2]);
  }
  g_step_pass = false;
}

}  // namespace

uint32_t time_get_time(Cpu* c, uint32_t now) {
  const uint32_t ret = ld32(c->esp);
  switch (ret) {
    case kLoopTop:
      loop_top(now);
      return now;
    case kSchedulerNow:
      if (ld32(c->esp + kSchedulerFrameNow) != kWalkSchedulerCall) return now;
      restore();
      return g_step;
    case kBeforeDraw:
      if (ld32(c->esp + kSchedulerFrameDraw) == kWalkSchedulerCall) before_draw();
      return now;
    case kAfterDraw:
    case kLoopEnd:
      restore();
      return now;
    default:
      return now;
  }
}

}  // namespace smooth
