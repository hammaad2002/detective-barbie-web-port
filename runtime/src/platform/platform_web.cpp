// Web platform layer (Emscripten). The game runs on pthreads (Web Workers); the page's main
// thread owns the canvas 2D context, composes DirectDraw frames in web_render_tick(), and
// forwards DOM input through the exported web_* functions. Audio is rendered by a feeder thread
// into a shared ring buffer that an AudioWorklet plays.
#include <emscripten.h>
#include <emscripten/html5.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/rt.h"
#include "../hle/ddraw.h"
#include "../web/web.h"
#include "platform.h"


namespace plat {
namespace {

std::mutex g_input_mutex;
std::deque<InputEvent> g_input;
uint8_t g_keys[256];
MouseState g_mouse{};
int g_mouse_x = 400, g_mouse_y = 300;
std::atomic<int> g_canvas_w{640}, g_canvas_h{480};
std::atomic<bool> g_window{false};
GamepadState g_pads[kMaxGamepads];
float g_rumble[kMaxGamepads * 2];  // strong, weak per slot; read by the page every frame

uint8_t dik_index(uint32_t dik) { return (uint8_t)((dik & 0x100) ? 0x80 | (dik & 0x7f) : dik); }

void push(const InputEvent& e) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  g_input.push_back(e);
}

}  // namespace

bool init() { return true; }

// The game's client area: the page maps pointer positions into it (the canvas size is the page's).
std::atomic<int> g_client_w{640}, g_client_h{480};

void create_window(const std::string& title, int width, int height) {
  g_window = true;
  (void)title;
  set_window_size(width, height);
}

bool has_window() { return g_window; }
void set_window_size(int width, int height) {
  if (width > 0 && height > 0) g_client_w = width, g_client_h = height;
}

void get_display_size(int* width, int* height) {
  *width = g_canvas_w;
  *height = g_canvas_h;
}

void get_window_size(int* width, int* height) {
  *width = g_canvas_w;
  *height = g_canvas_h;
}

// The game hides the cursor when it goes fullscreen; mirror that on the canvas (page thread).
void show_cursor(bool visible) {
  MAIN_THREAD_ASYNC_EM_ASM({ if (Module["canvas"]) Module["canvas"].style.cursor = $0 ? "" : "none"; }, visible ? 1 : 0);
}

// ---- Scripted input (DETECTIVE_INPUT / BELLA_INPUT; matches platform_sdl.cpp) ----
namespace {
struct ScriptedMouse {
  uint64_t frame;
  enum Kind { Move, ClickLeft, ClickRight } kind;
  int x = 0, y = 0;
  uint32_t hold = 5;
  bool started = false, down_sent = false, up_sent = false;
};
struct ScriptedKey {
  uint64_t frame;
  uint8_t vk = 0;
  uint16_t dik = 0;
  uint32_t hold = 4;
  bool down_sent = false, up_sent = false;
};
std::vector<ScriptedMouse> g_mouse_script;
std::vector<ScriptedKey> g_key_script;
std::string g_detective_input_script;
bool g_script_parsed = false;
bool lookup_key_name(const std::string& name, uint8_t* vk, uint16_t* dik) {
  std::string n = name;
  for (auto& c : n) c = (char)tolower((unsigned char)c);
  static const struct {
    const char* name;
    uint8_t vk;
    uint16_t dik;
  } kNamed[] = {{"backspace", 0x08, 0x0e}, {"escape", 0x1b, 0x01}, {"return", 0x0d, 0x1c}, {"enter", 0x0d, 0x1c},
                {"tab", 0x09, 0x0f},     {"space", 0x20, 0x39}};
  for (auto& e : kNamed)
    if (n == e.name) {
      *vk = e.vk;
      *dik = e.dik;
      return true;
    }
  if (n.size() == 1) {
    char c = n[0];
    if (c >= 'a' && c <= 'z') {
      *vk = (uint8_t)(c - 'a' + 'A');
      *dik = (uint16_t)(0x1e + (c - 'a'));
      return true;
    }
    if (c >= '0' && c <= '9') {
      *vk = (uint8_t)c;
      *dik = (uint16_t)(c == '0' ? 0x0b : 0x02 + (c - '1'));
      return true;
    }
  }
  return false;
}

bool parse_xy(const std::string& tail, int* x, int* y) {
  std::string s = tail;
  if (!s.empty() && s.front() == '(' && s.back() == ')') s = s.substr(1, s.size() - 2);
  size_t comma = s.find(',');
  if (comma == std::string::npos) comma = s.find('x');
  if (comma == std::string::npos) return false;
  *x = atoi(s.c_str());
  *y = atoi(s.c_str() + comma + 1);
  return true;
}

bool parse_mouse_action(const std::string& action, ScriptedMouse* out) {
  uint32_t hold = out->hold;
  size_t plus = action.find('+');
  std::string stem = plus == std::string::npos ? action : action.substr(0, plus);
  if (plus != std::string::npos) hold = (uint32_t)atoi(action.c_str() + plus + 1);
  auto try_move = [&](const char* prefix, size_t n) -> bool {
    if (stem.rfind(prefix, 0) != 0) return false;
    out->kind = ScriptedMouse::Move;
    if (!parse_xy(stem.substr(n), &out->x, &out->y)) return false;
    out->hold = 1;
    return true;
  };
  auto try_click = [&](const char* prefix, size_t n, ScriptedMouse::Kind kind) -> bool {
    if (stem.rfind(prefix, 0) != 0) return false;
    out->kind = kind;
    if (!parse_xy(stem.substr(n), &out->x, &out->y)) return false;
    out->hold = hold;
    return true;
  };
  if (try_move("move:", 5) || try_move("move", 4)) return true;
  if (try_click("lclick:", 7, ScriptedMouse::ClickLeft) || try_click("lclick", 6, ScriptedMouse::ClickLeft)) return true;
  if (try_click("rclick:", 7, ScriptedMouse::ClickRight) || try_click("rclick", 6, ScriptedMouse::ClickRight)) return true;
  if (try_click("click:", 6, ScriptedMouse::ClickLeft) || try_click("click", 5, ScriptedMouse::ClickLeft)) return true;
  return false;
}

size_t next_script_item(const std::string& s, size_t pos) {
  int depth = 0;
  for (size_t i = pos; i < s.size(); i++) {
    if (s[i] == '(') depth++;
    else if (s[i] == ')' && depth > 0) depth--;
    else if (s[i] == ',' && depth == 0) return i;
  }
  return std::string::npos;
}

void parse_input_script() {
  g_script_parsed = true;
  const char* e = nullptr;
  if (!g_detective_input_script.empty()) e = g_detective_input_script.c_str();
  if (!e) e = getenv("DETECTIVE_INPUT");
  if (!e) e = getenv("BELLA_INPUT");
  if (!e) return;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = next_script_item(s, pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    uint64_t frame = strtoull(item.c_str(), nullptr, 10);
    std::string action = item.substr(colon + 1);
    ScriptedMouse m{};
    m.frame = frame;
    if (parse_mouse_action(action, &m)) {
      g_mouse_script.push_back(m);
      continue;
    }
    ScriptedKey k{};
    k.frame = frame;
    k.hold = 4;
    size_t plus = action.find('+');
    if (plus != std::string::npos) {
      k.hold = (uint32_t)atoi(action.c_str() + plus + 1);
      action = action.substr(0, plus);
    }
    std::string key_name = action;
    if (action.rfind("key(", 0) == 0 && action.size() > 5 && action.back() == ')') key_name = action.substr(4, action.size() - 5);
    else if (action.rfind("key:", 0) == 0) key_name = action.substr(4);
    if (lookup_key_name(key_name, &k.vk, &k.dik)) g_key_script.push_back(k);
    else rt::log(rt::Log::Warn, "scripted input: unknown key '%s'", key_name.c_str());
  }
}

void scripted_keys(std::vector<InputEvent>& out) {
  const uint64_t f = ddraw::frame_counter();
  for (auto& k : g_key_script) {
    const uint64_t t0 = k.frame;
    bool want_down = f >= t0 && !k.down_sent;
    bool want_up = f >= t0 + k.hold && k.down_sent && !k.up_sent;
    if (!want_down && !want_up) continue;
    if (want_down) {
      {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_keys[dik_index(k.dik)] = 0x80;
      }
      InputEvent ev{};
      ev.kind = InputEvent::KeyDown;
      ev.vk = k.vk;
      ev.scancode = k.dik;
      out.push_back(ev);
      k.down_sent = true;
    } else {
      {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_keys[dik_index(k.dik)] = 0;
      }
      InputEvent ev{};
      ev.kind = InputEvent::KeyUp;
      ev.vk = k.vk;
      ev.scancode = k.dik;
      out.push_back(ev);
      k.up_sent = true;
    }
  }
}

void scripted_mouse(std::vector<InputEvent>& out) {
  if (!g_script_parsed) parse_input_script();
  const uint64_t f = ddraw::frame_counter();
  for (auto& m : g_mouse_script) {
    const uint64_t t0 = m.frame;
    if (!m.started && f >= t0) {
      m.started = true;
      {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_mouse_x = m.x;
        g_mouse_y = m.y;
      }
      InputEvent ev{};
      ev.kind = InputEvent::MouseMove;
      ev.x = m.x;
      ev.y = m.y;
      out.push_back(ev);
      if (m.kind != ScriptedMouse::Move) {
        uint32_t b = m.kind == ScriptedMouse::ClickRight ? 1u : 0u;
        {
          std::lock_guard<std::mutex> lock(g_input_mutex);
          g_mouse.buttons[b] = 0x80;
        }
        InputEvent down{};
        down.kind = InputEvent::MouseDown;
        down.button = b;
        down.x = m.x;
        down.y = m.y;
        out.push_back(down);
        m.down_sent = true;
      }
    } else if (m.down_sent && !m.up_sent && f >= t0 + m.hold) {
      m.up_sent = true;
      uint32_t b = m.kind == ScriptedMouse::ClickRight ? 1u : 0u;
      {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_mouse.buttons[b] = 0;
      }
      InputEvent up{};
      up.kind = InputEvent::MouseUp;
      up.button = b;
      up.x = m.x;
      up.y = m.y;
      out.push_back(up);
    }
  }
}

}  // namespace

void set_detective_input_script(const char* script) {
  g_detective_input_script = script ? script : "";
  g_script_parsed = false;
  g_mouse_script.clear();
  g_key_script.clear();
}

void poll_events(std::vector<InputEvent>& out) {
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    while (!g_input.empty()) {
      out.push_back(g_input.front());
      g_input.pop_front();
    }
  }
  scripted_mouse(out);
  scripted_keys(out);
}

const uint8_t* keyboard_state() { return g_keys; }

GamepadState gamepad_state(int slot) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  return slot >= 0 && slot < kMaxGamepads ? g_pads[slot] : GamepadState{};
}

void gamepad_rumble(int slot, float strong, float weak) {
  if (slot < 0 || slot >= kMaxGamepads) return;
  g_rumble[2 * slot] = strong;
  g_rumble[2 * slot + 1] = weak;
}

MouseState take_mouse_state() {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  MouseState m = g_mouse;
  g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
  return m;
}

void mouse_pos(int* x, int* y) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  if (x) *x = g_mouse_x;
  if (y) *y = g_mouse_y;
}

// SetCursorPos moves the pointer without producing DirectInput motion.
void set_mouse_pos(int x, int y) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  g_mouse_x = x;
  g_mouse_y = y;
}

bool mouse_button_down(int button) {
  if (button < 0 || button >= 4) return false;
  std::lock_guard<std::mutex> lock(g_input_mutex);
  return (g_mouse.buttons[button] & 0x80) != 0;
}

void frame_presented(uint64_t) {}

void present_rgba(const uint32_t* bgra, int w, int h) {
  present_rgba8((const uint8_t*)bgra, w, h);
}

void present_rgba8(const uint8_t* rgba, int w, int h) {
  EM_ASM(
      {
        const w = $0;
        const h = $1;
        const ptr = $2;
        const nbytes = w * h * 4;
        const t0 = performance.now();
        const off = Module.__presentOffscreen;
        const offCtx = Module.__presentOffCtx;
        const dst = Module.__presentCanvas;
        const dstCtx = Module.__presentCtx;
        if (!offCtx || !dstCtx) return;
        let img = Module.__presentImageData;
        if (!img || img.width !== w || img.height !== h) {
          if (off.width !== w || off.height !== h) {
            off.width = w;
            off.height = h;
          }
          img = offCtx.createImageData(w, h);
          Module.__presentImageData = img;
        }
        img.data.set(HEAPU8.subarray(ptr, ptr + nbytes));
        offCtx.putImageData(img, 0, 0);
        dstCtx.imageSmoothingEnabled = false;
        dstCtx.clearRect(0, 0, dst.width, dst.height);
        dstCtx.drawImage(off, 0, 0, w, h, 0, 0, dst.width, dst.height);
        const uploadUs = (performance.now() - t0) * 1000;
        const p = Module.__perfNative;
        if (p) {
          p.uploadUs += uploadUs;
          p.uploadN++;
        }
      },
      w, h, rgba);
}

namespace {
std::atomic<uint64_t> g_last_vsync_us{0};
constexpr uint64_t kFrameUs = 1000000 / 60;

uint64_t steady_us() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void pace_frame_slot() {
  const uint64_t now = steady_us();
  const uint64_t last = g_last_vsync_us.load();
  if (last && now > last) {
    const uint64_t elapsed = now - last;
    if (elapsed < kFrameUs) {
      const uint32_t wait_ms = (uint32_t)((kFrameUs - elapsed + 999) / 1000);
      if (wait_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
  }
  g_last_vsync_us.store(steady_us());
}
}  // namespace

void pace_presented_flip() { web::flip_paced(); }

void pace_vblank() { pace_frame_slot(); }

bool gl_create_context() { return web::gl_ready(); }
void gl_swap() {}
void gl_set_swap_interval(int) {}

// ---- Audio ------------------------------------------------------------------------------------

namespace {
struct AudioRing {
  std::atomic<uint32_t> write{0}, read{0};  // frame counters (wrap freely)
  uint32_t frames = 0;                       // capacity (power of two)
  uint32_t rate = 44100;
  float* data = nullptr;                     // interleaved stereo
};
AudioRing g_ring;
std::atomic<bool> g_audio_running{false};
}  // namespace

bool audio_open(int rate, void (*render)(float* out, int frames)) {
  if (!g_ring.data) {
    rt::log(rt::Log::Warn, "audio: no output ring (the page did not create an AudioContext)");
    return false;
  }
  if (g_audio_running.exchange(true)) return true;
  (void)rate;  // the page creates its AudioContext at the mixer rate
  std::thread([render] {
    // Keep ~90 ms buffered ahead of the worklet.
    const uint32_t target = g_ring.rate * 90 / 1000, chunk = 256;
    std::vector<float> buf(chunk * 2);
    for (;;) {
      uint32_t w = g_ring.write.load(std::memory_order_relaxed), r = g_ring.read.load(std::memory_order_acquire);
      if (w - r + chunk > target) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      render(buf.data(), (int)chunk);
      for (uint32_t i = 0; i < chunk; i++) {
        uint32_t slot = (w + i) & (g_ring.frames - 1);
        g_ring.data[2 * slot] = buf[2 * i];
        g_ring.data[2 * slot + 1] = buf[2 * i + 1];
      }
      g_ring.write.store(w + chunk, std::memory_order_release);
    }
  }).detach();
  rt::log(rt::Log::Info, "audio: feeding a %u-frame ring at %u Hz", g_ring.frames, g_ring.rate);
  return true;
}

}  // namespace plat

// ---- Exports for the page ------------------------------------------------------------------------

extern "C" {

// Allocates the audio ring; returns its address (layout: write u32, read u32, frames u32, rate u32,
// data pointer u32) so the AudioWorklet can read it from the shared memory.
EMSCRIPTEN_KEEPALIVE uint32_t web_audio_ring_create(uint32_t rate) {
  using namespace plat;
  static uint32_t header[5];
  g_ring.frames = 8192;
  g_ring.rate = rate;
  g_ring.data = new float[g_ring.frames * 2]();
  // The worklet reads the live atomics through these addresses.
  header[0] = (uint32_t)(uintptr_t)&g_ring.write;
  header[1] = (uint32_t)(uintptr_t)&g_ring.read;
  header[2] = g_ring.frames;
  header[3] = g_ring.rate;
  header[4] = (uint32_t)(uintptr_t)g_ring.data;
  return (uint32_t)(uintptr_t)header;
}

// The game's client size as width << 16 | height, for mapping pointer positions.
EMSCRIPTEN_KEEPALIVE int web_client_size() { return plat::g_client_w << 16 | plat::g_client_h; }

EMSCRIPTEN_KEEPALIVE void web_canvas_size(int w, int h) {
  plat::g_canvas_w = w;
  plat::g_canvas_h = h;
}

// kind: 0 key down, 1 key up, 2 character.
EMSCRIPTEN_KEEPALIVE void web_key(int kind, uint32_t vk, uint32_t dik, uint32_t ch) {
  using namespace plat;
  InputEvent e{};
  if (kind == 2) {
    e.kind = InputEvent::Char;
    e.ch = ch;
    push(e);
    return;
  }
  bool down = kind == 0;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (dik) g_keys[dik_index(dik)] = down ? 0x80 : 0;
  }
  e.kind = down ? InputEvent::KeyDown : InputEvent::KeyUp;
  e.vk = vk;
  e.scancode = dik;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_move(int x, int y, int dx, int dy) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_mouse.dx += dx;
    g_mouse.dy += dy;
    g_mouse_x = x;
    g_mouse_y = y;
  }
  InputEvent e{};
  e.kind = InputEvent::MouseMove;
  e.x = x, e.y = y;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_button(int button, int down, int x, int y) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (button >= 0 && button < 4) g_mouse.buttons[button] = down ? 0x80 : 0;
    g_mouse_x = x;
    g_mouse_y = y;
  }
  InputEvent e{};
  e.kind = down ? InputEvent::MouseDown : InputEvent::MouseUp;
  e.button = (uint32_t)button;
  e.x = x, e.y = y;
  push(e);
}

EMSCRIPTEN_KEEPALIVE void web_mouse_wheel(int dy) {
  using namespace plat;
  {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_mouse.wheel += dy * 120;
  }
  InputEvent e{};
  e.kind = InputEvent::MouseWheel;
  e.dy = dy;
  push(e);
}

// The page polls navigator.getGamepads() once per animation frame and reports each player slot.
EMSCRIPTEN_KEEPALIVE void web_gamepad(int slot, int connected, uint32_t buttons, float lx, float ly, float rx, float ry, float lt,
                                      float rt) {
  using namespace plat;
  if (slot < 0 || slot >= kMaxGamepads) return;
  std::lock_guard<std::mutex> lock(g_input_mutex);
  GamepadState& p = g_pads[slot];
  p.connected = connected != 0;
  p.buttons = buttons;
  p.lx = lx, p.ly = ly, p.rx = rx, p.ry = ry, p.lt = lt, p.rt = rt;
}

// Address of the rumble requests (float strong, weak per slot) for the page to play.
EMSCRIPTEN_KEEPALIVE float* web_gamepad_rumble() { return plat::g_rumble; }

// Frame-synchronous DETECTIVE_INPUT (shared across pthreads; set from the page before callMain).
EMSCRIPTEN_KEEPALIVE void web_set_detective_input(const char* script) { plat::set_detective_input_script(script); }

EMSCRIPTEN_KEEPALIVE void web_focus(int focused) {
  using namespace plat;
  if (!focused) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    memset(g_keys, 0, sizeof g_keys);
  }
  InputEvent e{};
  e.kind = focused ? InputEvent::FocusGained : InputEvent::FocusLost;
  push(e);
}

}  // extern "C"
