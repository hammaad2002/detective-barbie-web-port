// DirectSound 5 (DirectSoundCreate) HLE self-test: 8-bit mono secondary buffer, offline mixer render.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/mixer.h"
#include "core/objects.h"
#include "core/rt.h"
#include "hle/com.h"

void hle_link_dsound();

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

static int g_failures = 0;

static void expect(bool cond, const char* msg) {
  if (!cond) {
    ++g_failures;
    fprintf(stderr, "FAIL %s\n", msg);
  }
}

namespace {

constexpr unsigned kDsCreateSoundBuffer = 3;
constexpr unsigned kBufLock = 11;
constexpr unsigned kBufPlay = 12;
constexpr unsigned kBufGetCurrentPosition = 4;

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

static void write_wfx(uint32_t wfx, uint16_t ch, uint32_t rate, uint16_t bits) {
  uint16_t align = (uint16_t)(ch * bits / 8);
  st16(wfx, 1);
  st16(wfx + 2, ch);
  st32(wfx + 4, rate);
  st32(wfx + 8, rate * align);
  st16(wfx + 12, align);
  st16(wfx + 14, bits);
}

}  // namespace

int main() {
  hle_link_dsound();
  rt::memory_init();
  rt::dispatch_init();

  rt::GuestThread* gt = rt::create_guest_thread(0);
  Cpu* cpu = &gt->cpu;

  rt::HleFn direct_sound_create = rt::hle_find("dsound.dll", "DirectSoundCreate");
  expect(direct_sound_create != nullptr, "DirectSoundCreate registered");
  rt::HleFn by_ordinal = rt::hle_find("dsound.dll", "#1");
  expect(by_ordinal != nullptr, "dsound ordinal #1 alias");

  uint32_t ds_out = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(hle_call(cpu, direct_sound_create, {0, ds_out, 0}) == 0, "DirectSoundCreate hr");
  uint32_t ds = ld32(ds_out);
  expect(ds != 0, "IDirectSound object");

  constexpr uint32_t kBytes = 4096;
  uint32_t wfx = rt::heap_alloc(rt::process_heap(), 18, true);
  write_wfx(wfx, 1, 22050, 8);
  uint32_t desc = rt::heap_alloc(rt::process_heap(), 20, true);
  st32(desc + 0, 20);
  st32(desc + 4, 0);  // secondary
  st32(desc + 8, kBytes);
  st32(desc + 12, 0);
  st32(desc + 16, wfx);

  uint32_t buf_out = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(cpu, ds, kDsCreateSoundBuffer, {desc, buf_out, 0}) == 0, "CreateSoundBuffer");
  uint32_t buf = ld32(buf_out);
  expect(buf != 0, "IDirectSoundBuffer");

  uint32_t p1 = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t n1 = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(cpu, buf, kBufLock, {0, kBytes, p1, n1, 0, 0, 0}) == 0, "Lock");
  uint32_t mem = ld32(p1);
  expect(ld32(n1) == kBytes, "Lock nbytes");
  for (uint32_t i = 0; i < kBytes; i++) st8(mem + i, (uint8_t)(128 + (i % 128)));

  expect(com_call(cpu, buf, kBufPlay, {0, 0, 1}) == 0, "Play looping");

  uint32_t play_pos = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t write_pos = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(com_call(cpu, buf, kBufGetCurrentPosition, {play_pos, write_pos}) == 0, "GetCurrentPosition initial");
  uint32_t play0 = ld32(play_pos);
  expect(play0 == 0, "play cursor starts at 0");

  constexpr int kRenderFrames = 4410;
  std::vector<float> mix((size_t)kRenderFrames * 2);
  audio::render_offline(mix.data(), kRenderFrames);

  expect(com_call(cpu, buf, kBufGetCurrentPosition, {play_pos, write_pos}) == 0, "GetCurrentPosition after mix");
  uint32_t play1 = ld32(play_pos);
  expect(play1 > play0, "play cursor advanced");
  // 22050 Hz source mixed at 44100 Hz: ~0.5 source frames per output frame.
  uint32_t expect_bytes = (uint32_t)((uint64_t)kRenderFrames * 22050 / audio::kRate);
  expect(play1 >= expect_bytes - 4 && play1 <= expect_bytes + 4, "play cursor matches mixer clock");

  float peak = 0.0f;
  for (float s : mix) peak = std::max(peak, std::fabs(s));
  expect(peak > 0.01f, "mixed output audible");

  // First output frame samples byte 0 (128 -> 0.0); shortly after, ramp produces positive samples.
  bool saw_positive = false;
  for (int i = 0; i < kRenderFrames && !saw_positive; i++)
    if (mix[(size_t)i * 2] > 0.02f) saw_positive = true;
  expect(saw_positive, "ramp produces positive samples");

  if (g_failures) {
    fprintf(stderr, "DSOUND TEST FAIL (%d)\n", g_failures);
    return 1;
  }
  puts("DSOUND TEST PASS");
  return 0;
}
