// kernel32.dll file, mapping and volume APIs on top of the VFS.
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../core/objects.h"
#include "../core/rt.h"
#include "registry.h"

using namespace rt;

namespace {
constexpr size_t kWholeFileThreshold = 4 * 1024 * 1024;  // files <= 4 MiB loaded whole
constexpr size_t kWindowSize = 1024 * 1024;              // 1 MiB aligned window

// Read cache: avoids millions of tiny host stdio / WasmFS OPFS reads on read-only assets.
struct CachedFileObject : FileObject {
  std::mutex mutex;
  int64_t file_size_val = 0;
  int64_t pos = 0;
  bool whole = false;
  int64_t window_offset = -1;
  size_t window_size = 0;
  std::vector<uint8_t> buffer;
};

int64_t file_size(FILE* fp) {
  struct stat st;
  return fstat(fileno(fp), &st) == 0 ? (int64_t)st.st_size : -1;
}
}  // namespace


// Link anchor: forces this translation unit (and its HLE registrars) to be kept in a static link.
void hle_link_kernel32_files() {}

namespace {
const char* kDll = "kernel32.dll";

// ---- Overlay saves (writable) on top of read-only install / CD -----------------------------

std::mutex g_vfs_mutex;
std::string g_overlay_root;  // host path with trailing '/'
std::string g_guest_cwd;     // guest path with trailing '\\'
std::unordered_set<std::string> g_deleted;  // lower-case relative paths (forward slashes)
bool g_overlay_ready = false;

std::string vfs_lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string slash_to_backslash(std::string s) {
  for (auto& c : s)
    if (c == '/') c = '\\';
  return s;
}

std::string norm_slashes_host(std::string s) {
  for (auto& c : s)
    if (c == '\\') c = '/';
  return s;
}

bool mkdir_p(const std::string& path) {
  if (path.empty()) return false;
  std::string p = norm_slashes_host(path);
  std::string cur;
  for (size_t i = 0; i <= p.size(); i++) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
      if (i < p.size()) cur = p.substr(0, i);
    }
  }
  return true;
}

void load_delete_manifest() {
  std::string manifest = g_overlay_root + ".deleted-manifest";
  std::ifstream in(manifest);
  std::string line;
  while (std::getline(in, line)) {
    line = vfs_lower(slash_to_backslash(line));
    if (!line.empty()) g_deleted.insert(line);
  }
}

void save_delete_manifest() {
  std::string manifest = g_overlay_root + ".deleted-manifest";
  std::ofstream out(manifest);
  for (const auto& p : g_deleted) out << p << "\n";
}

void ensure_overlay_from_install() {
  if (g_overlay_ready) return;
  std::string install = vfs_host_root();
  if (install.empty()) return;
  if (g_overlay_root.empty()) {
    const char* env = std::getenv("DETECTIVE_SAVE_DIR");
    if (env && *env) {
      g_overlay_root = norm_slashes_host(env);
    } else {
      std::string p = install;
      if (!p.empty() && p.back() == '/') p.pop_back();
      size_t slash = p.find_last_of('/');
      g_overlay_root = slash == std::string::npos ? p + "/../saves/" : p.substr(0, slash + 1) + "../saves/";
      g_overlay_root = norm_slashes_host(g_overlay_root);
    }
    if (!g_overlay_root.empty() && g_overlay_root.back() != '/') g_overlay_root += '/';
    mkdir_p(g_overlay_root);
    load_delete_manifest();
  }
  if (g_guest_cwd.empty()) g_guest_cwd = kGameDir;
  g_overlay_ready = true;
}

std::string remap_cd_drive(std::string p) {
  if (p.size() >= 2 && p[1] == ':' && tolower((unsigned char)p[0]) == 'd') {
    std::string rest = p.substr(2);
    while (!rest.empty() && (rest[0] == '\\' || rest[0] == '/')) rest.erase(0, 1);
    p = std::string(kGameDir) + rest;
  }
  return p;
}

std::string vfs_guest_full_path(const std::string& in) {
  ensure_overlay_from_install();
  std::string p = slash_to_backslash(in);
  bool absolute = p.size() >= 2 && p[1] == ':';
  if (!absolute) {
    std::string base = g_guest_cwd.empty() ? std::string(kGameDir) : g_guest_cwd;
    if (!base.empty() && base.back() != '\\') base += '\\';
    p = (p.size() && p[0] == '\\' ? std::string("C:") : base) + p;
  }
  p = remap_cd_drive(p);
  for (auto& c : p)
    if (c == '/') c = '\\';
  std::vector<std::string> parts;
  std::string drive = p.substr(0, 2);
  size_t i = 2;
  while (i < p.size()) {
    size_t j = p.find('\\', i);
    if (j == std::string::npos) j = p.size();
    std::string comp = p.substr(i, j - i);
    if (comp == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!comp.empty() && comp != ".") {
      parts.push_back(comp);
    }
    i = j + 1;
  }
  std::string out = drive;
  for (auto& c : parts) out += "\\" + c;
  if (parts.empty()) out += "\\";
  return out;
}

std::string install_prefix_lower() {
  std::string root = std::string(kInstallDir);
  if (!root.empty() && root.back() == '\\') root.pop_back();
  return vfs_lower(root);
}

std::string guest_relative_key(const std::string& full_guest) {
  std::string root = std::string(kInstallDir);
  if (!root.empty() && root.back() == '\\') root.pop_back();
  if (vfs_lower(full_guest).rfind(vfs_lower(root), 0) != 0) return "";
  std::string rel = full_guest.substr(root.size());
  while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel.erase(0, 1);
  return vfs_lower(slash_to_backslash(rel));
}

std::string overlay_host_for_rel(const std::string& rel_key, bool create_parents) {
  if (g_overlay_root.empty()) return "";
  std::string rel = slash_to_backslash(rel_key);
  for (auto& c : rel)
    if (c == '\\') c = '/';
  std::string host = g_overlay_root + rel;
  if (create_parents) {
    size_t slash = host.find_last_of('/');
    if (slash != std::string::npos) mkdir_p(host.substr(0, slash));
  }
  return host;
}

bool sav_slot_name(const std::string& base) {
  if (base == "gamehist.ory") return true;
  if (base == "pinames.lst") return true;
  if (base == "current.ar1" || base == "current.ar2" || base == "current.ar3") return true;
  if (base.rfind("savgame") != 0 || base.size() < 11) return false;
  if (!isdigit((unsigned char)base[7]) || base[8] != '.') return false;
  std::string ext = base.substr(9);
  return ext == "ar1" || ext == "ar2" || ext == "ar3";
}

bool overlay_write_target(const std::string& full_guest) {
  std::string rel = guest_relative_key(full_guest);
  if (rel.empty()) return false;
  size_t slash = rel.find_last_of('\\');
  std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
  if (sav_slot_name(base)) return true;
  if (rel.rfind("data\\newgame.", 0) == 0) return true;
  return false;
}

bool host_exists_file(const std::string& host) {
  struct stat st;
  return stat(host.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string map_install_host(const std::string& guest, bool for_create) {
  return vfs_host_path(guest, for_create);
}

}  // namespace

namespace rt {

void vfs_overlay_init() {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  ensure_overlay_from_install();
}

void vfs_overlay_set_root(const std::string& host_path) {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  g_overlay_root = norm_slashes_host(host_path);
  if (!g_overlay_root.empty() && g_overlay_root.back() != '/') g_overlay_root += '/';
  g_overlay_ready = false;
  g_deleted.clear();
  ensure_overlay_from_install();
}

std::string vfs_overlay_root() {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  ensure_overlay_from_install();
  return g_overlay_root;
}

std::string vfs_map_full_path(const std::string& guest) {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  return vfs_guest_full_path(guest);
}

std::string vfs_map_host(const std::string& guest, bool for_create) {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  ensure_overlay_from_install();
  std::string full = vfs_guest_full_path(guest);
  std::string rel = guest_relative_key(full);
  const bool overlay_file = !rel.empty() && overlay_write_target(full);
  if (!rel.empty() && g_deleted.count(rel)) {
    if (for_create && overlay_file) {
      g_deleted.erase(rel);
      save_delete_manifest();
      return overlay_host_for_rel(rel, true);
    }
    return "";
  }
  if (!rel.empty()) {
    std::string oh = overlay_host_for_rel(rel, false);
    if (host_exists_file(oh)) return oh;
  }
  if (overlay_file && for_create) return overlay_host_for_rel(rel, true);
  if (overlay_file && !rel.empty()) {
    std::string oh = overlay_host_for_rel(rel, false);
    if (host_exists_file(oh)) return oh;
  }
  return map_install_host(full, for_create);
}

bool vfs_map_is_deleted(const std::string& guest) {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  ensure_overlay_from_install();
  std::string rel = guest_relative_key(vfs_guest_full_path(guest));
  return !rel.empty() && g_deleted.count(rel) != 0;
}

bool vfs_map_delete(const std::string& guest) {
  std::lock_guard<std::mutex> lock(g_vfs_mutex);
  ensure_overlay_from_install();
  std::string full = vfs_guest_full_path(guest);
  std::string rel = guest_relative_key(full);
  if (rel.empty()) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return false;
  }
  std::string oh = overlay_host_for_rel(rel, false);
  if (host_exists_file(oh)) {
    if (unlink(oh.c_str()) != 0) {
      set_last_error(ERROR_FILE_NOT_FOUND);
      return false;
    }
  }
  g_deleted.insert(rel);
  save_delete_manifest();
  return true;
}

}  // namespace rt

namespace {

constexpr uint32_t GENERIC_WRITE = 0x40000000;
constexpr uint32_t CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;

std::string guest_basename_lower(const std::string& guest) {
  std::string rel = guest_relative_key(vfs_guest_full_path(guest));
  size_t slash = rel.find_last_of('\\');
  return slash == std::string::npos ? vfs_lower(rel) : vfs_lower(rel.substr(slash + 1));
}

bool guest_basename_is_save_slot(const std::string& guest) { return sav_slot_name(guest_basename_lower(guest)); }

uint32_t open_file(const std::string& guest, uint32_t access, uint32_t disposition) {
  bool want_write = (access & GENERIC_WRITE) != 0;
  bool create = disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS;
  std::string hostp = vfs_map_host(guest, create || want_write);
  if (want_write && guest_basename_is_save_slot(guest)) {
    ensure_overlay_from_install();
    std::string rel = guest_relative_key(vfs_guest_full_path(guest));
    if (!rel.empty()) {
      std::string oh = overlay_host_for_rel(rel, true);
      if (!oh.empty()) hostp = oh;
    }
  }
  if (hostp.empty()) {
    log(Log::Debug, "CreateFile(%s): path not mapped", guest.c_str());
    set_last_error(ERROR_PATH_NOT_FOUND);
    return kInvalidHandle;
  }
  struct stat st;
  bool exists = stat(hostp.c_str(), &st) == 0;
  if (exists && S_ISDIR(st.st_mode)) {
    set_last_error(ERROR_ACCESS_DENIED);
    return kInvalidHandle;
  }
  const char* mode = nullptr;
  switch (disposition) {
    case CREATE_NEW:
      if (exists) {
        set_last_error(80);  // ERROR_FILE_EXISTS
        return kInvalidHandle;
      }
      mode = "w+b";
      break;
    case CREATE_ALWAYS:
      mode = "w+b";
      break;
    case OPEN_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
      }
      mode = want_write ? "r+b" : "rb";
      break;
    case OPEN_ALWAYS:
      mode = exists ? (want_write ? "r+b" : "rb") : "w+b";
      break;
    case TRUNCATE_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
      }
      mode = "w+b";
      break;
    default:
      set_last_error(ERROR_INVALID_PARAMETER);
      return kInvalidHandle;
  }
  FILE* fp = fopen(hostp.c_str(), mode);
  if (!fp) {
    set_last_error(want_write ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
    return kInvalidHandle;
  }
  std::shared_ptr<FileObject> f;
  if (!want_write && strcmp(mode, "rb") == 0) {
    int64_t sz = file_size(fp);
    if (sz >= 0) {
      auto cf = std::make_shared<CachedFileObject>();
      cf->fp = fp;
      cf->guest_path = guest;
      cf->host_path = hostp;
      cf->writable = false;
      cf->file_size_val = sz;
      cf->pos = 0;
      if ((size_t)sz <= kWholeFileThreshold) {
        cf->whole = true;
        cf->buffer.resize((size_t)sz);
        if (sz > 0) {
          size_t nread = fread(cf->buffer.data(), 1, (size_t)sz, fp);
          cf->buffer.resize(nread);
          cf->file_size_val = (int64_t)nread;
          fseeko(fp, 0, SEEK_SET);
        }
        cf->window_offset = 0;
        cf->window_size = cf->buffer.size();
      } else {
        cf->whole = false;
        cf->window_offset = -1;
        cf->window_size = 0;
      }
      f = cf;
    }
  }
  if (!f) {
    f = std::make_shared<FileObject>();
    f->fp = fp;
    f->guest_path = guest;
    f->host_path = hostp;
    f->writable = want_write;
  }
  // CREATE_ALWAYS / OPEN_ALWAYS report ERROR_ALREADY_EXISTS when the file existed.
  set_last_error((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && exists ? (uint32_t)ERROR_ALREADY_EXISTS : 0u);
  if (trace_enabled("file")) log(Log::Info, "CreateFile(%s, %s) -> %s", guest.c_str(), mode, hostp.c_str());
  return handle_new(f);
}
}  // namespace

HLE_STDCALL(kDll, CreateFileA, 7) {
  return open_file(read_cstr(arg(c, 0)), arg(c, 1), arg(c, 4));
}

HLE_STDCALL(kDll, CreateFileW, 7) {
  std::u16string w = read_wstr(arg(c, 0));
  std::string s(w.begin(), w.end());
  return open_file(s, arg(c, 1), arg(c, 4));
}

HLE_STDCALL(kDll, ReadFile, 5) {
  uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), nread = arg(c, 3), ov = arg(c, 4);
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    if (nread) st32(nread, 0);
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  if (cf) {
    std::lock_guard<std::mutex> lock(cf->mutex);
    int64_t read_pos = ov ? (int64_t)((uint64_t)ld32(ov + 12) << 32 | ld32(ov + 8)) : cf->pos;
    size_t got = 0;
    if (n && read_pos >= 0 && read_pos < cf->file_size_val) {
      size_t avail = (size_t)(cf->file_size_val - read_pos);
      size_t to_read = std::min((size_t)n, avail);
      if (cf->whole) {
        memcpy(host(buf), cf->buffer.data() + read_pos, to_read);
        got = to_read;
      } else if (to_read > kWindowSize) {
        fseeko(cf->fp, (off_t)read_pos, SEEK_SET);
        got = fread(host(buf), 1, to_read, cf->fp);
        cf->window_offset = -1;
        cf->window_size = 0;
      } else {
        while (got < to_read) {
          int64_t cur_pos = read_pos + got;
          if (cur_pos >= cf->window_offset && cur_pos < cf->window_offset + (int64_t)cf->window_size) {
            size_t chunk = std::min(to_read - got, (size_t)(cf->window_offset + (int64_t)cf->window_size - cur_pos));
            memcpy(host(buf) + got, cf->buffer.data() + (cur_pos - cf->window_offset), chunk);
            got += chunk;
          } else {
            int64_t win_start = (cur_pos / (int64_t)kWindowSize) * (int64_t)kWindowSize;
            int64_t bytes_to_win_end = cf->file_size_val - win_start;
            size_t win_len = std::min((int64_t)kWindowSize, bytes_to_win_end);
            if (cf->buffer.size() < kWindowSize) cf->buffer.resize(kWindowSize);
            fseeko(cf->fp, (off_t)win_start, SEEK_SET);
            size_t nrefill = fread(cf->buffer.data(), 1, win_len, cf->fp);
            cf->window_offset = win_start;
            cf->window_size = nrefill;
            if (nrefill == 0) break;
          }
        }
      }
    }
    cf->pos = read_pos + got;
    if (nread) st32(nread, (uint32_t)got);
    if (lfile_pos_get(h, nullptr)) lfile_pos_set(h, cf->pos);
    if (trace_enabled("file"))
      log(Log::Info, "ReadFile(%s) pos=%lld got=%zu nread=%u", cf->guest_path.c_str(), (long long)read_pos, got,
          nread ? ld32(nread) : 0u);
    if (ov) {
      st32(ov + 0, 0);
      st32(ov + 4, (uint32_t)got);
      if (got == 0 && n) {
        set_last_error(ERROR_HANDLE_EOF);
        return 0;
      }
    }
    return 1;
  }
  int64_t lpos = 0;
  bool use_lfile = lfile_pos_get(h, &lpos);
  if (ov) {
    fseeko(f->fp, (off_t)((uint64_t)ld32(ov + 12) << 32 | ld32(ov + 8)), SEEK_SET);
  } else if (use_lfile) {
    fseeko(f->fp, (off_t)lpos, SEEK_SET);
  }
  size_t got = n ? fread(host(buf), 1, n, f->fp) : 0;
  if (nread) st32(nread, (uint32_t)got);
  if (use_lfile) lfile_pos_set(h, lpos + (int64_t)got);
  if (ov) {
    // Completed synchronously: OVERLAPPED.Internal = STATUS_SUCCESS, InternalHigh = bytes.
    st32(ov + 0, 0);
    st32(ov + 4, (uint32_t)got);
    if (got == 0 && n) {
      set_last_error(ERROR_HANDLE_EOF);
      return 0;
    }
  }
  return 1;
}

HLE_STDCALL(kDll, WriteFile, 5) {
  uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), nwritten = arg(c, 3), ov = arg(c, 4);
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (ov) fseeko(f->fp, (off_t)((uint64_t)ld32(ov + 12) << 32 | ld32(ov + 8)), SEEK_SET);
  size_t put = n ? fwrite(host(buf), 1, n, f->fp) : 0;
  if (f->is_console) fflush(f->fp);
  int64_t after = (int64_t)ftello(f->fp);
  if (lfile_pos_get(h, nullptr)) lfile_pos_set(h, after);
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  if (cf && put) {
    std::lock_guard<std::mutex> lock(cf->mutex);
    cf->file_size_val = std::max(cf->file_size_val, after);
    cf->pos = after;
    cf->whole = false;
    cf->window_offset = -1;
  }
  if (nwritten) st32(nwritten, (uint32_t)put);
  if (ov) {
    st32(ov + 0, 0);
    st32(ov + 4, (uint32_t)put);
  }
  return put == n;
}

HLE_STDCALL(kDll, GetOverlappedResult, 4) {
  uint32_t ov = arg(c, 1), n = arg(c, 2);
  if (n) st32(n, ld32(ov + 4));
  return 1;
}

HLE_STDCALL(kDll, CancelIo, 1) { return 1; }

HLE_STDCALL(kDll, SetFilePointer, 4) {
  uint32_t h = arg(c, 0), hi_ptr = arg(c, 2), method = arg(c, 3);
  int64_t dist = (int32_t)arg(c, 1);
  if (hi_ptr) dist = (int64_t)((uint64_t)ld32(hi_ptr) << 32 | arg(c, 1));
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;
  }
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  if (cf) {
    std::lock_guard<std::mutex> lock(cf->mutex);
    int64_t base = method == 0 ? 0 : method == 1 ? cf->pos : cf->file_size_val;
    int64_t new_pos = base + dist;
    if (new_pos < 0) {
      set_last_error(131);  // ERROR_NEGATIVE_SEEK
      return 0xffffffffu;
    }
    cf->pos = new_pos;
    fseeko(cf->fp, (off_t)new_pos, SEEK_SET);
    if (lfile_pos_get(h, nullptr)) lfile_pos_set(h, new_pos);
    if (hi_ptr) st32(hi_ptr, (uint32_t)((uint64_t)new_pos >> 32));
    set_last_error(0);
    return (uint32_t)new_pos;
  }
  int whence = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
  if (fseeko(f->fp, (off_t)dist, whence) != 0) {
    set_last_error(131);  // ERROR_NEGATIVE_SEEK
    return 0xffffffffu;
  }
  uint64_t pos = (uint64_t)ftello(f->fp);
  if (lfile_pos_get(h, nullptr)) lfile_pos_set(h, (int64_t)pos);
  if (hi_ptr) st32(hi_ptr, (uint32_t)(pos >> 32));
  set_last_error(0);
  return (uint32_t)pos;
}

HLE_STDCALL(kDll, GetFileSize, 2) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0xffffffffu;
  }
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  int64_t size = cf ? cf->file_size_val : file_size(f->fp);
  if (arg(c, 1)) st32(arg(c, 1), (uint32_t)((uint64_t)size >> 32));
  return (uint32_t)size;
}

HLE_STDCALL(kDll, GetFileType, 1) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f) return 0;           // FILE_TYPE_UNKNOWN
  return f->is_console ? 2 : 1;  // FILE_TYPE_CHAR / FILE_TYPE_DISK
}

HLE_STDCALL(kDll, FlushFileBuffers, 1) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (f && f->fp) fflush(f->fp);
  return 1;
}

HLE_STDCALL(kDll, SetEndOfFile, 1) {
  uint32_t h = arg(c, 0);
  auto f = handle_as<FileObject>(h);
  if (!f || !f->fp) return 0;
  fflush(f->fp);
  int64_t end = (int64_t)ftello(f->fp);
  if (ftruncate(fileno(f->fp), (off_t)end) != 0) return 0;
  if (lfile_pos_get(h, nullptr)) lfile_pos_set(h, end);
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  if (cf) {
    std::lock_guard<std::mutex> lock(cf->mutex);
    cf->file_size_val = end;
    cf->pos = std::min(cf->pos, end);
    cf->whole = false;
    cf->buffer.clear();
    cf->window_offset = -1;
  }
  return 1;
}

HLE_STDCALL(kDll, GetFileAttributesA, 1) {
  std::string g = read_cstr(arg(c, 0));
  std::string hostp = vfs_map_host(g, false);
  struct stat st;
  if (hostp.empty() || stat(hostp.c_str(), &st) != 0) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return 0xffffffffu;  // INVALID_FILE_ATTRIBUTES
  }
  return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}

HLE_STDCALL(kDll, CreateDirectoryA, 2) {
  std::string g = read_cstr(arg(c, 0));
  std::string hostp = vfs_map_host(g, true);
  if (hostp.empty() || mkdir(hostp.c_str(), 0755) != 0) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return 0;
  }
  vfs_register_created(hostp);
  return 1;
}

HLE_STDCALL(kDll, SetCurrentDirectoryA, 1) {
  std::string path = read_cstr(arg(c, 0));
  std::string full = vfs_map_full_path(path);
  std::string prefix = install_prefix_lower();
  std::string low = vfs_lower(full);
  if (low.rfind(prefix, 0) != 0) {
    set_last_error(267);  // ERROR_DIRECTORY
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    g_guest_cwd = full;
    if (!g_guest_cwd.empty() && g_guest_cwd.back() != '\\') g_guest_cwd += '\\';
  }
  return 1;
}

HLE_STDCALL(kDll, GetCurrentDirectoryA, 2) {
  std::string dir = vfs_map_full_path(".");
  if (!dir.empty() && dir.back() == '\\') dir.pop_back();
  uint32_t n = arg(c, 0), buf = arg(c, 1);
  if (dir.size() + 1 > n) return (uint32_t)dir.size() + 1;
  write_cstr(buf, dir);
  return (uint32_t)dir.size();
}

HLE_STDCALL(kDll, GetFullPathNameA, 4) {
  std::string full = vfs_map_full_path(read_cstr(arg(c, 0)));
  uint32_t n = arg(c, 1), buf = arg(c, 2), part = arg(c, 3);
  if (n < full.size() + 1) return (uint32_t)full.size() + 1;
  write_cstr(buf, full);
  if (part) {
    size_t slash = full.find_last_of('\\');
    st32(part, slash == std::string::npos || slash + 1 == full.size() ? 0 : buf + (uint32_t)slash + 1);
  }
  return (uint32_t)full.size();
}

HLE_STDCALL(kDll, GetDiskFreeSpaceExA, 4) {
  for (int i = 1; i <= 3; i++)
    if (arg(c, i)) st64(arg(c, i), 8ull << 30);  // 8 GiB
  return 1;
}

// ---- Directory enumeration -----------------------------------------------------------------

struct FindObject : KObject {
  std::vector<std::string> names;  // matching entries (host names)
  std::vector<std::string> dirs;   // host dir for each name (install or overlay)
  size_t index = 0;
  const char* kind() const override { return "find"; }
};

// DOS wildcards: '*' and '?' only, case-insensitive.
bool wildcard_match(const std::string& pat, const std::string& s) {
  size_t p = 0, n = 0, star = std::string::npos, mark = 0;
  while (n < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || tolower((unsigned char)pat[p]) == tolower((unsigned char)s[n])))
      p++, n++;
    else if (p < pat.size() && pat[p] == '*')
      star = p++, mark = n;
    else if (star != std::string::npos)
      p = star + 1, n = ++mark;
    else
      return false;
  }
  while (p < pat.size() && pat[p] == '*') p++;
  return p == pat.size();
}

void write_find_data(uint32_t out, const std::string& host_dir, const std::string& name) {
  struct stat st;
  bool ok = stat((host_dir + "/" + name).c_str(), &st) == 0;
  memset(host(out), 0, 320);
  st32(out + 0, ok && S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
  uint64_t when = ok ? (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull : 0;
  st64(out + 4, when);   // ftCreationTime
  st64(out + 12, when);  // ftLastAccessTime
  st64(out + 20, when);  // ftLastWriteTime
  uint64_t size = ok ? (uint64_t)st.st_size : 0;
  st32(out + 28, (uint32_t)(size >> 32));
  st32(out + 32, (uint32_t)size);
  write_cstr(out + 44, name);  // cFileName
}

uint32_t find_first(Cpu* c, const std::string& pattern) {
  std::lock_guard<std::mutex> vfs_lock(g_vfs_mutex);
  ensure_overlay_from_install();
  std::string full = vfs_guest_full_path(pattern);
  size_t slash = full.find_last_of('\\');
  std::string dir_guest = slash == std::string::npos ? full : full.substr(0, slash);
  std::string pat = slash == std::string::npos ? "*.*" : full.substr(slash + 1);
  if (pat.empty()) pat = "*";
  std::string host_dir = map_install_host(dir_guest, false);
  if (host_dir.empty()) host_dir = norm_slashes_host(vfs_host_root());
  if (!host_dir.empty() && host_dir.back() == '/') host_dir.pop_back();
  if (host_dir.empty()) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return kInvalidHandle;
  }
  // Relative key of the directory itself ("" when enumerating install/game root).
  std::string rel_dir = guest_relative_key(dir_guest + "\\");
  std::string overlay_dir;
  if (!rel_dir.empty()) {
    overlay_dir = overlay_host_for_rel(rel_dir, false);
    size_t oslash = overlay_dir.find_last_of('/');
    if (oslash != std::string::npos) overlay_dir = overlay_dir.substr(0, oslash);
  } else if (!g_overlay_root.empty()) {
    // Saves (SavGame*.ar*, GameHist.ory) live at the overlay root, which maps to the
    // install root guest path — guest_relative_key("") must still merge that directory.
    overlay_dir = g_overlay_root;
    if (!overlay_dir.empty() && overlay_dir.back() == '/') overlay_dir.pop_back();
  }
  auto f = std::make_shared<FindObject>();
  // lower -> {display name, host dir}; overlay wins so sizes/stat hit the real save file.
  std::map<std::string, std::pair<std::string, std::string>> merged;
  auto add_dir = [&](const std::string& dir) {
    if (dir.empty()) return;
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string key = rel_dir + vfs_lower(name);
        if (g_deleted.count(key)) continue;
        merged[vfs_lower(name)] = {name, dir};
      }
      closedir(d);
    }
  };
  add_dir(host_dir);
  add_dir(overlay_dir);
  for (auto& [low, ent] : merged) {
    if (!wildcard_match(pat, ent.first)) continue;
    f->names.push_back(ent.first);
    f->dirs.push_back(ent.second);
  }
  if (f->names.empty()) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return kInvalidHandle;
  }
  if (trace_enabled("file"))
    log(Log::Info, "FindFirstFile(%s) -> %zu entries (overlay=%s)", pattern.c_str(), f->names.size(),
        overlay_dir.c_str());
  uint32_t h = handle_new(f);
  write_find_data(arg(c, 1), f->dirs[f->index], f->names[f->index]);
  f->index++;
  return h;
}

HLE_STDCALL(kDll, FindFirstFileA, 2) { return find_first(c, read_cstr(arg(c, 0))); }

HLE_STDCALL(kDll, FindNextFileA, 2) {
  auto f = handle_as<FindObject>(arg(c, 0));
  if (!f || f->index >= f->names.size()) {
    set_last_error(ERROR_NO_MORE_ITEMS);
    return 0;
  }
  write_find_data(arg(c, 1), f->dirs[f->index], f->names[f->index]);
  f->index++;
  return 1;
}

HLE_STDCALL(kDll, FindClose, 1) { return handle_close(arg(c, 0)) ? 1 : 0; }

// ---- File mappings -------------------------------------------------------------------------

HLE_STDCALL(kDll, CreateFileMappingA, 6) {
  auto f = handle_as<FileObject>(arg(c, 0));
  if (!f || !f->fp) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  auto m = std::make_shared<MappingObject>();
  m->file = f;
  uint32_t lo = arg(c, 4);
  auto cf = dynamic_cast<CachedFileObject*>(f.get());
  m->size = lo ? lo : (uint32_t)(cf ? cf->file_size_val : file_size(f->fp));
  return handle_new(m);
}

HLE_STDCALL(kDll, MapViewOfFile, 5) {
  auto m = handle_as<MappingObject>(arg(c, 0));
  if (!m) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
  }
  uint64_t off = (uint64_t)arg(c, 2) << 32 | arg(c, 3);
  uint32_t size = arg(c, 4) ? arg(c, 4) : m->size - (uint32_t)off;
  uint32_t base = vm_reserve(0, size);
  if (!base || !vm_commit(base, size)) {
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  // Read-only snapshot of the file (the game only maps files for reading).
  auto cf = dynamic_cast<CachedFileObject*>(m->file.get());
  size_t got = 0;
  if (cf && cf->whole) {
    std::lock_guard<std::mutex> lock(cf->mutex);
    if (off < (uint64_t)cf->file_size_val) {
      size_t avail = std::min((size_t)size, (size_t)(cf->file_size_val - off));
      memcpy(host(base), cf->buffer.data() + off, avail);
      got = avail;
    }
  } else {
    std::unique_lock<std::mutex> lock;
    if (cf) lock = std::unique_lock<std::mutex>(cf->mutex);
    long pos = ftell(m->file->fp);
    fseeko(m->file->fp, (off_t)off, SEEK_SET);
    got = fread(host(base), 1, size, m->file->fp);
    fseek(m->file->fp, pos, SEEK_SET);
  }
  log(Log::Debug, "MapViewOfFile(%s, %u bytes) -> %08x (%zu read)", m->file->guest_path.c_str(), size, base, got);
  return base;
}

HLE_STDCALL(kDll, UnmapViewOfFile, 1) { return vm_release(arg(c, 0)); }

// ---- Volumes: the player's own disc is mounted as D: ---------------------------------------

HLE_STDCALL(kDll, GetLogicalDrives, 0) { return (1u << 2) | (1u << 3); }

HLE_STDCALL(kDll, GetLogicalDriveStringsA, 2) {
  static const char drives[] = "C:\\\0D:\\\0";
  static const uint32_t len = 6;  // excludes the final terminating null
  uint32_t n = arg(c, 0), buf = arg(c, 1);
  if (n < len + 1) return len + 1;
  memcpy(host(buf), drives, sizeof drives);  // includes the double null terminator
  return len;
}

HLE_STDCALL(kDll, GetDriveTypeA, 1) {
  std::string root = read_cstr(arg(c, 0));
  char d = root.empty() ? 'C' : (char)toupper((unsigned char)root[0]);
  return d == 'C' ? 3 : d == 'D' ? 5 : 1;  // DRIVE_FIXED / DRIVE_CDROM / DRIVE_NO_ROOT_DIR
}

HLE_STDCALL(kDll, GetVolumeInformationA, 8) {
  std::string root = read_cstr(arg(c, 0));
  char d = root.empty() ? 'C' : (char)toupper((unsigned char)root[0]);
  const char* label = d == 'D' ? "DETECTIVE" : d == 'C' ? "SYSTEM" : nullptr;
  if (!label) {
    set_last_error(21);  // ERROR_NOT_READY
    return 0;
  }
  if (arg(c, 1) && arg(c, 2)) write_cstr(arg(c, 1), label);
  if (arg(c, 3)) st32(arg(c, 3), d == 'D' ? 0x20041006 : 0x1234abcd);
  if (arg(c, 4)) st32(arg(c, 4), 255);
  if (arg(c, 5)) st32(arg(c, 5), 0);
  if (arg(c, 6) && arg(c, 7)) write_cstr(arg(c, 6), d == 'D' ? "CDFS" : "NTFS");
  return 1;
}

static uint32_t delete_file_hle(Cpu* c) {
  std::string guest = read_cstr(arg(c, 0));
  return vfs_map_delete(guest) ? 1u : 0u;
}

static void delete_file_entry(Cpu* c) { ret_stdcall(c, delete_file_hle(c), 1); }

__attribute__((constructor(65535))) static void vfs_register_delete_file() {
  hle_register("kernel32.dll", "DeleteFileA", delete_file_entry);
}
