// VFS overlay + CD volume semantics (native self-test).
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/objects.h"
#include "core/rt.h"
#include "hle/registry.h"

void hle_link_kernel32();
void hle_link_kernel32_files();
void hle_link_advapi32();

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

static uint32_t guest_cstr(const std::string& s) {
  uint32_t p = rt::heap_alloc(rt::process_heap(), (uint32_t)s.size() + 1, true);
  rt::write_cstr(p, s);
  return p;
}

int main() {
  namespace fs = std::filesystem;
  hle_link_kernel32();
  hle_link_kernel32_files();
  hle_link_advapi32();
  rt::memory_init();
  rt::dispatch_init();

  fs::path tmp = fs::temp_directory_path() / "detective-files-test-e5f9fb4e";
  fs::remove_all(tmp);
  fs::create_directories(tmp / "data");
  std::ofstream(tmp / "data" / "probe.dat") << "CDROM";
  fs::path overlay = tmp / "overlay";
  fs::create_directories(overlay);

  rt::vfs_init(tmp.string());
  rt::vfs_overlay_set_root(overlay.string());
  rt::registry_init((tmp / "registry.txt").string());

  rt::GuestThread* gt = rt::create_guest_thread(0x10000);
  rt::bind_host_thread(gt);
  Cpu* cpu = &gt->cpu;

  rt::HleFn create_file = rt::hle_find("kernel32.dll", "CreateFileA");
  rt::HleFn write_file = rt::hle_find("kernel32.dll", "WriteFile");
  rt::HleFn read_file = rt::hle_find("kernel32.dll", "ReadFile");
  rt::HleFn close_handle = rt::hle_find("kernel32.dll", "CloseHandle");
  rt::HleFn delete_file = rt::hle_find("kernel32.dll", "DeleteFileA");
  rt::HleFn get_drive_type = rt::hle_find("kernel32.dll", "GetDriveTypeA");
  rt::HleFn get_vol_info = rt::hle_find("kernel32.dll", "GetVolumeInformationA");
  rt::HleFn get_logical = rt::hle_find("kernel32.dll", "GetLogicalDrives");
  rt::HleFn reg_open = rt::hle_find("advapi32.dll", "RegOpenKeyExA");
  rt::HleFn reg_query = rt::hle_find("advapi32.dll", "RegQueryValueExA");
  rt::HleFn reg_close = rt::hle_find("advapi32.dll", "RegCloseKey");

  expect(create_file && write_file && read_file && delete_file, "kernel32 imports registered");

  expect(hle_call(cpu, get_drive_type, {guest_cstr("D:\\")}) == 5, "D: is CD-ROM");
  expect(hle_call(cpu, get_drive_type, {guest_cstr("C:\\")}) == 3, "C: is fixed");
  expect((hle_call(cpu, get_logical, {}) & (1u << 3)) != 0, "logical drives include D:");

  uint32_t vol_buf = rt::heap_alloc(rt::process_heap(), 32, true);
  uint32_t fs_buf = rt::heap_alloc(rt::process_heap(), 16, true);
  expect(hle_call(cpu, get_vol_info, {guest_cstr("D:\\"), vol_buf, 32, 0, 0, 0, fs_buf, 16}) == 1,
         "GetVolumeInformation D:");
  expect(std::string(rt::read_cstr(vol_buf)) == "DETECTIVE", "volume label DETECTIVE");
  expect(std::string(rt::read_cstr(fs_buf)) == "CDFS", "D: filesystem CDFS");

  std::string host_read = rt::vfs_map_host("data\\PROBE.DAT", false);
  expect(!host_read.empty(), "case-insensitive asset lookup");
  {
    std::ifstream in(host_read);
    std::string body;
    in >> body;
    expect(body == "CDROM", "install asset readable");
  }

  uint32_t save_path = guest_cstr("SavGame1.ar1");
  uint32_t h = hle_call(cpu, create_file, {save_path, 0x40000000u, 0, 0, 2u, 0x80u, 0});
  expect(h != rt::kInvalidHandle, "CreateFile save slot");
  uint32_t payload = rt::heap_alloc(rt::process_heap(), 8, true);
  memcpy(rt::host(payload), "SAVEGAME", 8);
  uint32_t written = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(hle_call(cpu, write_file, {h, payload, 8, written, 0}) == 1, "WriteFile save");
  hle_call(cpu, close_handle, {h});

  bool overlay_has_save = false;
  for (const auto& e : fs::directory_iterator(overlay))
    if (e.path().filename() == "SavGame1.ar1" || e.path().filename() == "savgame1.ar1") overlay_has_save = true;
  expect(overlay_has_save, "save lands in overlay not install tree");
  expect(!fs::exists(tmp / "SavGame1.ar1"), "install tree untouched");

  uint32_t h2 = hle_call(cpu, create_file, {save_path, 0x80000000u, 0, 0, 3u, 0x80u, 0});
  expect(h2 != rt::kInvalidHandle, "reopen save for read");
  uint32_t rbuf = rt::heap_alloc(rt::process_heap(), 8, true);
  uint32_t nread = rt::heap_alloc(rt::process_heap(), 4, true);
  expect(hle_call(cpu, read_file, {h2, rbuf, 8, nread, 0}) == 1, "ReadFile overlay save");
  expect(memcmp(rt::host(rbuf), "SAVEGAME", 8) == 0, "overlay readback");
  hle_call(cpu, close_handle, {h2});

  expect(hle_call(cpu, delete_file, {save_path}) == 1, "DeleteFile save");
  expect(rt::vfs_map_is_deleted("SavGame1.ar1"), "delete recorded");
  overlay_has_save = false;
  for (const auto& e : fs::directory_iterator(overlay))
    if (e.path().filename() == "SavGame1.ar1" || e.path().filename() == "savgame1.ar1") overlay_has_save = true;
  expect(!overlay_has_save, "overlay file removed");

  // SavGameN.ar{1,2,3} + current.ar* (m12 in-game save set) route to overlay like SavGame1.
  for (const char* name : {"SavGame2.ar2", "SavGame2.ar3", "current.ar1"}) {
    uint32_t p = guest_cstr(name);
    uint32_t hs = hle_call(cpu, create_file, {p, 0x40000000u, 0, 0, 2u, 0x80u, 0});
    expect(hs != rt::kInvalidHandle, name);
    expect(hle_call(cpu, write_file, {hs, payload, 4, written, 0}) == 1, "WriteFile multi-part save");
    hle_call(cpu, close_handle, {hs});
    bool found = false;
    std::string want = name;
    for (auto& c : want) c = (char)tolower((unsigned char)c);
    for (const auto& e : fs::directory_iterator(overlay)) {
      std::string fn = e.path().filename().string();
      for (auto& c : fn) c = (char)tolower((unsigned char)c);
      if (fn == want) found = true;
    }
    expect(found, "overlay has save part");
    expect(hle_call(cpu, delete_file, {p}) == 1, "DeleteFile save part");
  }

  uint32_t hk = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t sub =
      guest_cstr("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bdetect.exe");
  expect(hle_call(cpu, reg_open, {0x80000002u, sub, 0, 0x000f003fu, hk}) == 0, "RegOpenKeyEx App Paths");
  uint32_t key = ld32(hk);
  uint32_t path_buf = rt::heap_alloc(rt::process_heap(), 260, true);
  uint32_t type = rt::heap_alloc(rt::process_heap(), 4, true);
  uint32_t size = rt::heap_alloc(rt::process_heap(), 4, true);
  st32(size, 260);
  uint32_t name_path = guest_cstr("Path");
  expect(hle_call(cpu, reg_query, {key, name_path, 0, type, path_buf, size}) == 0, "RegQuery Path");
  std::string reg_path = rt::read_cstr(path_buf);
  expect(reg_path == std::string(rt::kGameDir), "registry Path is install dir");
  hle_call(cpu, reg_close, {key});

  if (g_failures) {
    fprintf(stderr, "FILES TEST FAIL (%d)\n", g_failures);
    return 1;
  }
  puts("FILES TEST PASS");
  return 0;
}
