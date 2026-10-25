// Native platform layer on SDL2: window, keyboard/mouse input (as Windows VK codes and
// DirectInput/PC set-1 scan codes) and gamepads (SDL GameController, or scripted for tests).
#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/rt.h"
#include "../hle/ddraw.h"
#include "platform.h"

namespace plat {
namespace {

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
int g_tex_w = 0, g_tex_h = 0;
std::atomic<bool> g_presenter_run{false};
uint8_t g_keys[256];
MouseState g_mouse{};
int32_t g_mouse_x = 0, g_mouse_y = 0;
std::mutex g_mouse_mutex;

struct KeyMap {
  SDL_Scancode sdl;
  uint8_t vk;
  uint16_t dik;  // set-1 scan code; 0x100 marks extended (E0-prefixed) keys
};

const KeyMap kKeys[] = {
    {SDL_SCANCODE_ESCAPE, 0x1b, 0x01}, {SDL_SCANCODE_1, '1', 0x02}, {SDL_SCANCODE_2, '2', 0x03},
    {SDL_SCANCODE_3, '3', 0x04}, {SDL_SCANCODE_4, '4', 0x05}, {SDL_SCANCODE_5, '5', 0x06}, {SDL_SCANCODE_6, '6', 0x07},
    {SDL_SCANCODE_7, '7', 0x08}, {SDL_SCANCODE_8, '8', 0x09}, {SDL_SCANCODE_9, '9', 0x0a}, {SDL_SCANCODE_0, '0', 0x0b},
    {SDL_SCANCODE_MINUS, 0xbd, 0x0c}, {SDL_SCANCODE_EQUALS, 0xbb, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0e},
    {SDL_SCANCODE_TAB, 0x09, 0x0f}, {SDL_SCANCODE_Q, 'Q', 0x10}, {SDL_SCANCODE_W, 'W', 0x11}, {SDL_SCANCODE_E, 'E', 0x12},
    {SDL_SCANCODE_R, 'R', 0x13}, {SDL_SCANCODE_T, 'T', 0x14}, {SDL_SCANCODE_Y, 'Y', 0x15}, {SDL_SCANCODE_U, 'U', 0x16},
    {SDL_SCANCODE_I, 'I', 0x17}, {SDL_SCANCODE_O, 'O', 0x18}, {SDL_SCANCODE_P, 'P', 0x19},
    {SDL_SCANCODE_LEFTBRACKET, 0xdb, 0x1a}, {SDL_SCANCODE_RIGHTBRACKET, 0xdd, 0x1b}, {SDL_SCANCODE_RETURN, 0x0d, 0x1c},
    {SDL_SCANCODE_LCTRL, 0x11, 0x1d}, {SDL_SCANCODE_A, 'A', 0x1e}, {SDL_SCANCODE_S, 'S', 0x1f}, {SDL_SCANCODE_D, 'D', 0x20},
    {SDL_SCANCODE_F, 'F', 0x21}, {SDL_SCANCODE_G, 'G', 0x22}, {SDL_SCANCODE_H, 'H', 0x23}, {SDL_SCANCODE_J, 'J', 0x24},
    {SDL_SCANCODE_K, 'K', 0x25}, {SDL_SCANCODE_L, 'L', 0x26}, {SDL_SCANCODE_SEMICOLON, 0xba, 0x27},
    {SDL_SCANCODE_APOSTROPHE, 0xde, 0x28}, {SDL_SCANCODE_GRAVE, 0xc0, 0x29}, {SDL_SCANCODE_LSHIFT, 0x10, 0x2a},
    {SDL_SCANCODE_BACKSLASH, 0xdc, 0x2b}, {SDL_SCANCODE_Z, 'Z', 0x2c}, {SDL_SCANCODE_X, 'X', 0x2d},
    {SDL_SCANCODE_C, 'C', 0x2e}, {SDL_SCANCODE_V, 'V', 0x2f}, {SDL_SCANCODE_B, 'B', 0x30}, {SDL_SCANCODE_N, 'N', 0x31},
    {SDL_SCANCODE_M, 'M', 0x32}, {SDL_SCANCODE_COMMA, 0xbc, 0x33}, {SDL_SCANCODE_PERIOD, 0xbe, 0x34},
    {SDL_SCANCODE_SLASH, 0xbf, 0x35}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36}, {SDL_SCANCODE_KP_MULTIPLY, 0x6a, 0x37},
    {SDL_SCANCODE_LALT, 0x12, 0x38}, {SDL_SCANCODE_SPACE, 0x20, 0x39}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3a},
    {SDL_SCANCODE_F1, 0x70, 0x3b}, {SDL_SCANCODE_F2, 0x71, 0x3c}, {SDL_SCANCODE_F3, 0x72, 0x3d},
    {SDL_SCANCODE_F4, 0x73, 0x3e}, {SDL_SCANCODE_F5, 0x74, 0x3f}, {SDL_SCANCODE_F6, 0x75, 0x40},
    {SDL_SCANCODE_F7, 0x76, 0x41}, {SDL_SCANCODE_F8, 0x77, 0x42}, {SDL_SCANCODE_F9, 0x78, 0x43},
    {SDL_SCANCODE_F10, 0x79, 0x44}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x91, 0x46},
    {SDL_SCANCODE_KP_7, 0x67, 0x47}, {SDL_SCANCODE_KP_8, 0x68, 0x48}, {SDL_SCANCODE_KP_9, 0x69, 0x49},
    {SDL_SCANCODE_KP_MINUS, 0x6d, 0x4a}, {SDL_SCANCODE_KP_4, 0x64, 0x4b}, {SDL_SCANCODE_KP_5, 0x65, 0x4c},
    {SDL_SCANCODE_KP_6, 0x66, 0x4d}, {SDL_SCANCODE_KP_PLUS, 0x6b, 0x4e}, {SDL_SCANCODE_KP_1, 0x61, 0x4f},
    {SDL_SCANCODE_KP_2, 0x62, 0x50}, {SDL_SCANCODE_KP_3, 0x63, 0x51}, {SDL_SCANCODE_KP_0, 0x60, 0x52},
    {SDL_SCANCODE_KP_PERIOD, 0x6e, 0x53}, {SDL_SCANCODE_F11, 0x7a, 0x57}, {SDL_SCANCODE_F12, 0x7b, 0x58},
    {SDL_SCANCODE_KP_ENTER, 0x0d, 0x11c}, {SDL_SCANCODE_RCTRL, 0x11, 0x11d}, {SDL_SCANCODE_KP_DIVIDE, 0x6f, 0x135},
    {SDL_SCANCODE_RALT, 0x12, 0x138}, {SDL_SCANCODE_HOME, 0x24, 0x147}, {SDL_SCANCODE_UP, 0x26, 0x148},
    {SDL_SCANCODE_PAGEUP, 0x21, 0x149}, {SDL_SCANCODE_LEFT, 0x25, 0x14b}, {SDL_SCANCODE_RIGHT, 0x27, 0x14d},
    {SDL_SCANCODE_END, 0x23, 0x14f}, {SDL_SCANCODE_DOWN, 0x28, 0x150}, {SDL_SCANCODE_PAGEDOWN, 0x22, 0x151},
    {SDL_SCANCODE_INSERT, 0x2d, 0x152}, {SDL_SCANCODE_DELETE, 0x2e, 0x153},
};

const KeyMap* lookup(SDL_Scancode s) {
  for (auto& k : kKeys)
    if (k.sdl == s) return &k;
  return nullptr;
}

// DirectInput keyboard indices: extended keys use 0x80 | scancode (e.g. DIK_UP = 0xC8).
uint8_t dik_index(uint16_t dik) { return (uint8_t)((dik & 0x100) ? 0x80 | (dik & 0x7f) : dik); }

bool headless_test_mode() {
  return getenv("BELLA_HIDDEN") || getenv("BELLA_DUMP") || getenv("BELLA_DUMP_DIR") || getenv("DETECTIVE_HIDDEN");
}

bool frame_dump_mode() { return getenv("DETECTIVE_DUMP_FRAME") != nullptr; }

struct FrameDumpRequest {
  uint64_t frame = 0;
  std::string path;
  bool done = false;
};

struct GuestPeekRequest {
  uint64_t frame = 0;
  uint32_t addr = 0;
  uint32_t len = 0;
  bool done = false;
};

struct GuestPokeRequest {
  uint64_t frame = 0;
  uint32_t addr = 0;
  uint32_t value = 0;
  bool done = false;
};

std::vector<GuestPeekRequest> parse_guest_peeks() {
  std::vector<GuestPeekRequest> out;
  const char* e = getenv("DETECTIVE_PEEK");
  if (!e || !e[0]) return out;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    unsigned long long f = 0;
    unsigned long a = 0, n = 0;
    if (sscanf(item.c_str(), "%llu:%lx:%lu", &f, &a, &n) == 3 && f && n)
      out.push_back({f, (uint32_t)a, (uint32_t)n, false});
  }
  return out;
}

std::vector<GuestPokeRequest> parse_guest_pokes() {
  std::vector<GuestPokeRequest> out;
  const char* e = getenv("DETECTIVE_POKE");
  if (!e || !e[0]) return out;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    unsigned long long f = 0;
    unsigned long a = 0, v = 0;
    if (sscanf(item.c_str(), "%llu:%lx=%lx", &f, &a, &v) == 3 && f && a)
      out.push_back({f, (uint32_t)a, (uint32_t)v, false});
  }
  return out;
}

void run_guest_pokes(uint64_t fc, std::vector<GuestPokeRequest>& reqs) {
  for (auto& req : reqs) {
    if (req.done || !req.frame || fc < req.frame || !req.addr) continue;
    req.done = true;
    st32(req.addr, req.value);
    rt::log(rt::Log::Info, "DETECTIVE_POKE: frame %llu wrote [%08x]=%08x (%u)", (unsigned long long)fc, req.addr,
            req.value, req.value);
  }
}

void run_guest_peeks(uint64_t fc, std::vector<GuestPeekRequest>& reqs) {
  for (auto& req : reqs) {
    if (req.done || !req.frame || fc < req.frame || !req.addr || !req.len) continue;
    req.done = true;
    std::string hex;
    std::string ascii;
    for (uint32_t i = 0; i < req.len; i++) {
      uint8_t b = ld8(req.addr + i);
      char buf[4];
      snprintf(buf, sizeof(buf), "%02x", b);
      hex += buf;
      if (i) hex += ' ';
      ascii += (b >= 32 && b < 127) ? (char)b : '.';
    }
    rt::log(rt::Log::Info, "DETECTIVE_PEEK frame %llu @0x%x len %u: %s | \"%s\"", (unsigned long long)fc, req.addr,
            req.len, hex.c_str(), ascii.c_str());
  }
}

size_t next_comma_outside_parens(const std::string& s, size_t pos) {
  int depth = 0;
  for (size_t i = pos; i < s.size(); i++) {
    if (s[i] == '(') depth++;
    else if (s[i] == ')' && depth > 0) depth--;
    else if (s[i] == ',' && depth == 0) return i;
  }
  return std::string::npos;
}

std::vector<FrameDumpRequest> parse_frame_dumps() {
  std::vector<FrameDumpRequest> out;
  const char* e = getenv("DETECTIVE_DUMP_FRAME");
  if (!e || !e[0]) return out;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = next_comma_outside_parens(s, pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    FrameDumpRequest req;
    req.frame = strtoull(item.c_str(), nullptr, 10);
    req.path = item.substr(colon + 1);
    if (req.frame && !req.path.empty()) out.push_back(std::move(req));
  }
  return out;
}

bool write_ppm(const std::string& path, const std::vector<uint32_t>& bgra, int w, int h) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    rt::log(rt::Log::Error, "DETECTIVE_DUMP_FRAME: cannot open %s", path.c_str());
    return false;
  }
  out << "P6\n" << w << " " << h << "\n255\n";
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t px = bgra[(size_t)y * w + x];
      uint8_t b = (uint8_t)(px & 0xff), g = (uint8_t)((px >> 8) & 0xff), r = (uint8_t)((px >> 16) & 0xff);
      out.put((char)r);
      out.put((char)g);
      out.put((char)b);
    }
  rt::log(rt::Log::Info, "DETECTIVE_DUMP_FRAME: wrote %dx%d frame to %s", w, h, path.c_str());
  return true;
}

void ensure_present_window(int width, int height) {
  if (!g_window) {
    const bool hidden = getenv("BELLA_HIDDEN") != nullptr;
    const std::string title = "Detective Barbie";
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (hidden) flags |= SDL_WINDOW_HIDDEN;
    g_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!g_window && headless_test_mode()) return;
    if (!g_window) rt::fatal("SDL_CreateWindow failed: %s", SDL_GetError());
  }
  if (!g_renderer) {
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer && (headless_test_mode() || frame_dump_mode())) return;
    if (!g_renderer) rt::fatal("SDL_CreateRenderer failed: %s", SDL_GetError());
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  }
}

void ensure_texture(int w, int h) {
  if (g_texture && w == g_tex_w && h == g_tex_h) return;
  if (g_texture) SDL_DestroyTexture(g_texture);
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_BGRA8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  if (!g_texture) rt::fatal("SDL_CreateTexture failed: %s", SDL_GetError());
  g_tex_w = w;
  g_tex_h = h;
  SDL_SetWindowSize(g_window, w, h);
}

// DETECTIVE_CTL=<dir>: live control for interactive debugging. Each presenter tick, if <dir>/cmd exists it is
// consumed line by line: "dump <path>", "peek <hexaddr> <len>", "poke <hexaddr> <hexvalue>", or any DETECTIVE_INPUT
// action (click(x,y)+hold, move(x,y), key(name)+hold) which is scheduled for the next frame. Results go to <dir>/ack.
std::mutex g_ctl_mutex;
std::vector<std::string> g_ctl_pending;

void ctl_poll(uint64_t fc) {
  static const char* dir = getenv("DETECTIVE_CTL");
  if (!dir || !dir[0]) return;
  const std::string cmd = std::string(dir) + "/cmd", tmp = cmd + ".run";
  if (rename(cmd.c_str(), tmp.c_str()) != 0) return;
  std::ifstream in(tmp);
  std::string out = "frame " + std::to_string(fc) + "\n", line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    char path[512];
    unsigned addr = 0, val = 0, len = 0;
    if (sscanf(line.c_str(), "dump %511s", path) == 1) {
      std::vector<uint32_t> rgba;
      int w = 0, h = 0;
      bool ok = false;
      for (int tries = 0; tries < 200 && !(ok = ddraw::compose(rgba, w, h)); tries++) SDL_Delay(2);  // compose try-locks
      out += (ok && write_ppm(path, rgba, w, h)) ? "dumped\n" : "dump failed\n";
    } else if (sscanf(line.c_str(), "peek %x %u", &addr, &len) == 2) {
      char b[4];
      out += "peek " + line.substr(5) + ":";
      for (unsigned i = 0; i < len; i++) snprintf(b, sizeof b, " %02x", ld8(addr + i)), out += b;
      out += "\n";
    } else if (line == "stats") {
      for (int i = 0; i < rt::StatCount; i++) out += std::to_string(rt::g_stats[i].load()) + " ";
      out += "\n";
    } else if (double sc = 0; sscanf(line.c_str(), "speed %lf", &sc) == 1) {
      rt::set_time_scale(sc);
      out += "speed set\n";
    } else if (sscanf(line.c_str(), "poke %x %x", &addr, &val) == 2) {
      st32(addr, val);
      out += "poked\n";
    } else {
      std::lock_guard<std::mutex> lock(g_ctl_mutex);
      g_ctl_pending.push_back(line);
      out += "queued " + line + "\n";
    }
  }
  remove(tmp.c_str());
  std::ofstream(std::string(dir) + "/ack.tmp") << out;
  rename((std::string(dir) + "/ack.tmp").c_str(), (std::string(dir) + "/ack").c_str());
}

void presenter_loop() {
  uint64_t last = 0;
  std::vector<FrameDumpRequest> dumps = parse_frame_dumps();
  std::vector<GuestPeekRequest> peeks = parse_guest_peeks();
  std::vector<GuestPokeRequest> pokes = parse_guest_pokes();
  while (g_presenter_run) {
    const uint64_t fc = ddraw::frame_counter();
    if (fc != last && fc != 0) {
      std::vector<uint32_t> rgba;
      int w = 0, h = 0;
      if (ddraw::compose(rgba, w, h) && w > 0 && h > 0) {
        run_guest_pokes(fc, pokes);
        run_guest_peeks(fc, peeks);
        for (auto& dump : dumps) {
          if (!dump.done && dump.frame && fc >= dump.frame) {
            write_ppm(dump.path, rgba, w, h);
            dump.done = true;
          }
        }
        present_rgba(rgba.data(), w, h);
        frame_presented(fc);
        last = fc;
      }
    }
    ctl_poll(fc);
    SDL_Delay(8);
  }
}

void start_presenter() {
  if (headless_test_mode() && !frame_dump_mode()) return;
  if (g_presenter_run.exchange(true)) return;
  std::thread(presenter_loop).detach();
}

}  // namespace

bool init() {
  // Let SIGINT/SIGTERM terminate the process instead of becoming a quit event the game may ignore.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!getenv("DISPLAY")) {
    const char* host_display = getenv("HOST_DISPLAY");
    if (host_display && host_display[0]) setenv("DISPLAY", host_display, 1);
  }
  const Uint32 full = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
  if (SDL_Init(full) != 0) {
    if (headless_test_mode() || frame_dump_mode()) {
      const char* display = getenv("DISPLAY");
      const char* driver = getenv("SDL_VIDEODRIVER");
      rt::log(rt::Log::Warn,
              "SDL_Init (video) failed: %s — continuing without video for headless test/dump "
              "(DISPLAY=%s, SDL_VIDEODRIVER=%s)",
              SDL_GetError(), display ? display : "(unset)", driver ? driver : "(default)");
      SDL_ClearError();
      const Uint32 no_video = SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER;
      if (SDL_Init(no_video) != 0) {
        rt::log(rt::Log::Error, "SDL_Init failed: %s", SDL_GetError());
        return false;
      }
      return true;
    }
    rt::log(rt::Log::Error, "SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  start_presenter();
  return true;
}

void create_window(const std::string& title, int width, int height) {
  if (g_window) return;
  const bool hidden = getenv("BELLA_HIDDEN") != nullptr || frame_dump_mode();
  const std::string window_title = "Detective Barbie - " + title;
  Uint32 flags = SDL_WINDOW_RESIZABLE;
  if (hidden) flags |= SDL_WINDOW_HIDDEN;
  g_window = SDL_CreateWindow(window_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
  if (!g_window && (headless_test_mode() || frame_dump_mode())) {
    rt::log(rt::Log::Warn, "SDL_CreateWindow failed: %s — continuing without a window", SDL_GetError());
    SDL_ClearError();
    return;
  }
  if (!g_window) rt::fatal("SDL_CreateWindow failed: %s", SDL_GetError());
}

bool has_window() { return g_window != nullptr; }

bool gl_create_context() {
  if (!g_window) create_window("Detective Barbie", 640, 480);
  SDL_GLContext ctx = SDL_GL_CreateContext(g_window);
  if (!ctx) {
    rt::log(rt::Log::Error, "SDL_GL_CreateContext (OpenGL ES 3.0) failed: %s", SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(g_window, ctx);
  return true;
}

void gl_swap() { SDL_GL_SwapWindow(g_window); }

void gl_set_swap_interval(int interval) { SDL_GL_SetSwapInterval(interval); }

void present_rgba(const uint32_t* bgra, int w, int h) {
  if (headless_test_mode() || frame_dump_mode() || w <= 0 || h <= 0) return;
  ensure_present_window(w, h);
  if (!g_renderer) return;
  ensure_texture(w, h);
  if (SDL_UpdateTexture(g_texture, nullptr, bgra, w * 4) != 0)
    rt::log(rt::Log::Warn, "SDL_UpdateTexture: %s", SDL_GetError());
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
  SDL_RenderPresent(g_renderer);
}

namespace {
void (*g_audio_render)(float*, int) = nullptr;
void audio_callback(void*, Uint8* stream, int len) { g_audio_render((float*)stream, len / (int)(2 * sizeof(float))); }
}  // namespace

bool audio_open(int rate, void (*render)(float* out, int frames)) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    rt::log(rt::Log::Warn, "SDL audio init failed: %s", SDL_GetError());
    return false;
  }
  g_audio_render = render;
  SDL_AudioSpec want{}, have{};
  want.freq = rate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_callback;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev) {
    rt::log(rt::Log::Warn, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  rt::log(rt::Log::Info, "audio: %d Hz, %d-frame buffer", have.freq, have.samples);
  return true;
}

void set_window_size(int width, int height) {
  if (g_window) SDL_SetWindowSize(g_window, width, height);
}

void get_display_size(int* width, int* height) {
  SDL_DisplayMode m;
  if (SDL_GetDesktopDisplayMode(0, &m) == 0) {
    *width = m.w;
    *height = m.h;
  } else {
    *width = 1920;
    *height = 1080;
  }
}

void get_window_size(int* width, int* height) {
  if (g_window) SDL_GetWindowSize(g_window, width, height);
  else *width = 640, *height = 480;
}

void show_cursor(bool visible) { SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE); }

namespace {
void scripted_events(std::vector<InputEvent>& out);
void scripted_pads();

// Controllers fill player slots in the order they are connected.
SDL_GameController* g_controllers[kMaxGamepads] = {};

void controller_added(int device_index) {
  for (auto& c : g_controllers)
    if (c && SDL_JoystickGetDeviceInstanceID(device_index) == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c))) return;
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (g_controllers[slot]) continue;
    g_controllers[slot] = SDL_GameControllerOpen(device_index);
    if (g_controllers[slot]) rt::log(rt::Log::Info, "gamepad: %s is player %d", SDL_GameControllerName(g_controllers[slot]), slot + 1);
    return;
  }
}

void controller_removed(SDL_JoystickID id) {
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    SDL_GameController* c = g_controllers[slot];
    if (c && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) {
      rt::log(rt::Log::Info, "gamepad: player %d's controller disconnected", slot + 1);
      SDL_GameControllerClose(c);
      g_controllers[slot] = nullptr;
    }
  }
}
}  // namespace

void poll_events(std::vector<InputEvent>& out) {
  scripted_events(out);
  scripted_pads();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    InputEvent ev{};
    switch (e.type) {
      case SDL_CONTROLLERDEVICEADDED:
        controller_added(e.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        controller_removed(e.cdevice.which);
        break;
      case SDL_QUIT:
        ev.kind = InputEvent::Quit;
        out.push_back(ev);
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const KeyMap* k = lookup(e.key.keysym.scancode);
        if (!k) break;
        bool down = e.type == SDL_KEYDOWN;
        g_keys[dik_index(k->dik)] = down ? 0x80 : 0;
        ev.kind = down ? InputEvent::KeyDown : InputEvent::KeyUp;
        ev.vk = k->vk;
        ev.scancode = k->dik;
        ev.repeat = e.key.repeat != 0;
        out.push_back(ev);
        break;
      }
      case SDL_TEXTINPUT:
        ev.kind = InputEvent::Char;
        ev.ch = (uint8_t)e.text.text[0];
        out.push_back(ev);
        break;
      case SDL_MOUSEMOTION: {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.dx += e.motion.xrel;
        g_mouse.dy += e.motion.yrel;
        g_mouse_x = e.motion.x;
        g_mouse_y = e.motion.y;
        ev.kind = InputEvent::MouseMove;
        ev.x = e.motion.x;
        ev.y = e.motion.y;
        out.push_back(ev);
        break;
      }
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        uint32_t b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        bool down = e.type == SDL_MOUSEBUTTONDOWN;
        {
          std::lock_guard<std::mutex> lock(g_mouse_mutex);
          g_mouse.buttons[b] = down ? 0x80 : 0;
          g_mouse_x = e.button.x;
          g_mouse_y = e.button.y;
        }
        ev.kind = down ? InputEvent::MouseDown : InputEvent::MouseUp;
        ev.button = b;
        ev.x = e.button.x;
        ev.y = e.button.y;
        out.push_back(ev);
        break;
      }
      case SDL_MOUSEWHEEL: {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.wheel += e.wheel.y * 120;
        ev.kind = InputEvent::MouseWheel;
        ev.dy = e.wheel.y;
        out.push_back(ev);
        break;
      }
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
          // A hidden test window gains and loses focus at random; the game ignores input while
          // inactive, which made scripted runs nondeterministic. Headless runs stay focused.
          if (headless_test_mode() && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) break;
          ev.kind = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? InputEvent::FocusGained : InputEvent::FocusLost;
          if (ev.kind == InputEvent::FocusLost) memset(g_keys, 0, sizeof g_keys);
          out.push_back(ev);
        }
        break;
      default:
        break;
    }
  }
}

const uint8_t* keyboard_state() { return g_keys; }

// ---- Scripted input (automated tests) ----------------------------------------------------------
// BELLA_INPUT="3600:Return,3700:Down+10,..." presses the SDL key named after the colon at that
// presented frame, holding it for 4 frames (or the +N given).
namespace {
struct ScriptedKey {
  uint64_t frame;
  SDL_Scancode sc;
  uint32_t hold;
  bool down_sent = false, up_sent = false;
};
struct ScriptedMouse {
  uint64_t frame;
  // Down/Up = button without auto-release (for drag sequences).
  // Drag = press at (x,y), move to (x2,y2) while held, then release (hold = travel frames).
  enum Kind { Move, ClickLeft, ClickRight, Down, Up, Drag } kind;
  int x = 0, y = 0;
  int x2 = 0, y2 = 0;
  uint32_t hold = 5;
  bool started = false, down_sent = false, up_sent = false;
  bool drag_moving = false;
};
std::vector<ScriptedKey> g_script;
std::vector<ScriptedMouse> g_mouse_script;
std::atomic<uint64_t> g_frame{0};
bool g_script_parsed = false;

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
  // drag(x1,y1,x2,y2)[+travel_frames] — press at start, lerp to end while held, release.
  auto try_drag = [&](const char* prefix, size_t n) -> bool {
    if (stem.rfind(prefix, 0) != 0) return false;
    std::string tail = stem.substr(n);
    if (!tail.empty() && tail.front() == '(' && tail.back() == ')') tail = tail.substr(1, tail.size() - 2);
    int vals[4] = {};
    size_t p = 0;
    for (int i = 0; i < 4; i++) {
      if (p >= tail.size()) return false;
      vals[i] = atoi(tail.c_str() + p);
      if (i == 3) break;
      size_t c = tail.find(',', p);
      if (c == std::string::npos) return false;
      p = c + 1;
    }
    out->kind = ScriptedMouse::Drag;
    out->x = vals[0];
    out->y = vals[1];
    out->x2 = vals[2];
    out->y2 = vals[3];
    out->hold = hold < 2 ? 12 : hold;  // travel duration in frames
    return true;
  };
  if (try_move("move:", 5) || try_move("move", 4)) return true;
  if (try_drag("drag:", 5) || try_drag("drag", 4)) return true;
  if (try_click("down:", 5, ScriptedMouse::Down) || try_click("down", 4, ScriptedMouse::Down)) {
    out->hold = UINT32_MAX;  // stays down until matching up()
    return true;
  }
  if (try_click("up:", 3, ScriptedMouse::Up) || try_click("up", 2, ScriptedMouse::Up)) {
    out->hold = 1;
    return true;
  }
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

void add_script_action(uint64_t frame, std::string action) {
  ScriptedMouse m{};
  m.frame = frame;
  if (parse_mouse_action(action, &m)) {
    g_mouse_script.push_back(m);
    return;
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
  k.sc = SDL_GetScancodeFromName(key_name.c_str());
  if (k.sc == SDL_SCANCODE_UNKNOWN) rt::log(rt::Log::Warn, "scripted input: unknown key '%s'", key_name.c_str());
  else g_script.push_back(k);

}

void parse_script() {
  g_script_parsed = true;
  const char* e = getenv("DETECTIVE_INPUT");
  if (!e) e = getenv("BELLA_INPUT");
  if (!e) return;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = next_script_item(s, pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    uint64_t frame = 0;
    std::string action;
    size_t at = item.find('@');
    if (at != std::string::npos) {
      frame = strtoull(item.c_str() + at + 1, nullptr, 10);
      action = item.substr(0, at);
    } else {
      size_t colon = item.find(':');
      if (colon == std::string::npos) continue;
      frame = strtoull(item.c_str(), nullptr, 10);
      action = item.substr(colon + 1);
    }
    add_script_action(frame, action);
  }
}

void apply_scripted_mouse_pos(int x, int y, std::vector<InputEvent>& out) {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  g_mouse.dx += x - g_mouse_x;
  g_mouse.dy += y - g_mouse_y;
  g_mouse_x = x;
  g_mouse_y = y;
  if (g_window) SDL_WarpMouseInWindow(g_window, x, y);
  InputEvent ev{};
  ev.kind = InputEvent::MouseMove;
  ev.x = x;
  ev.y = y;
  out.push_back(ev);
}

// When Flip stalls (asset load) then resumes, many scripted events become due in one
 // poll. Firing every due mouse click before every due key made OK land before "ab"
 // typing and left the profile chooser wedged. Drain releases first, then start at
 // most one new mouse and one new key per poll, earliest scheduled frame first
 // (keys before mouse when frames tie).
void scripted_mouse_releases(std::vector<InputEvent>& out, uint64_t f) {
  for (auto& m : g_mouse_script) {
    if (m.kind == ScriptedMouse::Down || m.kind == ScriptedMouse::Up || m.kind == ScriptedMouse::Move) continue;
    if (!m.down_sent || m.up_sent) continue;
    if (m.kind == ScriptedMouse::Drag) {
      // Intermediate move while held; release at end position after hold frames.
      if (f > m.frame && f < m.frame + m.hold) {
        m.drag_moving = true;
        float t = (float)(f - m.frame) / (float)m.hold;
        if (t > 1.f) t = 1.f;
        int mx = m.x + (int)((m.x2 - m.x) * t);
        int my = m.y + (int)((m.y2 - m.y) * t);
        apply_scripted_mouse_pos(mx, my, out);
      } else if (f >= m.frame + m.hold) {
        m.up_sent = true;
        apply_scripted_mouse_pos(m.x2, m.y2, out);
        {
          std::lock_guard<std::mutex> lock(g_mouse_mutex);
          g_mouse.buttons[0] = 0;
        }
        InputEvent ev{};
        ev.kind = InputEvent::MouseUp;
        ev.button = 0;
        ev.x = m.x2;
        ev.y = m.y2;
        out.push_back(ev);
      }
      continue;
    }
    if (f >= m.frame + m.hold) {
      m.up_sent = true;
      uint32_t b = m.kind == ScriptedMouse::ClickRight ? 1u : 0u;
      {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.buttons[b] = 0;
      }
      InputEvent ev{};
      ev.kind = InputEvent::MouseUp;
      ev.button = b;
      ev.x = m.x;
      ev.y = m.y;
      out.push_back(ev);
    }
  }
}

static const char* mouse_kind_name(ScriptedMouse::Kind k) {
  switch (k) {
    case ScriptedMouse::Move: return "move";
    case ScriptedMouse::ClickRight: return "rclick";
    case ScriptedMouse::Down: return "down";
    case ScriptedMouse::Up: return "up";
    case ScriptedMouse::Drag: return "drag";
    default: return "click";
  }
}

void scripted_mouse_start_one(std::vector<InputEvent>& out, uint64_t f, uint64_t max_frame) {
  ScriptedMouse* best = nullptr;
  for (auto& m : g_mouse_script) {
    if (m.started || f < m.frame || m.frame > max_frame) continue;
    if (!best || m.frame < best->frame) best = &m;
  }
  if (!best) return;
  // Only start events scheduled for this earliest frame (one click/move).
  uint64_t frame = best->frame;
  for (auto& m : g_mouse_script) {
    if (m.started || m.frame != frame) continue;
    m.started = true;
    apply_scripted_mouse_pos(m.x, m.y, out);
    if (m.kind == ScriptedMouse::Up) {
      {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.buttons[0] = 0;
      }
      InputEvent ev{};
      ev.kind = InputEvent::MouseUp;
      ev.button = 0;
      ev.x = m.x;
      ev.y = m.y;
      out.push_back(ev);
      m.down_sent = true;
      m.up_sent = true;
    } else if (m.kind != ScriptedMouse::Move) {
      uint32_t b = m.kind == ScriptedMouse::ClickRight ? 1u : 0u;
      {
        std::lock_guard<std::mutex> lock(g_mouse_mutex);
        g_mouse.buttons[b] = 0x80;
      }
      InputEvent ev{};
      ev.kind = InputEvent::MouseDown;
      ev.button = b;
      ev.x = m.x;
      ev.y = m.y;
      out.push_back(ev);
      m.down_sent = true;
    }
    rt::log(rt::Log::Info, "scripted input: %s %d,%d at frame %llu", mouse_kind_name(m.kind), m.x, m.y,
            (unsigned long long)f);
    break;  // one new mouse action per poll
  }
}

void scripted_key_releases(uint64_t f, std::vector<InputEvent>& out) {
  for (auto& k : g_script) {
    const KeyMap* m = lookup(k.sc);
    if (!m) continue;
    if (k.down_sent && !k.up_sent && f >= k.frame + k.hold) {
      g_keys[dik_index(m->dik)] = 0;
      InputEvent ev{};
      ev.kind = InputEvent::KeyUp;
      ev.vk = m->vk;
      ev.scancode = m->dik;
      out.push_back(ev);
      k.up_sent = true;
    }
  }
}

void scripted_key_start_one(uint64_t f, uint64_t max_frame, std::vector<InputEvent>& out) {
  ScriptedKey* best = nullptr;
  for (auto& k : g_script) {
    if (k.down_sent || f < k.frame || k.frame > max_frame) continue;
    if (!lookup(k.sc)) continue;
    if (!best || k.frame < best->frame) best = &k;
  }
  if (!best) return;
  const KeyMap* m = lookup(best->sc);
  g_keys[dik_index(m->dik)] = 0x80;
  InputEvent ev{};
  ev.kind = InputEvent::KeyDown;
  ev.vk = m->vk;
  ev.scancode = m->dik;
  out.push_back(ev);
  best->down_sent = true;
  rt::log(rt::Log::Info, "scripted input: %s down at frame %llu", SDL_GetScancodeName(best->sc), (unsigned long long)f);
}

void scripted_events(std::vector<InputEvent>& out) {
  if (!g_script_parsed) parse_script();
  uint64_t f = g_frame.load();
  {
    std::lock_guard<std::mutex> lock(g_ctl_mutex);
    for (auto& a : g_ctl_pending) add_script_action(f + 1, a);
    g_ctl_pending.clear();
  }
  scripted_mouse_releases(out, f);
  scripted_key_releases(f, out);

  // poll_events may run many times per Flip; only start one new action per presented frame.
  static uint64_t s_last_start_at = UINT64_MAX;
  if (f == s_last_start_at) return;

  uint64_t next_mouse = UINT64_MAX, next_key = UINT64_MAX;
  for (auto& m : g_mouse_script) {
    if (!m.started && f >= m.frame && m.frame < next_mouse) next_mouse = m.frame;
  }
  for (auto& k : g_script) {
    if (!k.down_sent && f >= k.frame && lookup(k.sc) && k.frame < next_key) next_key = k.frame;
  }
  if (next_key == UINT64_MAX && next_mouse == UINT64_MAX) return;
  // Keys before mouse on a tie so "ab" lands before OK when a catch-up poll collapses them.
  if (next_key <= next_mouse) scripted_key_start_one(f, next_key, out);
  else scripted_mouse_start_one(out, f, next_mouse);
  s_last_start_at = f;
}

// BELLA_PAD="600:p0.ly=-1+90,700:p0.a,..." drives virtual gamepads by presented frame:
// pN.<button>[+hold] presses a button (a b x y lb rb lt rt back start ls rs up down left right),
// pN.<axis>=v[+hold] holds a stick axis (lx ly rx ry, -1..1, +y down) or trigger (lt rt, 0..1),
// pN.connect / pN.disconnect plug the pad in or out. A pad named in the script starts connected.
struct ScriptedPad {
  uint64_t frame;
  int slot;
  std::string control;
  float value;
  uint32_t hold;
  bool started = false, ended = false;
};
std::vector<ScriptedPad> g_pad_script;
GamepadState g_scripted[kMaxGamepads];
bool g_pad_scripted[kMaxGamepads] = {};
bool g_pad_script_parsed = false;

uint32_t pad_button(const std::string& n) {
  static const std::pair<const char*, uint32_t> names[] = {
      {"a", PadA}, {"b", PadB}, {"x", PadX}, {"y", PadY}, {"lb", PadLB}, {"rb", PadRB}, {"back", PadBack},
      {"start", PadStart}, {"ls", PadLS}, {"rs", PadRS}, {"up", PadUp}, {"down", PadDown}, {"left", PadLeft},
      {"right", PadRight}};
  for (auto& [name, bit] : names)
    if (n == name) return bit;
  return 0;
}

float* pad_axis(GamepadState& p, const std::string& n) {
  if (n == "lx") return &p.lx;
  if (n == "ly") return &p.ly;
  if (n == "rx") return &p.rx;
  if (n == "ry") return &p.ry;
  if (n == "lt") return &p.lt;
  if (n == "rt") return &p.rt;
  return nullptr;
}

void parse_pad_script() {
  g_pad_script_parsed = true;
  const char* e = getenv("BELLA_PAD");
  if (!e) return;
  std::string s = e;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? s.size() : end + 1;
    ScriptedPad k{};
    size_t colon = item.find(':'), dot = item.find('.');
    if (colon == std::string::npos || dot == std::string::npos || dot < colon + 3 || item[colon + 1] != 'p') {
      rt::log(rt::Log::Warn, "BELLA_PAD: cannot parse '%s'", item.c_str());
      continue;
    }
    k.frame = strtoull(item.c_str(), nullptr, 10);
    k.slot = atoi(item.c_str() + colon + 2);
    std::string rest = item.substr(dot + 1);
    k.hold = 4;
    k.value = 1;
    if (size_t plus = rest.find('+'); plus != std::string::npos) {
      k.hold = (uint32_t)atoi(rest.c_str() + plus + 1);
      rest = rest.substr(0, plus);
    }
    if (size_t eq = rest.find('='); eq != std::string::npos) {
      k.value = strtof(rest.c_str() + eq + 1, nullptr);
      rest = rest.substr(0, eq);
    }
    k.control = rest;
    if (k.slot < 0 || k.slot >= kMaxGamepads) continue;
    GamepadState probe;
    if (!pad_button(rest) && !pad_axis(probe, rest) && rest != "connect" && rest != "disconnect") {
      rt::log(rt::Log::Warn, "BELLA_PAD: unknown control '%s'", rest.c_str());
      continue;
    }
    g_pad_script.push_back(k);
    g_pad_scripted[k.slot] = true;
    g_scripted[k.slot].connected = true;
  }
}

void scripted_pads() {
  if (!g_pad_script_parsed) parse_pad_script();
  uint64_t f = g_frame.load();
  for (auto& k : g_pad_script) {
    GamepadState& p = g_scripted[k.slot];
    if (!k.started && f >= k.frame) {
      k.started = true;
      if (k.control == "connect" || k.control == "disconnect") {
        p = GamepadState{};
        p.connected = k.control == "connect";
        k.ended = true;
      } else if (uint32_t bit = pad_button(k.control)) {
        p.buttons |= bit;
      } else {
        *pad_axis(p, k.control) = k.value;
      }
      rt::log(rt::Log::Info, "scripted pad %d: %s=%g at frame %llu", k.slot, k.control.c_str(), k.value, (unsigned long long)f);
    } else if (k.started && !k.ended && f >= k.frame + k.hold) {
      k.ended = true;
      if (uint32_t bit = pad_button(k.control)) p.buttons &= ~bit;
      else *pad_axis(p, k.control) = 0;
    }
  }
}
}  // namespace

void frame_presented(uint64_t frame) {
  g_frame = frame;
  rt::note_present();
}

GamepadState gamepad_state(int slot) {
  GamepadState p;
  if (slot < 0 || slot >= kMaxGamepads) return p;
  if (g_pad_scripted[slot]) return g_scripted[slot];
  SDL_GameController* c = g_controllers[slot];
  if (!c || !SDL_GameControllerGetAttached(c)) return p;
  p.connected = true;
  static const std::pair<SDL_GameControllerButton, uint32_t> buttons[] = {
      {SDL_CONTROLLER_BUTTON_A, PadA}, {SDL_CONTROLLER_BUTTON_B, PadB}, {SDL_CONTROLLER_BUTTON_X, PadX},
      {SDL_CONTROLLER_BUTTON_Y, PadY}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PadLB},
      {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PadRB}, {SDL_CONTROLLER_BUTTON_BACK, PadBack},
      {SDL_CONTROLLER_BUTTON_START, PadStart}, {SDL_CONTROLLER_BUTTON_LEFTSTICK, PadLS},
      {SDL_CONTROLLER_BUTTON_RIGHTSTICK, PadRS}, {SDL_CONTROLLER_BUTTON_DPAD_UP, PadUp},
      {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PadDown}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PadLeft},
      {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PadRight}};
  for (auto& [b, bit] : buttons)
    if (SDL_GameControllerGetButton(c, b)) p.buttons |= bit;
  auto axis = [c](SDL_GameControllerAxis a) { return std::max(-1.f, SDL_GameControllerGetAxis(c, a) / 32767.f); };
  p.lx = axis(SDL_CONTROLLER_AXIS_LEFTX), p.ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
  p.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), p.ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
  p.lt = axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), p.rt = axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  if (p.lt > 0.5f) p.buttons |= PadLT;
  if (p.rt > 0.5f) p.buttons |= PadRT;
  return p;
}

void gamepad_rumble(int slot, float strong, float weak) {
  if (slot < 0 || slot >= kMaxGamepads) return;
  if (g_pad_scripted[slot]) {
    rt::log(rt::Log::Info, "scripted pad %d: rumble strong=%.3f weak=%.3f", slot, strong, weak);
    return;
  }
  // The game keeps an effect playing until it stops it; SDL needs a duration.
  if (SDL_GameController* c = g_controllers[slot])
    SDL_GameControllerRumble(c, (Uint16)(std::clamp(strong, 0.f, 1.f) * 65535), (Uint16)(std::clamp(weak, 0.f, 1.f) * 65535), 10000);
}

MouseState take_mouse_state() {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  MouseState m = g_mouse;
  g_mouse.dx = g_mouse.dy = g_mouse.wheel = 0;
  return m;
}

void mouse_pos(int* x, int* y) {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  if (x) *x = g_mouse_x;
  if (y) *y = g_mouse_y;
}

// SetCursorPos moves the pointer without producing DirectInput motion.
void set_mouse_pos(int x, int y) {
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  g_mouse_x = x;
  g_mouse_y = y;
  if (g_window) SDL_WarpMouseInWindow(g_window, x, y);
}

bool mouse_button_down(int button) {
  if (button < 0 || button >= 4) return false;
  std::lock_guard<std::mutex> lock(g_mouse_mutex);
  return (g_mouse.buttons[button] & 0x80) != 0;
}

}  // namespace plat
