// BDETECT.EXE Win32 import gaps: 16-bit file I/O, Global*, threads, GDI DIB/print stubs.
#include <ctime>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>

#include "../core/objects.h"
#include "../platform/platform.h"
#include "ddraw.h"
#include "registry.h"

using namespace rt;

void hle_link_win32_extra() {}

std::mutex g_lfile_mutex;
std::map<int32_t, std::shared_ptr<FileObject>> g_lfiles;
std::map<int32_t, int64_t> g_lfile_pos;

namespace {

const char* kKernel32 = "kernel32.dll";
const char* kUser32 = "user32.dll";
const char* kGdi32 = "gdi32.dll";
const char* kComdlg32 = "comdlg32.dll";

constexpr int32_t HFILE_ERROR = -1;
constexpr uint32_t GMEM_FIXED = 0x0000;
constexpr uint32_t GMEM_MOVEABLE = 0x0002;
constexpr uint32_t GMEM_ZEROINIT = 0x0040;
struct GlobalBlock {
  uint32_t size = 0;
  uint32_t mem = 0;
  uint32_t lock_count = 0;
};

std::mutex g_global_mutex;
std::map<uint32_t, GlobalBlock> g_global;
uint32_t g_next_global = 0x00020000;

int32_t hfile_from_handle(uint32_t h) {
  if (h == kInvalidHandle) return HFILE_ERROR;
  return (int32_t)h;
}

uint32_t global_alloc_block(uint32_t flags, uint32_t size) {
  if (!size) size = 1;
  uint32_t mem = heap_alloc(process_heap(), size, (flags & GMEM_ZEROINIT) != 0);
  if (!mem) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  const bool moveable = (flags & GMEM_MOVEABLE) != 0;
  std::lock_guard<std::mutex> lock(g_global_mutex);
  if (moveable) {
    uint32_t h = g_next_global;
    g_next_global += 4;
    g_global[h] = {size, mem, 0};
    return h;
  }
  // GMEM_FIXED (default when GMEM_MOVEABLE is clear): Win32 returns the pointer directly.
  g_global[mem] = {size, mem, 0};
  return mem;
}

// ---- kernel32: strings, version, time ------------------------------------------------------

HLE_STDCALL(kKernel32, lstrlenA, 1) {
  uint32_t p = arg(c, 0);
  if (!p) return 0;
  return (uint32_t)strlen((const char*)host(p));
}

HLE_STDCALL(kKernel32, GetVersion, 0) { return 0x00000004; }  // Win32 4.0

HLE_STDCALL(kKernel32, GetLocalTime, 1) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  uint32_t p = arg(c, 0);
  st16(p + 0, (uint32_t)(tm.tm_year + 1900));
  st16(p + 2, (uint32_t)(tm.tm_mon + 1));
  st16(p + 4, (uint32_t)tm.tm_wday);
  st16(p + 6, (uint32_t)tm.tm_mday);
  st16(p + 8, (uint32_t)tm.tm_hour);
  st16(p + 10, (uint32_t)tm.tm_min);
  st16(p + 12, (uint32_t)tm.tm_sec);
  st16(p + 14, 0);
  return 0;
}

// ---- kernel32: Global* ---------------------------------------------------------------------

HLE_STDCALL(kKernel32, GlobalAlloc, 2) { return global_alloc_block(arg(c, 0), arg(c, 1)); }

HLE_STDCALL(kKernel32, GlobalReAlloc, 3) {
  uint32_t h = arg(c, 0), size = arg(c, 1), flags = arg(c, 2);
  std::lock_guard<std::mutex> lock(g_global_mutex);
  auto it = g_global.find(h);
  if (it == g_global.end()) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  const bool fixed = it->second.mem == h;
  if (!size) {
    heap_free(process_heap(), it->second.mem);
    g_global.erase(it);
    return 0;
  }
  const uint32_t old_mem = it->second.mem;
  const uint32_t lock_count = it->second.lock_count;
  uint32_t mem = heap_realloc(process_heap(), old_mem, size, (flags & GMEM_ZEROINIT) != 0);
  if (!mem) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  if (fixed && mem != h) {
    g_global.erase(it);
    g_global[mem] = {size, mem, lock_count};
    return mem;
  }
  it->second.mem = mem;
  it->second.size = size;
  return fixed ? mem : h;
}

HLE_STDCALL(kKernel32, GlobalFree, 1) {
  uint32_t h = arg(c, 0);
  std::lock_guard<std::mutex> lock(g_global_mutex);
  auto it = g_global.find(h);
  if (it == g_global.end()) return h;
  heap_free(process_heap(), it->second.mem);
  g_global.erase(it);
  return 0;
}

HLE_STDCALL(kKernel32, GlobalLock, 1) {
  uint32_t h = arg(c, 0);
  std::lock_guard<std::mutex> lock(g_global_mutex);
  auto it = g_global.find(h);
  if (it == g_global.end()) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  it->second.lock_count++;
  return it->second.mem;
}

HLE_STDCALL(kKernel32, GlobalUnlock, 1) {
  uint32_t h = arg(c, 0);
  std::lock_guard<std::mutex> lock(g_global_mutex);
  auto it = g_global.find(h);
  if (it == g_global.end()) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (it->second.lock_count == 0) return 0;  // not locked
  it->second.lock_count--;
  return it->second.lock_count == 0 ? 1 : 0;
}

HLE_STDCALL(kKernel32, GlobalHandle, 1) {
  uint32_t p = arg(c, 0);
  std::lock_guard<std::mutex> lock(g_global_mutex);
  for (auto& [h, b] : g_global)
    if (b.mem == p) return h;
  set_last_error(ERROR_INVALID_HANDLE);
  return 0;
}

// ---- kernel32: OpenFile / _l* (VFS-backed) ---------------------------------------------------

static uint32_t open_file_vfs(const std::string& guest, bool write) {
  std::string hostp = vfs_map_host(guest, write);
  if (hostp.empty()) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return kInvalidHandle;
  }
  const char* mode = write ? "r+b" : "rb";
  FILE* fp = fopen(hostp.c_str(), mode);
  if (!fp) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return kInvalidHandle;
  }
  auto f = std::make_shared<FileObject>();
  f->fp = fp;
  f->guest_path = guest;
  f->host_path = hostp;
  f->writable = write;
  return handle_new(f);
}

HLE_STDCALL(kKernel32, OpenFile, 3) {
  uint32_t path = arg(c, 0), of = arg(c, 1), style = arg(c, 2);
  if (of) memset(host(of), 0, 148);
  std::string guest = read_cstr(path);
  bool write = (style & 0x0001) != 0 || (style & 0x0002) != 0;
  uint32_t h = open_file_vfs(guest, write);
  if (h == kInvalidHandle) {
    if (of) st16(of + 2, 2);  // OFS_ERRPATHNOTFOUND
    return (uint32_t)HFILE_ERROR;
  }
  int32_t hf = hfile_from_handle(h);
  {
    std::lock_guard<std::mutex> lock(g_lfile_mutex);
    g_lfiles[hf] = handle_as<FileObject>(h);
    g_lfile_pos[hf] = 0;
  }
  if (of) {
    host(of)[0] = 12;
    write_cstr(of + 8, guest);
  }
  if (trace_enabled("file")) {
    std::string hostp = vfs_map_host(guest, write);
    log(Log::Info, "OpenFile(%s, write=%d) -> hf %d (%s)", guest.c_str(), write ? 1 : 0, hf,
        hostp.empty() ? "?" : hostp.c_str());
  }
  return (uint32_t)hf;
}

HLE_STDCALL(kKernel32, _lread, 3) {
  int32_t hf = (int32_t)arg(c, 0);
  uint32_t buf = arg(c, 1), n = arg(c, 2);
  std::lock_guard<std::mutex> lock(g_lfile_mutex);
  auto it = g_lfiles.find(hf);
  if (it == g_lfiles.end() || !it->second || !it->second->fp) return (uint32_t)HFILE_ERROR;
  int64_t pos = g_lfile_pos[hf];
  fseeko(it->second->fp, (off_t)pos, SEEK_SET);
  size_t got = n ? fread(host(buf), 1, n, it->second->fp) : 0;
  g_lfile_pos[hf] = pos + (int64_t)got;
  return (uint32_t)got;
}

HLE_STDCALL(kKernel32, _llseek, 3) {
  int32_t hf = (int32_t)arg(c, 0);
  int32_t off = (int32_t)arg(c, 1);
  int origin = (int)arg(c, 2);
  std::lock_guard<std::mutex> lock(g_lfile_mutex);
  auto it = g_lfiles.find(hf);
  if (it == g_lfiles.end() || !it->second || !it->second->fp) return (uint32_t)HFILE_ERROR;
  int64_t base = origin == 1 ? g_lfile_pos[hf] : 0;
  if (origin == 2) {
    fseeko(it->second->fp, 0, SEEK_END);
    base = ftello(it->second->fp);
  }
  int64_t np = base + off;
  if (np < 0) return (uint32_t)HFILE_ERROR;
  g_lfile_pos[hf] = np;
  if (fseeko(it->second->fp, (off_t)np, SEEK_SET) != 0) return (uint32_t)HFILE_ERROR;
  return (uint32_t)np;
}

HLE_STDCALL(kKernel32, _lclose, 1) {
  int32_t hf = (int32_t)arg(c, 0);
  std::lock_guard<std::mutex> lock(g_lfile_mutex);
  g_lfiles.erase(hf);
  g_lfile_pos.erase(hf);
  return handle_close((uint32_t)hf) ? 0 : HFILE_ERROR;
}

HLE_STDCALL(kKernel32, DeleteFileA, 1) {
  std::string guest = read_cstr(arg(c, 0));
  return vfs_map_delete(guest) ? 1u : 0u;
}

// ---- kernel32: threads ---------------------------------------------------------------------

HLE_STDCALL(kKernel32, ExitThread, 1) {
  uint32_t code = arg(c, 0);
  GuestThread* t = current_thread();
  if (t) {
    t->exit_code = code;
    t->finished.store(true);
  }
  pthread_exit(nullptr);
  return 0;
}

HLE_STDCALL(kKernel32, TerminateThread, 2) {
  auto to = handle_as<ThreadObject>(arg(c, 0));
  if (!to || !to->thread) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  to->thread->exit_code = arg(c, 1);
  to->thread->finished.store(true);
  return 1;
}

HLE_STDCALL(kKernel32, SuspendThread, 1) {
  auto to = handle_as<ThreadObject>(arg(c, 0));
  if (!to || !to->thread) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;
  }
  if (!to->suspended) {
    to->suspended = true;
    if (to->host_running) to->thread->paused.store(true);
    return 1;
  }
  return 0;
}

// ---- user32 --------------------------------------------------------------------------------

static uint32_t impl_VkKeyScanA(Cpu* c) {
  int ch = (char)arg(c, 0);
  if (ch >= 'a' && ch <= 'z') return (uint32_t)(ch - 'a' + 'A');
  if (ch >= 'A' && ch <= 'Z') return (uint32_t)((ch - 'A' + 'A') | (1 << 8));
  if (ch >= '0' && ch <= '9') return (uint32_t)ch;
  if (ch == ' ') return 0x20;
  return 0xffffffffu;
}

HLE_STDCALL(kUser32, VkKeyScanA, 1) { return impl_VkKeyScanA(c); }

// The game posts WM_KEYDOWN/VK_ESCAPE to its own window to quit from the end screen.
HLE_STDCALL(kUser32, PostMessageA, 4) {
  user32_post(arg(c, 1), arg(c, 2), arg(c, 3));
  return 1;
}

HLE_STDCALL(kUser32, WaitMessage, 0) {
  for (int i = 0; i < 100; i++) {
    user32_pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 1;
}

HLE_STDCALL(kUser32, DestroyWindow, 1) {
  (void)arg(c, 0);
  return 1;
}

static uint32_t impl_wsprintfA(Cpu* c) {
  uint32_t out = arg(c, 0);
  uint32_t fmt = arg(c, 1);
  if (!fmt) return 0;
  std::string f = read_cstr(fmt);
  char buf[2048];
  uint32_t a0 = arg(c, 2);
  uint32_t a1 = arg(c, 3);
  int n = std::snprintf(buf, sizeof(buf), f.c_str(), a0, a1);
  if (n < 0) n = 0;
  if (out) write_cstr(out, std::string(buf, (size_t)std::min(n, (int)sizeof(buf) - 1)));
  return (uint32_t)n;
}

static void entry_wsprintfA(Cpu* c) {
  c->eax = impl_wsprintfA(c);
  c->esp += 4;  // cdecl: caller pops three arguments
}

static HleRegistrar reg_wsprintfA(kUser32, "wsprintfA", entry_wsprintfA);

// ---- gdi32 ---------------------------------------------------------------------------------

enum : int {
  HORZRES = 8,
  VERTRES = 10,
  BITSPIXEL = 12,
  PLANES = 14,
  NUMCOLORS = 24,
  RASTERCAPS = 38,
  SIZEPALETTE = 104,
};
enum : uint32_t {
  RC_PALETTE = 0x0001,
  RC_BITBLT = 0x0001,
  RC_STRETCHDIB = 0x2000,
  RC_BITMAP64 = 0x8000,
};

HLE_STDCALL(kGdi32, SetDIBitsToDevice, 12) {
  uint32_t hdc = arg(c, 0);
  int xDest = (int)arg(c, 1), yDest = (int)arg(c, 2);
  int w = (int)arg(c, 3), h = (int)arg(c, 4);
  int xSrc = (int)arg(c, 5), ySrc = (int)arg(c, 6);
  uint32_t bits = arg(c, 9), bmi = arg(c, 10);
  (void)hdc;
  (void)arg(c, 7);
  (void)arg(c, 8);
  (void)arg(c, 11);
  ddraw::gdi_blit_dib(xDest, yDest, w, h, xSrc, ySrc, w, h, bits, bmi, true);
  return 1;
}

HLE_STDCALL(kGdi32, StretchDIBits, 13) {
  int xDest = (int)arg(c, 1), yDest = (int)arg(c, 2);
  int wDest = (int)arg(c, 3), hDest = (int)arg(c, 4);
  int xSrc = (int)arg(c, 5), ySrc = (int)arg(c, 6);
  int wSrc = (int)arg(c, 7), hSrc = (int)arg(c, 8);
  uint32_t bits = arg(c, 10), bmi = arg(c, 11);
  ddraw::gdi_blit_dib(xDest, yDest, wDest, hDest, xSrc, ySrc, wSrc, hSrc, bits, bmi, true);
  return hDest;
}

HLE_STDCALL(kGdi32, SetBkMode, 2) {
  ddraw::gdi_set_bk_mode(arg(c, 0), (int)arg(c, 1));
  return arg(c, 1);
}
HLE_STDCALL(kGdi32, SetTextColor, 2) { return ddraw::gdi_set_text_color(arg(c, 0), arg(c, 1)); }
HLE_STDCALL(kGdi32, TextOutA, 5) {
  uint32_t hdc = arg(c, 0);
  int x = (int)arg(c, 1), y = (int)arg(c, 2);
  uint32_t str = arg(c, 3);
  int len = (int)arg(c, 4);
  if (len <= 0 && str) len = (int)strlen((const char*)host(str));
  ddraw::gdi_text_out(hdc, x, y, str, len);
  return 1;
}
HLE_STDCALL(kGdi32, CreateFontA, 14) {
  int height = (int)arg(c, 0);
  int weight = (int)arg(c, 4);
  (void)arg(c, 1);
  (void)arg(c, 2);
  (void)arg(c, 3);
  (void)arg(c, 5);
  (void)arg(c, 6);
  (void)arg(c, 7);
  (void)arg(c, 8);
  (void)arg(c, 9);
  (void)arg(c, 10);
  (void)arg(c, 11);
  (void)arg(c, 12);
  (void)arg(c, 13);
  return ddraw::gdi_create_font(height, weight);
}
HLE_STDCALL(kGdi32, SelectObject, 2) {
  uint32_t hdc = arg(c, 0), obj = arg(c, 1);
  if (obj >= 0x00020010u) return ddraw::gdi_select_font(hdc, obj);
  return obj;
}
HLE_STDCALL(kGdi32, GetCharWidthA, 4) {
  uint32_t hdc = arg(c, 0), first = arg(c, 1), last = arg(c, 2), widths = arg(c, 3);
  return ddraw::gdi_get_char_widths(hdc, first, last, widths) ? 1 : 0;
}
HLE_STDCALL(kGdi32, DeleteDC, 1) { return 1; }
HLE_STDCALL(kGdi32, GetStockObject, 1) { return 0x00020002; }
HLE_STDCALL(kGdi32, StartDocA, 2) { return 1; }
HLE_STDCALL(kGdi32, StartPage, 1) { return 1; }
HLE_STDCALL(kGdi32, EndPage, 1) { return 1; }
HLE_STDCALL(kGdi32, EndDoc, 1) { return 1; }

// ---- comdlg32 ------------------------------------------------------------------------------

HLE_STDCALL(kComdlg32, PrintDlgA, 1) {
  set_last_error(1223u);  // ERROR_CANCELLED
  return 0;
}

}  // namespace

namespace rt {

bool lfile_pos_get(uint32_t handle, int64_t* pos) {
  int32_t hf = (int32_t)handle;
  std::lock_guard<std::mutex> lock(g_lfile_mutex);
  auto it = g_lfile_pos.find(hf);
  if (it == g_lfile_pos.end()) return false;
  if (pos) *pos = it->second;
  return true;
}

void lfile_pos_set(uint32_t handle, int64_t pos) {
  int32_t hf = (int32_t)handle;
  std::lock_guard<std::mutex> lock(g_lfile_mutex);
  if (g_lfiles.find(hf) == g_lfiles.end()) return;
  g_lfile_pos[hf] = pos;
}

}  // namespace rt
