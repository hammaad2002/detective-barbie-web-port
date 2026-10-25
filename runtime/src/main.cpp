// Native entry point: map the game executable, bind the HLE layer and run the CRT entry point.
//   detective [install-dir] [registry-file]
#include <fstream>
#include <iterator>

#include "core/objects.h"
#include "core/rt.h"
#include "hle/registry.h"
#include "platform/platform.h"

int main(int argc, char** argv) {
  std::string install = argc > 1 ? argv[1] : "work/install";
  std::string reg = argc > 2 ? argv[2] : "work/registry.txt";
  rt::memory_init();
  rt::install_signal_handlers();
  rt::vfs_init(install);
  rt::registry_init(reg);
  if (!plat::init()) return 1;

  std::ifstream f(install + "/BDETECT.EXE", std::ios::binary);
  if (!f) rt::fatal("cannot open %s/BDETECT.EXE", install.c_str());
  std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  rt::load_image(exe);
  // CRT may call the game's malloc (0x45cc20) before the first HeapCreate stores 0x48a374.
  st32(0x48a374u, rt::process_heap());
  rt::dispatch_init();
  rt::watchdog_start();

  rt::GuestThread* t = rt::create_guest_thread(0x100000);
  rt::bind_host_thread(t);
  rt::log(rt::Log::Info, "starting guest at entry %08x", rt::image_entry());
  uint32_t code = rt::guest_call(&t->cpu, rt::image_entry(), {});
  rt::process_exit(code);
}
