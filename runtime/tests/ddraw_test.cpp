// DirectDraw HLE self-test (native, no window).
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/rt.h"
#include "hle/com.h"
#include "hle/ddraw.h"
#include "platform/platform.h"
#include "recomp.h"

extern "C" {
const GuestFnEntry recomp_functions[1] = {{0, nullptr}};
const uint32_t recomp_function_count = 0;

typedef void* mspace;
namespace {
mspace g_test_mspace = reinterpret_cast<mspace>(1);
uint32_t g_test_heap_next = rt::kHeapBase;
void* test_heap_alloc(size_t bytes) {
  uint32_t align = 16;
  uint32_t a = (g_test_heap_next + align - 1) & ~(align - 1);
  if (a + bytes > rt::kHeapEnd) return nullptr;
  g_test_heap_next = a + (uint32_t)bytes;
  return rt::host(a);
}
}  // namespace
mspace create_mspace_with_base(void*, size_t, int) { return g_test_mspace; }
void* mspace_malloc(mspace m, size_t bytes) { return m ? test_heap_alloc(bytes) : nullptr; }
void* mspace_calloc(mspace m, size_t n, size_t sz) {
  size_t bytes = n * sz;
  void* p = mspace_malloc(m, bytes);
  if (p) memset(p, 0, bytes);
  return p;
}
void* mspace_realloc(mspace m, void* mem, size_t bytes) {
  if (!mem) return mspace_malloc(m, bytes);
  return mspace_malloc(m, bytes);
}
void mspace_free(mspace, void*) {}
void mspace_mallinfo(mspace) {}
}

void hle_link_ddraw();

namespace {

constexpr unsigned kDdCreateSurface = 6;
constexpr unsigned kDdSetDisplayMode = 21;
constexpr unsigned kDdCreatePalette = 5;

constexpr unsigned kSfcLock = 25;
constexpr unsigned kSfcSetPalette = 31;
constexpr unsigned kSfcBltFast = 7;
constexpr unsigned kSfcSetColorKey = 29;

constexpr uint32_t DDSCAPS_COMPLEX = 0x08;
constexpr uint32_t DDSCAPS_FLIP = 0x10;
constexpr uint32_t DDSCAPS_PRIMARYSURFACE = 0x200;
constexpr uint32_t DDSCAPS_OFFSCREENPLAIN = 0x40;
constexpr uint32_t DDSCAPS_SYSTEMMEMORY = 0x800;

constexpr uint32_t DDSD_CAPS = 0x01;
constexpr uint32_t DDSD_HEIGHT = 0x02;
constexpr uint32_t DDSD_WIDTH = 0x04;
constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t DDSD_BACKBUFFERCOUNT = 0x20;

constexpr uint32_t DDPF_PALETTEINDEXED8 = 0x20;
constexpr uint32_t DDCKEY_SRCBLT = 0x08;
constexpr uint32_t DDBLTFAST_SRCCOLORKEY = 0x01;

static int g_failures = 0;

static void expect(bool cond, const char* msg) {
  if (!cond) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n", msg);
  }
}

static uint32_t hle_call(Cpu* c, rt::HleFn fn, std::initializer_list<uint32_t> args) {
  uint32_t saved = c->esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c->esp -= 4;
    st32(c->esp, *it);
  }
  c->esp -= 4;
  st32(c->esp, rt::kReturnSentinel);
  fn(c);
  c->esp = saved;
  return c->eax;
}

static uint32_t com_call(Cpu* c, uint32_t obj, unsigned slot, std::initializer_list<uint32_t> args) {
  uint32_t fn = ld32(ld32(obj) + 4u * slot);
  std::vector<uint32_t> stack{obj};
  stack.insert(stack.end(), args);
  uint32_t saved = c->esp;
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    c->esp -= 4;
    st32(c->esp, *it);
  }
  c->esp -= 4;
  st32(c->esp, rt::kReturnSentinel);
  rt_dispatch(c, fn);
  c->esp = saved;
  return c->eax;
}

static uint32_t alloc_desc(uint32_t w, uint32_t h, uint32_t caps, uint32_t back_buffers = 0) {
  uint32_t d = rt::heap_alloc(rt::process_heap(), 108, true);
  st32(d, 108);
  st32(d + 4, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | (back_buffers ? DDSD_BACKBUFFERCOUNT : 0));
  st32(d + 8, h);
  st32(d + 12, w);
  if (back_buffers) st32(d + 20, back_buffers);
  st32(d + 0x48, 32);
  st32(d + 0x4c, DDPF_PALETTEINDEXED8);
  st32(d + 0x58, 8);
  st32(d + 0x68, caps);
  return d;
}

}  // namespace

int main() {
  hle_link_ddraw();
  rt::memory_init();
  rt::dispatch_init();
  rt::bind_host_thread(rt::create_guest_thread(0x10000));
  Cpu* c = &rt::current_thread()->cpu;

  rt::HleFn create = rt::hle_find("ddraw.dll", "DirectDrawCreate");
  expect(create != nullptr, "DirectDrawCreate registered");

  uint32_t dd_out = rt::heap_alloc(rt::process_heap(), 4, true);
  hle_call(c, create, {0, dd_out, 0});
  uint32_t dd = ld32(dd_out);
  expect(dd != 0, "DirectDrawCreate returns object");

  expect(com_call(c, dd, kDdSetDisplayMode, {640, 480, 8}) == 0, "SetDisplayMode");

  uint32_t pal_entries = rt::heap_alloc(rt::process_heap(), 256 * 4, true);
  for (int i = 0; i < 256; i++) {
    st8(pal_entries + i * 4 + 0, (uint8_t)i);
    st8(pal_entries + i * 4 + 1, 0);
    st8(pal_entries + i * 4 + 2, 0);
    st8(pal_entries + i * 4 + 3, 0);
  }
  uint32_t pal_out = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(c, dd, kDdCreatePalette, {0, pal_entries, pal_out, 0}) == 0, "CreatePalette");
  uint32_t palette = ld32(pal_out);

  uint32_t pri_desc = alloc_desc(64, 48, DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX, 1);
  uint32_t pri_out = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(c, dd, kDdCreateSurface, {pri_desc, pri_out, 0}) == 0, "CreateSurface primary");
  uint32_t primary = ld32(pri_out);
  expect(com_call(c, primary, kSfcSetPalette, {palette}) == 0, "SetPalette");

  uint32_t lock_desc = rt::heap_alloc(rt::process_heap(), 108, true);
  st32(lock_desc, 108);
  expect(com_call(c, primary, kSfcLock, {0, lock_desc, 0, 0}) == 0, "Lock primary");
  uint32_t surf_mem = ld32(lock_desc + 36);
  int pitch = (int)ld32(lock_desc + 16);
  expect(surf_mem != 0 && pitch >= 64, "Lock gave surface memory");
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++) st8(surf_mem + y * pitch + x, (uint8_t)x);

  uint32_t off_desc = alloc_desc(16, 16, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
  uint32_t off_out = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(c, dd, kDdCreateSurface, {off_desc, off_out, 0}) == 0, "CreateSurface offscreen");
  uint32_t offscreen = ld32(off_out);
  expect(com_call(c, offscreen, kSfcSetPalette, {palette}) == 0, "offscreen SetPalette");

  uint32_t ck = rt::heap_alloc(rt::process_heap(), 8, true);
  st32(ck, 0);
  st32(ck + 4, 0);
  expect(com_call(c, offscreen, kSfcSetColorKey, {DDCKEY_SRCBLT, ck}) == 0, "SetColorKey");

  uint32_t off_lock = rt::heap_alloc(rt::process_heap(), 108, true);
  st32(off_lock, 108);
  com_call(c, offscreen, kSfcLock, {0, off_lock, 0, 0});
  uint32_t off_mem = ld32(off_lock + 36);
  int off_pitch = (int)ld32(off_lock + 16);
  memset(rt::host(off_mem), 0, (size_t)off_pitch * 16);
  st8(off_mem + 8 + 8 * off_pitch, 200);

  expect(com_call(c, primary, kSfcBltFast, {32, 16, offscreen, 0, DDBLTFAST_SRCCOLORKEY}) == 0, "BltFast");

  std::vector<uint32_t> rgba;
  int w = 0, h = 0;
  expect(ddraw::compose(rgba, w, h), "compose succeeded");
  expect(w == 64 && h == 48, "compose dimensions");

  auto px = [&](int x, int y) -> uint32_t { return rgba[(size_t)y * w + x]; };
  expect(px(10, 10) == 0xFF0A0000u, "gradient pixel (10,10)");
  expect(px(5, 5) == 0xFF050000u, "gradient pixel (5,5)");
  expect(px(40, 24) == 0xFFC80000u, "colorkey blt pixel (40,24)");

  // 8 bpp DIB blit (movie / SetDIBitsToDevice path): row pitch is dword-aligned.
  const int dib_w = 8, dib_h = 4;
  uint32_t bmi = rt::heap_alloc(rt::process_heap(), 40 + 256 * 4, true);
  memset(rt::host(bmi), 0, 40 + 256 * 4);
  st32(bmi + 0, 40);
  st32(bmi + 4, dib_w);
  st32(bmi + 8, dib_h);
  st16(bmi + 12, 1);
  st16(bmi + 14, 8);
  st32(bmi + 16, 0);  // BI_RGB
  for (int i = 0; i < 256; i++) {
    st8(bmi + 40 + i * 4 + 0, 0);
    st8(bmi + 40 + i * 4 + 1, (uint8_t)(i * 2));
    st8(bmi + 40 + i * 4 + 2, (uint8_t)(255 - i));
    st8(bmi + 40 + i * 4 + 3, 0);
  }
  const int pitch = ((dib_w + 3) / 4) * 4;
  uint32_t bits = rt::heap_alloc(rt::process_heap(), pitch * dib_h, true);
  memset(rt::host(bits), 0, pitch * dib_h);
  for (int y = 0; y < dib_h; y++)
    for (int x = 0; x < dib_w; x++) st8(bits + y * pitch + x, (uint8_t)(y * dib_w + x + 10));

  ddraw::gdi_blit_dib(0, 0, dib_w, dib_h, 0, 0, dib_w, dib_h, bits, bmi, true);
  expect(ddraw::compose(rgba, w, h), "compose after gdi blit");
  expect(w == 64 && h == 48, "compose still primary dimensions");
  expect(px(0, 0) == 0xFFF51400u, "gdi blit palette index 10 -> R=245,G=20,B=0");

  if (g_failures == 0) std::puts("ddraw_test PASS");
  else std::fprintf(stderr, "ddraw_test FAIL (%d)\n", g_failures);
  return g_failures ? 1 : 0;
}
