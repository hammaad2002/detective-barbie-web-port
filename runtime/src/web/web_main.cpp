// Web entry point. With PROXY_TO_PTHREAD, main() runs on a pthread and becomes the game's main
// thread; the page's main thread calls web_render_init() once and web_render_tick() every
// animation frame to compose DirectDraw surfaces and blit them to a 2D canvas.
#include <emscripten.h>
#include <emscripten/wasmfs.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

#include "../core/objects.h"
#include "../core/rt.h"
#include "../hle/ddraw.h"
#include "../hle/registry.h"
#include "../platform/platform.h"
#include "web.h"

namespace web {
namespace {

struct Batch {
  std::vector<uint32_t> words;
  bool present;
  uint64_t seq;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Batch> g_queue;
uint64_t g_submitted = 0, g_executed = 0, g_frames_queued = 0, g_frames_done = 0;
std::atomic<bool> g_gl{false};

std::mutex g_present_mutex;
std::condition_variable g_present_cv;
uint64_t g_presented_frame = 0;

uint64_t enqueue(std::vector<uint32_t>&& words, bool present) {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint64_t seq = ++g_submitted;
  g_queue.push_back({std::move(words), present, seq});
  if (present) g_frames_queued++;
  return seq;
}

}  // namespace

bool gl_ready() { return g_gl; }

void submit(std::vector<uint32_t>&& words, bool present) {
  enqueue(std::move(words), present);
  if (!present) return;
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [] { return g_frames_queued - g_frames_done <= 1; });
}

void submit_and_wait(std::vector<uint32_t>&& words) {
  uint64_t seq = enqueue(std::move(words), false);
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [&] { return g_executed >= seq; });
}

void present_wait() {
  const uint64_t target = ddraw::frame_counter();
  if (target == 0) return;
  std::unique_lock<std::mutex> lock(g_present_mutex);
  g_present_cv.wait(lock, [&] { return g_presented_frame >= target; });
}

void flip_paced() {
  EM_ASM({
    if (Module.__perfNative) Module.__perfNative.guestFlips++;
  });
  const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  static std::atomic<uint64_t> last_vsync{0};
  const uint64_t last = last_vsync.load();
  constexpr uint64_t kFrameUs = 1000000 / 60;
  if (last && now > last) {
    const uint64_t elapsed = now - last;
    if (elapsed < kFrameUs) {
      const uint32_t wait_ms = (uint32_t)((kFrameUs - elapsed + 999) / 1000);
      if (wait_ms) std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
  }
  last_vsync.store((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
}

uint64_t g_last_composed = 0;
alignas(64) static uint8_t g_frame_rgba[640 * 480 * 4];
static uint8_t* g_present_ptr = nullptr;
static int g_present_w = 0, g_present_h = 0;
static bool g_have_frame = false;

}  // namespace web

extern "C" {

// Called once on the page's main thread after the canvas exists.
EMSCRIPTEN_KEEPALIVE int web_render_init(const char* canvas_selector) {
  const int ok = EM_ASM_INT(
      {
        const sel = UTF8ToString($0);
        const canvas = document.querySelector(sel);
        if (!canvas) return 0;
        Module.__presentSelector = sel;
        Module.__presentCanvas = canvas;
        Module.__presentCtx = canvas.getContext("2d", { alpha: false });
        if (!Module.__presentCtx) return 0;
        Module.__presentOffscreen = document.createElement("canvas");
        Module.__presentOffCtx = Module.__presentOffscreen.getContext("2d", { alpha: false });
        Module.__perfNative = {};
        Module.__perfNative.composeUs = 0;
        Module.__perfNative.composeN = 0;
        Module.__perfNative.uploadUs = 0;
        Module.__perfNative.uploadN = 0;
        Module.__perfNative.guestFlips = 0;
        Module.__ddrawFrameCounter = 0;
        return Module.__presentOffCtx ? 1 : 0;
      },
      canvas_selector);
  if (!ok) {
    rt::log(rt::Log::Error, "2D canvas is not available");
    return 0;
  }
  web::g_gl = true;
  return 1;
}

// Called every animation frame on the page's main thread: compose at most one new game frame.
EMSCRIPTEN_KEEPALIVE int web_render_tick() {
  const uint64_t fc = ddraw::frame_counter();
  EM_ASM({ Module.__ddrawFrameCounter = $0; }, (double)fc);
  // Game frame statistics for tools/web-fps.mjs: [flips, primaryBlts, cursorPolls, sleeps, locks].
  EM_ASM({ (Module.__gameStats = Module.__gameStats || [])[$0] = $1; }, 0, (double)rt::g_stats[rt::StatFlips]);
  EM_ASM({ Module.__gameStats[$0] = $1; }, 1, (double)rt::g_stats[rt::StatPrimaryBlts]);
  EM_ASM({ Module.__gameStats[$0] = $1; }, 2, (double)rt::g_stats[rt::StatCursorPolls]);
  EM_ASM({ Module.__gameStats[$0] = $1; }, 3, (double)rt::g_stats[rt::StatSleeps]);
  EM_ASM({ Module.__gameStats[$0] = $1; }, 4, (double)rt::g_stats[rt::StatLocks]);
  // Compose every tick: the game also draws through Lock/Unlock and GDI without bumping the counter.
  if (fc > 0) {
    int w = 0, h = 0;
    uint8_t* buf = nullptr;
    if (ddraw::compose_rgba8(web::g_frame_rgba, (int)sizeof web::g_frame_rgba, w, h, &buf) && w > 0 && h > 0) {
      if (!buf) buf = web::g_frame_rgba;
      web::g_present_ptr = buf;
      web::g_present_w = w;
      web::g_present_h = h;
      web::g_have_frame = true;
      web::g_last_composed = fc;
      EM_ASM({
        const p = Module.__perfNative;
        if (p) {
          p.composeN++;
          p.composeUs += $0;
        }
      }, (double)ddraw::last_compose_us());
      plat::frame_presented(fc);
      {
        std::lock_guard<std::mutex> lock(web::g_present_mutex);
        if (fc > web::g_presented_frame) web::g_presented_frame = fc;
      }
      web::g_present_cv.notify_all();
    }
  }
  if (!web::g_have_frame || !web::g_present_ptr) return 0;
  plat::present_rgba8(web::g_present_ptr, web::g_present_w, web::g_present_h);
  return 1;
}

}  // extern "C"

// argv: [install-dir] [registry-file]; the page mounts OPFS before starting the module.
int main(int argc, char** argv) {
  std::string install = argc > 1 ? argv[1] : "/opfs/bella";
  std::string reg = argc > 2 ? argv[2] : install + "/registry.txt";
  if (install.rfind("/opfs", 0) == 0) {
    backend_t opfs = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/opfs", 0777, opfs) != 0) rt::fatal("cannot mount OPFS at /opfs");
  }
  rt::memory_init();
  rt::vfs_init(install);
  if (install.rfind("/opfs/", 0) == 0) {
    std::string saves = install.substr(0, install.find_last_of('/')) + "/saves";
    rt::vfs_overlay_set_root(saves);
  }
  rt::registry_init(reg);
  plat::init();
  std::ifstream f(install + "/BDETECT.EXE", std::ios::binary);
  if (!f) rt::fatal("cannot open %s/BDETECT.EXE (is the game installed?)", install.c_str());
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  rt::load_image(exe);
  rt::dispatch_init();
  rt::watchdog_start();
  rt::GuestThread* t = rt::create_guest_thread(0x100000);
  rt::bind_host_thread(t);
  rt::log(rt::Log::Info, "starting guest at entry %08x", rt::image_entry());
  uint32_t code = rt::guest_call(&t->cpu, rt::image_entry(), {});
  rt::process_exit(code);
}
