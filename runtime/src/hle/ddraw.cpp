// ddraw.dll: software DirectDraw surfaces for 2D presentation.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

#include "../core/objects.h"
#include "../platform/platform.h"
#include "com.h"
#include "ddraw.h"
#include "font8x16.h"

using namespace rt;

namespace ddraw {
void publish_frame();  // snapshot the finished frame for the presenter (defined with compose)
}

namespace {

using HWND = uint32_t;
struct RECT {
  int32_t left, top, right, bottom;
};

ComInterface& surface_iface();
ComInterface& surface4_iface();
ComInterface& surface7_iface();
ComInterface& dd1_iface();
ComInterface& dd2_iface();
ComInterface& dd4_iface();
ComInterface& dd7_iface();

constexpr uint32_t DD_OK = S_OK;
constexpr uint32_t DDERR_INVALIDPARAMS = E_INVALIDARG;
constexpr uint32_t DDERR_NOCLIPPER = 0x8876001e;
constexpr uint32_t DDERR_NOPALETTEATTACHED = 0x88760094;
constexpr uint32_t DDERR_NOCOLORKEY = 0x887600c2;
constexpr uint32_t DDERR_SURFACELOST = 0x887600cc;

constexpr uint32_t DDSCAPS_BACKBUFFER = 0x00000004;
constexpr uint32_t DDSCAPS_COMPLEX = 0x00000008;
constexpr uint32_t DDSCAPS_FLIP = 0x00000010;
constexpr uint32_t DDSCAPS_FRONTBUFFER = 0x00000020;
constexpr uint32_t DDSCAPS_OFFSCREENPLAIN = 0x00000040;
constexpr uint32_t DDSCAPS_PALETTE = 0x00000100;
constexpr uint32_t DDSCAPS_PRIMARYSURFACE = 0x00000200;
constexpr uint32_t DDSCAPS_SYSTEMMEMORY = 0x00000800;

constexpr uint32_t DDPF_RGB = 0x00000040;
constexpr uint32_t DDPF_PALETTEINDEXED8 = 0x00000020;

constexpr uint32_t DDBLT_COLORFILL = 0x00000400;
constexpr uint32_t DDBLT_KEYSRC = 0x00008000;
constexpr uint32_t DDBLTFAST_SRCCOLORKEY = 0x00000001;

constexpr uint32_t DDCKEY_SRCBLT = 0x00000008;
constexpr uint32_t DDCKEY_DESTBLT = 0x00000002;

constexpr uint32_t DDSD_CAPS = 0x00000001;
constexpr uint32_t DDSD_HEIGHT = 0x00000002;
constexpr uint32_t DDSD_WIDTH = 0x00000004;
constexpr uint32_t DDSD_PITCH = 0x00000008;
constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000;
constexpr uint32_t DDSD_LPSURFACE = 0x00000800;
constexpr uint32_t DDSD_BACKBUFFERCOUNT = 0x00000020;

constexpr uint32_t DDSDM_DEFAULT = 0x00000001;

constexpr uint32_t kDdrawHdcBase = 0x00010040;

const Guid IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_IDirectDraw = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectDraw2 = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
const Guid IID_IDirectDraw4 = {0x9c59509a, 0x39bd, 0x11d1, {0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5}};
const Guid IID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
const Guid IID_IDirectDrawSurface = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectDrawSurface4 = {0x0B2B8630, 0xAD35, 0x11D0, {0x8E, 0xA6, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B}};
const Guid IID_IDirectDrawSurface7 = {0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
const Guid IID_IDirectDrawPalette = {0x6C14DB84, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectDrawClipper = {0x6C14DB85, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

struct PixelFormat {
  uint32_t bpp = 8;
  uint32_t flags = DDPF_PALETTEINDEXED8;
  uint32_t r_mask = 0, g_mask = 0, b_mask = 0;
};

struct DisplayMode {
  uint32_t w = 640, h = 480, bpp = 8;
  PixelFormat pf;
};

std::recursive_mutex g_dd_mutex;
std::atomic<uint64_t> g_frame_counter{0};
thread_local bool t_drew_primary = false;  // this thread drew to the visible surface (the game's frame thread)
DisplayMode g_mode, g_saved_mode;
bool g_mode_set = false;
HWND g_hwnd = 0;
uint32_t g_coop_flags = 0;

std::vector<uint32_t> g_gdi_rgba;
int g_gdi_w = 0, g_gdi_h = 0;

class DrawSurface;
void write_pixel(DrawSurface* s, int x, int y, uint32_t c, const uint8_t* pal256x4);

struct GdiFont {
  int height = 16;
  int weight = 400;
};

struct GdiDcState {
  int font_handle = 0;
  int bk_mode = 1;  // TRANSPARENT
  uint32_t text_color = 0x00000000;  // COLORREF black
  uint32_t bk_color = 0x00FFFFFF;
  DrawSurface* surface = nullptr;
};

std::unordered_map<uint32_t, GdiFont> g_gdi_fonts;
std::unordered_map<uint32_t, GdiDcState> g_gdi_dcs;
std::atomic<uint32_t> g_next_font_handle{0x00020010u};

GdiDcState& gdi_dc(uint32_t hdc) {
  auto& st = g_gdi_dcs[hdc];
  if (hdc == ddraw::kGdiWindowHdc && !st.surface) st.surface = nullptr;
  return st;
}

const GdiFont& gdi_font_for_dc(const GdiDcState& dc) {
  auto it = g_gdi_fonts.find((uint32_t)dc.font_handle);
  if (it != g_gdi_fonts.end()) return it->second;
  static const GdiFont kDefault{16, 400};
  return kDefault;
}


int gdi_glyph_advance_px(int ch, int font_h) {
  (void)ch;
  return 8 * std::max(1, (font_h + 8) / 16);
}

void gdi_ensure_overlay(int min_w, int min_h) {
  if (min_w < 640) min_w = 640;
  if (min_h < 480) min_h = 480;
  if (g_gdi_w >= min_w && g_gdi_h >= min_h) return;
  int nw = std::max(g_gdi_w, min_w);
  int nh = std::max(g_gdi_h, min_h);
  std::vector<uint32_t> newbuf((size_t)nw * nh, 0u);
  for (int y = 0; y < g_gdi_h; y++)
    memcpy(newbuf.data() + (size_t)y * nw, g_gdi_rgba.data() + (size_t)y * g_gdi_w, (size_t)g_gdi_w * 4);
  g_gdi_rgba = std::move(newbuf);
  g_gdi_w = nw;
  g_gdi_h = nh;
}

uint32_t colorref_to_bgra(uint32_t cref) {
  uint8_t r = (uint8_t)(cref & 0xff);
  uint8_t g = (uint8_t)((cref >> 8) & 0xff);
  uint8_t b = (uint8_t)((cref >> 16) & 0xff);
  return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

int nearest_palette_index(const uint8_t* pal256x4, uint8_t r, uint8_t g, uint8_t b) {
  int best = 0, best_d = 0x7fffffff;
  for (int i = 0; i < 256; i++) {
    int dr = (int)pal256x4[i * 4 + 2] - r;
    int dg = (int)pal256x4[i * 4 + 1] - g;
    int db = (int)pal256x4[i * 4 + 0] - b;
    int d = dr * dr + dg * dg + db * db;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

void gdi_put_overlay_px(int x, int y, uint32_t bgra) {
  if (x < 0 || y < 0) return;
  gdi_ensure_overlay(x + 1, y + 1);
  if (x < g_gdi_w && y < g_gdi_h) g_gdi_rgba[(size_t)y * g_gdi_w + x] = bgra;
}

void gdi_draw_glyph_overlay(int x, int y, int ch, int font_h, uint32_t fg, uint32_t bg, int bk_mode) {
  if (ch < 32 || ch > 127) ch = '?';
  const uint8_t* glyph = kFont8x16[ch - 32];
  const int scale = std::max(1, (font_h + 8) / 16);
  for (int row = 0; row < 16; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      bool on = (bits >> (7 - col)) & 1;
      for (int sy = 0; sy < scale; sy++)
        for (int sx = 0; sx < scale; sx++) {
          int px = x + col * scale + sx;
          int py = y + row * scale + sy;
          if (on)
            gdi_put_overlay_px(px, py, fg);
          else if (bk_mode == 2)
            gdi_put_overlay_px(px, py, bg);
        }
    }
  }
}

class DrawPalette;
class DrawSurface;
class DirectDraw;
class DrawClipper;

DrawSurface* g_visible_primary = nullptr;

uint32_t ok(Cpu*, ComObject*) { return DD_OK; }

PixelFormat mode_pixel_format(const DisplayMode& m) {
  PixelFormat pf;
  pf.bpp = m.bpp;
  if (m.bpp == 8) {
    pf.flags = DDPF_PALETTEINDEXED8;
  } else {
    pf.flags = DDPF_RGB;
    if (m.bpp == 16) {
      pf.r_mask = 0xF800;
      pf.g_mask = 0x07E0;
      pf.b_mask = 0x001F;
    } else if (m.bpp == 24) {
      pf.r_mask = 0x00FF0000;
      pf.g_mask = 0x0000FF00;
      pf.b_mask = 0x000000FF;
    } else {
      pf.r_mask = 0x00FF0000;
      pf.g_mask = 0x0000FF00;
      pf.b_mask = 0x000000FF;
    }
  }
  return pf;
}

PixelFormat read_pixel_format(uint32_t addr) {
  PixelFormat pf = mode_pixel_format(g_mode);
  if (!addr || ld32(addr) < 32) return pf;
  uint32_t flags = ld32(addr + 4);
  if (flags & DDPF_PALETTEINDEXED8) {
    pf.bpp = 8;
    pf.flags = DDPF_PALETTEINDEXED8;
    return pf;
  }
  if (flags & DDPF_RGB) {
    pf.flags = DDPF_RGB;
    pf.bpp = ld32(addr + 12);
    pf.r_mask = ld32(addr + 16);
    pf.g_mask = ld32(addr + 20);
    pf.b_mask = ld32(addr + 24);
    if (!pf.bpp) pf.bpp = 16;
    return pf;
  }
  return pf;
}

void write_pixel_format(uint32_t addr, const PixelFormat& pf) {
  if (!addr) return;
  st32(addr, 32);
  st32(addr + 4, pf.flags);
  st32(addr + 8, 0);
  st32(addr + 12, pf.bpp);
  st32(addr + 16, pf.r_mask);
  st32(addr + 20, pf.g_mask);
  st32(addr + 24, pf.b_mask);
  st32(addr + 28, 0);
}

uint32_t pitch_bytes(uint32_t w, uint32_t bpp) { return ((w * bpp + 31) / 32) * 4; }

uint32_t bgra_from_rgb(uint8_t r, uint8_t g, uint8_t b) { return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }

uint32_t read_pixel(const DrawSurface* s, int x, int y, const uint8_t* pal256x4);
void write_pixel(DrawSurface* s, int x, int y, uint32_t c, const uint8_t* pal256x4);

class DrawPalette : public ComObject {
 public:
  const char* type_name() const override { return "DirectDrawPalette"; }
  uint8_t entries[256 * 4]{};  // PE RGBX
  uint32_t caps = 0;
};

class DrawClipper : public ComObject {
 public:
  const char* type_name() const override { return "DirectDrawClipper"; }
  HWND hwnd = 0;
};

class DrawSurface : public ComObject {
 public:
  const char* type_name() const override { return "DirectDrawSurface"; }
  DirectDraw* dd = nullptr;
  uint32_t w = 0, h = 0, pitch = 0, mem = 0;
  PixelFormat pf;
  uint32_t caps = 0;
  DrawPalette* palette = nullptr;
  DrawClipper* clipper = nullptr;
  bool has_ck_src = false, has_ck_dest = false;
  uint32_t ck_src = 0, ck_dest = 0;
  DrawSurface* attached = nullptr;
  bool is_front = false;
  uint32_t lock_count = 0;
  uint32_t hdc = 0;

  ~DrawSurface() override {
    if (mem) heap_free(process_heap(), mem);
  }

  bool is_primary_visible() const { return g_visible_primary == this; }

  void bump_if_primary() {
    if (caps & DDSCAPS_PRIMARYSURFACE) g_frame_counter++, g_stats[StatPrimaryBlts]++, t_drew_primary = true;
  }
};

class DirectDraw : public ComObject {
 public:
  const char* type_name() const override { return "DirectDraw8"; }
  DrawSurface* primary = nullptr;
};

DrawPalette* pal(ComObject* o) { return static_cast<DrawPalette*>(o); }
DrawSurface* surf(ComObject* o) { return static_cast<DrawSurface*>(o); }
DirectDraw* dd(ComObject* o) { return static_cast<DirectDraw*>(o); }

void fill_surface_desc(uint32_t desc, DrawSurface* s, bool lock_fields) {
  if (!desc || !s) return;
  uint32_t sz = ld32(desc);
  if (sz < 108) return;
  st32(desc + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | (lock_fields ? (DDSD_PITCH | DDSD_LPSURFACE) : 0));
  st32(desc + 8, s->h);
  st32(desc + 12, s->w);
  st32(desc + 16, (int32_t)s->pitch);
  st32(desc + 0x68, s->caps);
  write_pixel_format(desc + 0x48, s->pf);
  if (lock_fields) {
    st32(desc + 36, s->mem);  // DDSURFACEDESC.lpSurface (after dwAlphaBitDepth + dwReserved)
  }
}

void fill_display_mode_desc(uint32_t desc, const DisplayMode& m) {
  if (!desc) return;
  uint32_t sz = ld32(desc);
  if (sz < 108) return;
  st32(desc + 4, DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT);
  st32(desc + 8, m.h);
  st32(desc + 12, m.w);
  st32(desc + 16, (int32_t)pitch_bytes(m.w, m.bpp));
  write_pixel_format(desc + 0x48, m.pf.bpp ? m.pf : mode_pixel_format(m));
}

uint32_t read_pixel(const DrawSurface* s, int x, int y, const uint8_t* pal256x4) {
  if (!s || x < 0 || y < 0 || x >= (int)s->w || y >= (int)s->h) return 0;
  uint32_t off = s->mem + y * s->pitch;
  if (s->pf.bpp == 8) {
    uint8_t idx = ld8(off + x);
    if (pal256x4) {
      const uint8_t* e = pal256x4 + idx * 4;
      return bgra_from_rgb(e[0], e[1], e[2]);
    }
    return bgra_from_rgb(idx, 0, 0);
  }
  if (s->pf.bpp == 16) {
    uint16_t v = (uint16_t)ld16(off + x * 2);
    uint32_t r = ((v & s->pf.r_mask) >> 11) & 0xFF;
    uint32_t g = ((v & s->pf.g_mask) >> 5) & 0xFF;
    uint32_t b = (v & s->pf.b_mask) << 3;
    if (s->pf.r_mask == 0x7C00) {  // 555
      r = ((v & 0x7C00) >> 7) & 0xFF;
      g = ((v & 0x03E0) >> 2) & 0xFF;
      b = ((v & 0x001F) << 3);
    }
    return bgra_from_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
  }
  if (s->pf.bpp == 24) {
    uint32_t p = off + x * 3;
    return bgra_from_rgb(ld8(p + 2), ld8(p + 1), ld8(p));
  }
  uint32_t p = off + x * 4;
  return bgra_from_rgb(ld8(p + 2), ld8(p + 1), ld8(p));
}

void write_pixel(DrawSurface* s, int x, int y, uint32_t c, const uint8_t* pal256x4) {
  if (!s || x < 0 || y < 0 || x >= (int)s->w || y >= (int)s->h) return;
  uint32_t off = s->mem + y * s->pitch;
  if (s->pf.bpp == 8) {
    st8(off + x, (uint8_t)c);
    return;
  }
  if (s->pf.bpp == 16) {
    st16(off + x * 2, (uint16_t)c);
    return;
  }
  if (s->pf.bpp == 24) {
    uint32_t p = off + x * 3;
    st8(p, (uint8_t)c);
    st8(p + 1, (uint8_t)(c >> 8));
    st8(p + 2, (uint8_t)(c >> 16));
    return;
  }
  st32(off + x * 4, c);
}

void gdi_draw_glyph_surface(DrawSurface* s, int x, int y, int ch, int font_h, uint32_t fg_cref, uint32_t bg_cref,
                            int bk_mode) {
  if (!s || !s->mem) return;
  uint8_t fr = (uint8_t)(fg_cref & 0xff), fg = (uint8_t)((fg_cref >> 8) & 0xff), fb = (uint8_t)((fg_cref >> 16) & 0xff);
  uint8_t br = (uint8_t)(bg_cref & 0xff), bg = (uint8_t)((bg_cref >> 8) & 0xff), bb = (uint8_t)((bg_cref >> 16) & 0xff);
  const uint8_t* pal = s->palette ? s->palette->entries : nullptr;
  if (ch < 32 || ch > 127) ch = '?';
  const uint8_t* glyph = kFont8x16[ch - 32];
  const int scale = std::max(1, (font_h + 8) / 16);
  // Palette lookups once per glyph, not per pixel (the game redraws its menu text every frame).
  const bool indexed = s->pf.bpp == 8 && pal;
  const uint32_t fg_index = indexed ? nearest_palette_index(pal, fr, fg, fb) : 0;
  const uint32_t bg_index = indexed && bk_mode == 2 ? nearest_palette_index(pal, br, bg, bb) : 0;
  for (int row = 0; row < 16; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 8; col++) {
      bool on = (bits >> (7 - col)) & 1;
      for (int sy = 0; sy < scale; sy++)
        for (int sx = 0; sx < scale; sx++) {
          int px = x + col * scale + sx;
          int py = y + row * scale + sy;
          if (indexed) {
            if (on)
              write_pixel(s, px, py, fg_index, pal);
            else if (bk_mode == 2)
              write_pixel(s, px, py, bg_index, pal);
          } else {
            uint32_t c = on ? colorref_to_bgra(fg_cref) : colorref_to_bgra(bg_cref);
            if (on || bk_mode == 2) write_pixel(s, px, py, c, pal);
          }
        }
    }
  }
  s->bump_if_primary();
}

bool colorkey_match(uint32_t pix, uint32_t key, uint32_t bpp) {
  if (bpp == 8) return (pix & 0xFF) == (key & 0xFF);
  if (bpp == 16) return (pix & 0xFFFF) == (key & 0xFFFF);
  return pix == key;
}

void blt_rect(DrawSurface* dst, const RECT* dr, DrawSurface* src, const RECT* sr, uint32_t flags, uint32_t fill_color) {
  if (!dst || !dst->mem) return;
  RECT d = dr ? *dr : RECT{0, 0, (int32_t)dst->w, (int32_t)dst->h};
  if (flags & DDBLT_COLORFILL) {
    if (dst->pf.bpp == 8) {  // fast path: row memset over the clipped rectangle
      const int x0 = std::max<int>(d.left, 0), x1 = std::min<int>(d.right, (int)dst->w);
      for (int y = std::max<int>(d.top, 0); y < std::min<int>(d.bottom, (int)dst->h) && x1 > x0; y++)
        memset(host(dst->mem + y * dst->pitch + x0), (uint8_t)fill_color, x1 - x0);
    } else {
      for (int y = d.top; y < d.bottom; y++)
        for (int x = d.left; x < d.right; x++) write_pixel(dst, x, y, fill_color, nullptr);
    }
    dst->bump_if_primary();
    return;
  }
  if (!src || !src->mem) return;
  RECT s = sr ? *sr : RECT{0, 0, (int32_t)src->w, (int32_t)src->h};
  int dw = d.right - d.left, dh = d.bottom - d.top;
  int sw = s.right - s.left, sh = s.bottom - s.top;
  if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
  // Fast path for the common case (every blit this game makes): same format, no stretching.
  // Clip once, then copy rows; colour-keyed 8-bit sprites skip key bytes.
  if (dw == sw && dh == sh && src->pf.bpp == dst->pf.bpp) {
    int dx = d.left, dy = d.top, sx = s.left, sy = s.top, w = dw, h = dh;
    if (dx < 0) sx -= dx, w += dx, dx = 0;
    if (dy < 0) sy -= dy, h += dy, dy = 0;
    if (sx < 0) dx -= sx, w += sx, sx = 0;
    if (sy < 0) dy -= sy, h += sy, sy = 0;
    w = std::min({w, (int)dst->w - dx, (int)src->w - sx});
    h = std::min({h, (int)dst->h - dy, (int)src->h - sy});
    const int bpp = (int)dst->pf.bpp / 8;
    const bool key = (flags & DDBLT_KEYSRC) && src->has_ck_src;
    if (w > 0 && h > 0 && (!key || bpp == 1)) {
      const bool bottom_up = src == dst && dy > sy;  // overlapping self-blit moving down
      for (int r = 0; r < h; r++) {
        const int row = bottom_up ? h - 1 - r : r;
        uint8_t* dp = host(dst->mem + (dy + row) * dst->pitch + dx * bpp);
        const uint8_t* sp = host(src->mem + (sy + row) * src->pitch + sx * bpp);
        if (!key) {
          memmove(dp, sp, (size_t)w * bpp);
        } else {
          const uint8_t k = (uint8_t)src->ck_src;
          for (int i = 0; i < w; i++)
            if (sp[i] != k) dp[i] = sp[i];
        }
      }
      dst->bump_if_primary();
      if (w == (int)dst->w && h == (int)dst->h && (dst->caps & DDSCAPS_PRIMARYSURFACE)) ddraw::publish_frame();
      return;
    }
  }
  const uint8_t* pal = src->palette ? src->palette->entries : (dst->palette ? dst->palette->entries : nullptr);
  for (int y = 0; y < dh; y++) {
    int sy = s.top + y * sh / dh;
    for (int x = 0; x < dw; x++) {
      int sx = s.left + x * sw / dw;
      uint32_t sp = 0;
      if (src->pf.bpp == 8) sp = ld8(src->mem + sy * src->pitch + sx);
      else if (src->pf.bpp == 16) sp = ld16(src->mem + sy * src->pitch + sx * 2);
      else if (src->pf.bpp == 24) {
        uint32_t p = src->mem + sy * src->pitch + sx * 3;
        sp = ld8(p) | (ld8(p + 1) << 8) | (ld8(p + 2) << 16);
      } else sp = ld32(src->mem + sy * src->pitch + sx * 4);
      if ((flags & DDBLT_KEYSRC) && src->has_ck_src && colorkey_match(sp, src->ck_src, src->pf.bpp)) continue;
      write_pixel(dst, d.left + x, d.top + y, sp, pal);
    }
  }
  dst->bump_if_primary();
}

DrawSurface* alloc_surface(DirectDraw* d, uint32_t w, uint32_t h, const PixelFormat& pf, uint32_t caps) {
  auto* s = new DrawSurface();
  s->dd = d;
  s->w = w;
  s->h = h;
  s->pf = pf;
  s->caps = caps;
  s->pitch = pitch_bytes(w, pf.bpp);
  uint32_t bytes = s->pitch * h;
  s->mem = heap_alloc(process_heap(), bytes, true);
  if (pf.bpp == 8) memset(host(s->mem), 0, bytes);
  return s;
}

// ---- palette ---------------------------------------------------------------------------------

uint32_t pal_SetEntries(Cpu* c, ComObject* self) {
  auto* p = pal(self);
  uint32_t start = arg(c, 2), count = arg(c, 3), entries = arg(c, 4);
  for (uint32_t i = 0; i < count && start + i < 256; i++) {
    uint32_t e = entries + i * 4;
    memcpy(p->entries + (start + i) * 4, host(e), 4);
  }
  return DD_OK;
}

uint32_t pal_GetEntries(Cpu* c, ComObject* self) {
  auto* p = pal(self);
  uint32_t start = arg(c, 2), count = arg(c, 3), entries = arg(c, 4);
  for (uint32_t i = 0; i < count && start + i < 256; i++)
    memcpy(host(entries + i * 4), p->entries + (start + i) * 4, 4);
  return DD_OK;
}

ComInterface& palette_iface() {
  static ComInterface i("IDirectDrawPalette",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetCaps", 1, nullptr},
                         {"GetEntries", 4, pal_GetEntries},
                         {"Initialize", 3, nullptr},
                         {"SetEntries", 4, pal_SetEntries}});
  return i;
}

// ---- clipper ---------------------------------------------------------------------------------

ComInterface& clipper_iface() {
  static ComInterface i("IDirectDrawClipper",
                        {{"QueryInterface", 2, com_QueryInterface_none},
                         {"AddRef", 0, com_AddRef},
                         {"Release", 0, com_Release},
                         {"GetClipList", 3, nullptr},
                         {"GetHWnd", 1, nullptr},
                         {"Initialize", 2, nullptr},
                         {"IsClipListChanged", 1, nullptr},
                         {"SetClipList", 2, nullptr},
                         {"SetHWnd", 2, ok}});
  return i;
}

// ---- surface ---------------------------------------------------------------------------------

uint32_t sfc_QueryInterface(Cpu* c, ComObject* self) {
  Guid iid = read_guid(arg(c, 1));
  uint32_t out = arg(c, 2);
  if (guid_eq(iid, IID_IDirectDrawSurface) || guid_eq(iid, IID_IDirectDrawSurface4) || guid_eq(iid, IID_IDirectDrawSurface7) ||
      guid_eq(iid, IID_IUnknown)) {
    self->refs++;
    if (guid_eq(iid, IID_IDirectDrawSurface4)) st32(out, com_wrap(self, surface4_iface()));
    else if (guid_eq(iid, IID_IDirectDrawSurface7)) st32(out, com_wrap(self, surface7_iface()));
    else st32(out, arg(c, 0));
    return DD_OK;
  }
  if (out) st32(out, 0);
  return E_NOINTERFACE;
}

uint32_t sfc_GetAttachedSurface(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  uint32_t caps = arg(c, 1), out = arg(c, 2);
  uint32_t want = caps ? ld32(caps) : 0;
  DrawSurface* att = nullptr;
  if ((want & DDSCAPS_BACKBUFFER) && s->attached) att = s->attached;
  else if ((want & DDSCAPS_FRONTBUFFER) && s->attached) att = s->attached->attached ? s : s;
  if (!att && s->attached) att = s->attached;
  if (!att) return DDERR_INVALIDPARAMS;
  st32(out, com_wrap(att, surface_iface()));
  return DD_OK;
}

uint32_t sfc_Blt(Cpu* c, ComObject* self) {
  auto* dst = surf(self);
  uint32_t dr = arg(c, 1), src_ptr = arg(c, 2), sr = arg(c, 3), flags = arg(c, 4), fx = arg(c, 5);
  RECT rdst{}, rsrc{};
  RECT* pdr = nullptr;
  RECT* psr = nullptr;
  if (dr) {
    memcpy(&rdst, host(dr), 16);
    pdr = &rdst;
  }
  if (sr) {
    memcpy(&rsrc, host(sr), 16);
    psr = &rsrc;
  }
  DrawSurface* src = src_ptr ? surf(com_object(src_ptr)) : nullptr;
  uint32_t fill = fx ? ld32(fx + 4) : 0;
  blt_rect(dst, pdr, src, psr, flags, fill);
  return DD_OK;
}

uint32_t sfc_BltFast(Cpu* c, ComObject* self) {
  auto* dst = surf(self);
  int x = (int)arg(c, 1), y = (int)arg(c, 2);
  DrawSurface* src = surf(com_object(arg(c, 3)));
  uint32_t sr = arg(c, 4), flags = arg(c, 5);
  RECT rsrc{};
  if (sr) memcpy(&rsrc, host(sr), 16);
  else
    rsrc = {0, 0, (int32_t)src->w, (int32_t)src->h};
  RECT rdst = {x, y, x + (rsrc.right - rsrc.left), y + (rsrc.bottom - rsrc.top)};
  uint32_t f = 0;
  if (flags & DDBLTFAST_SRCCOLORKEY) f |= DDBLT_KEYSRC;
  blt_rect(dst, &rdst, src, &rsrc, f, 0);
  return DD_OK;
}

uint32_t sfc_Flip(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  if (s->attached) {
    DrawSurface* a = s->attached;
    std::swap(s->mem, a->mem);
    std::swap(s->pitch, a->pitch);
    if (g_visible_primary == s) g_visible_primary = a;
    else if (g_visible_primary == a) g_visible_primary = s;
  }
  g_frame_counter++;
  g_stats[StatFlips]++;
  // Debug slow motion (set_time_scale < 1): stretch frame-driven logic the same way as the clock.
  if (const double ts = time_scale(); ts < 1.0) std::this_thread::sleep_for(std::chrono::milliseconds((int)(16.0 / ts - 16.0)));
  return DD_OK;
}

uint32_t sfc_Lock(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  g_stats[StatLocks]++;  // the game locks its frame buffer once per rendered frame
  uint32_t desc = arg(c, 2);
  s->lock_count++;
  fill_surface_desc(desc, s, true);
  return DD_OK;
}

uint32_t sfc_Unlock(Cpu*, ComObject* self) {
  if (surf(self)->lock_count) surf(self)->lock_count--;
  return DD_OK;
}

uint32_t sfc_GetSurfaceDesc(Cpu* c, ComObject* self) {
  fill_surface_desc(arg(c, 1), surf(self), false);
  return DD_OK;
}

uint32_t sfc_GetCaps(Cpu* c, ComObject* self) {
  st32(arg(c, 1), surf(self)->caps);
  return DD_OK;
}

uint32_t sfc_GetPixelFormat(Cpu* c, ComObject* self) {
  write_pixel_format(arg(c, 1), surf(self)->pf);
  return DD_OK;
}

uint32_t sfc_SetPalette(Cpu* c, ComObject* self) {
  uint32_t p = arg(c, 1);
  surf(self)->palette = p ? pal(com_object(p)) : nullptr;
  return DD_OK;
}

uint32_t sfc_GetPalette(Cpu* c, ComObject* self) {
  auto* pal_obj = surf(self)->palette;
  uint32_t out = arg(c, 1);
  if (!out) return DDERR_INVALIDPARAMS;
  if (!pal_obj) {
    st32(out, 0);
    return DDERR_NOPALETTEATTACHED;
  }
  st32(out, com_wrap(pal_obj, palette_iface()));
  return DD_OK;
}

uint32_t sfc_SetColorKey(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  uint32_t which = arg(c, 1), ck = arg(c, 2);
  if (!ck) return DDERR_INVALIDPARAMS;
  uint32_t low = ld32(ck), high = ld32(ck + 4);
  if (which & DDCKEY_SRCBLT) {
    s->has_ck_src = true;
    s->ck_src = low;
  }
  if (which & DDCKEY_DESTBLT) {
    s->has_ck_dest = true;
    s->ck_dest = low;
  }
  (void)high;
  return DD_OK;
}

uint32_t sfc_GetColorKey(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  uint32_t which = arg(c, 1), ck = arg(c, 2);
  if (which & DDCKEY_SRCBLT) {
    if (!s->has_ck_src) return DDERR_NOCOLORKEY;
    st32(ck, s->ck_src);
    st32(ck + 4, s->ck_src);
    return DD_OK;
  }
  if (which & DDCKEY_DESTBLT) {
    if (!s->has_ck_dest) return DDERR_NOCOLORKEY;
    st32(ck, s->ck_dest);
    st32(ck + 4, s->ck_dest);
    return DD_OK;
  }
  return DDERR_NOCOLORKEY;
}

uint32_t sfc_GetClipper(Cpu* c, ComObject* self) {
  uint32_t out = arg(c, 1);
  if (!out) return DDERR_INVALIDPARAMS;
  auto* cl = surf(self)->clipper;
  if (!cl) {
    st32(out, 0);
    return DDERR_NOCLIPPER;
  }
  st32(out, com_wrap(cl, clipper_iface()));
  return DD_OK;
}

uint32_t sfc_SetClipper(Cpu* c, ComObject* self) {
  uint32_t cl = arg(c, 1);
  surf(self)->clipper = cl ? static_cast<DrawClipper*>(com_object(cl)) : nullptr;
  return DD_OK;
}

uint32_t sfc_GetDC(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  if (!s->hdc) {
    static std::atomic<uint32_t> next_hdc{kDdrawHdcBase};
    s->hdc = next_hdc++;
  }
  gdi_dc(s->hdc).surface = s;
  st32(arg(c, 1), s->hdc);
  return DD_OK;
}

uint32_t sfc_ReleaseDC(Cpu*, ComObject*) { return DD_OK; }

uint32_t sfc_IsLost(Cpu*, ComObject*) { return S_FALSE; }

uint32_t sfc_Restore(Cpu*, ComObject*) { return DD_OK; }

uint32_t sfc_Release(Cpu* c, ComObject* self) {
  auto* s = surf(self);
  if (s->refs.load() == 1) {
    if (g_visible_primary == s) g_visible_primary = nullptr;
    if (s->dd && s->dd->primary == s) s->dd->primary = nullptr;
  }
  return com_Release(c, self);
}

#define SURF_METHODS                                                                                 \
  {"QueryInterface", 2, sfc_QueryInterface}, {"AddRef", 0, com_AddRef}, {"Release", 0, sfc_Release}, \
      {"AddAttachedSurface", 1, nullptr}, {"AddOverlayDirtyRect", 1, nullptr},                       \
      {"Blt", 5, sfc_Blt}, {"BltBatch", 3, nullptr}, {"BltFast", 5, sfc_BltFast},                    \
      {"DeleteAttachedSurface", 2, nullptr}, {"EnumAttachedSurfaces", 2, nullptr},                 \
      {"EnumOverlayZOrders", 3, nullptr}, {"Flip", 2, sfc_Flip},                                     \
      {"GetAttachedSurface", 2, sfc_GetAttachedSurface}, {"GetBltStatus", 1, ok},                    \
      {"GetCaps", 1, sfc_GetCaps}, {"GetClipper", 1, sfc_GetClipper}, {"GetColorKey", 2, sfc_GetColorKey},  \
      {"GetDC", 1, sfc_GetDC}, {"GetFlipStatus", 1, ok}, {"GetOverlayPosition", 2, nullptr},         \
      {"GetPalette", 1, sfc_GetPalette}, {"GetPixelFormat", 1, sfc_GetPixelFormat},                 \
      {"GetSurfaceDesc", 1, sfc_GetSurfaceDesc}, {"Initialize", 2, nullptr}, {"IsLost", 0, sfc_IsLost}, \
      {"Lock", 4, sfc_Lock}, {"ReleaseDC", 1, sfc_ReleaseDC}, {"Restore", 0, sfc_Restore},           \
      {"SetClipper", 1, sfc_SetClipper}, {"SetColorKey", 2, sfc_SetColorKey},                      \
      {"SetOverlayPosition", 2, nullptr}, {"SetPalette", 1, sfc_SetPalette}, {"Unlock", 1, sfc_Unlock}, \
      {"UpdateOverlay", 5, nullptr}, {"UpdateOverlayDisplay", 1, nullptr},                           \
      {"UpdateOverlayZOrder", 2, nullptr}

ComInterface& surface_iface() {
  static ComInterface i("IDirectDrawSurface", {SURF_METHODS}, &g_dd_mutex);
  return i;
}

ComInterface& surface4_iface() {
  static ComInterface i("IDirectDrawSurface4",
                        {SURF_METHODS,
                         {"GetDDInterface", 1, nullptr},
                         {"PageLock", 1, nullptr},
                         {"PageUnlock", 1, nullptr},
                         {"SetSurfaceDesc", 2, nullptr},
                         {"SetPrivateData", 4, nullptr},
                         {"GetPrivateData", 3, nullptr},
                         {"FreePrivateData", 1, nullptr},
                         {"GetUniquenessValue", 1, nullptr},
                         {"ChangeUniquenessValue", 0, nullptr}},
                        &g_dd_mutex);
  return i;
}

ComInterface& surface7_iface() {
  static ComInterface i("IDirectDrawSurface7",
                        {SURF_METHODS,
                         {"GetDDInterface", 1, nullptr},
                         {"PageLock", 1, nullptr},
                         {"PageUnlock", 1, nullptr},
                         {"SetSurfaceDesc", 2, nullptr},
                         {"SetPrivateData", 4, nullptr},
                         {"GetPrivateData", 3, nullptr},
                         {"FreePrivateData", 1, nullptr},
                         {"GetUniquenessValue", 1, nullptr},
                         {"ChangeUniquenessValue", 0, nullptr},
                         {"SetPriority", 1, nullptr},
                         {"GetPriority", 1, nullptr},
                         {"SetLOD", 1, nullptr},
                         {"GetLOD", 1, nullptr}},
                        &g_dd_mutex);
  return i;
}

// ---- IDirectDraw -----------------------------------------------------------------------------

uint32_t dd_QueryInterface(Cpu* c, ComObject* self) {
  Guid iid = read_guid(arg(c, 1));
  uint32_t out = arg(c, 2);
  if (guid_eq(iid, IID_IDirectDraw) || guid_eq(iid, IID_IUnknown)) {
    self->refs++;
    st32(out, arg(c, 0));
    return DD_OK;
  }
  if (guid_eq(iid, IID_IDirectDraw2)) {
    self->refs++;
    st32(out, com_wrap(self, dd2_iface()));
    return DD_OK;
  }
  if (guid_eq(iid, IID_IDirectDraw4)) {
    self->refs++;
    st32(out, com_wrap(self, dd4_iface()));
    return DD_OK;
  }
  if (guid_eq(iid, IID_IDirectDraw7)) {
    self->refs++;
    st32(out, com_wrap(self, dd7_iface()));
    return DD_OK;
  }
  if (out) st32(out, 0);
  return E_NOINTERFACE;
}

uint32_t dd_SetCooperativeLevel(Cpu* c, ComObject*) {
  g_hwnd = (HWND)arg(c, 1);
  g_coop_flags = arg(c, 2);
  return DD_OK;
}

uint32_t dd_SetDisplayMode_impl(uint32_t w, uint32_t h, uint32_t bpp) {
  g_mode.w = w ? w : 640;
  g_mode.h = h ? h : 480;
  g_mode.bpp = bpp ? bpp : 8;
  g_mode.pf = mode_pixel_format(g_mode);
  g_mode_set = true;
  return DD_OK;
}

uint32_t dd_SetDisplayMode1(Cpu* c, ComObject*) {
  return dd_SetDisplayMode_impl(arg(c, 1), arg(c, 2), arg(c, 3));
}

uint32_t dd_SetDisplayMode2(Cpu* c, ComObject*) {
  return dd_SetDisplayMode_impl(arg(c, 1), arg(c, 2), arg(c, 3));
}

uint32_t dd_GetDisplayMode(Cpu* c, ComObject*) {
  fill_display_mode_desc(arg(c, 1), g_mode);
  return DD_OK;
}

uint32_t dd_RestoreDisplayMode(Cpu*, ComObject*) {
  if (g_mode_set) g_mode = g_saved_mode;
  return DD_OK;
}

uint32_t dd_GetVerticalBlankStatus(Cpu* c, ComObject*) {
  if (arg(c, 1)) st32(arg(c, 1), 0);
  return DD_OK;
}

uint32_t dd_WaitForVerticalBlank(Cpu*, ComObject*) { return DD_OK; }

uint32_t dd_CreatePalette(Cpu* c, ComObject*) {
  uint32_t flags = arg(c, 1), table = arg(c, 2), out = arg(c, 3);
  auto* p = new DrawPalette();
  p->caps = flags;
  if (table) memcpy(p->entries, host(table), 256 * 4);
  st32(out, com_wrap(p, palette_iface()));
  return DD_OK;
}

uint32_t dd_CreateClipper(Cpu* c, ComObject*) {
  uint32_t out = arg(c, 2);
  st32(out, com_wrap(new DrawClipper(), clipper_iface()));
  return DD_OK;
}

uint32_t dd_CreateSurface(Cpu* c, ComObject* self) {
  auto* d = dd(self);
  uint32_t desc = arg(c, 1), out = arg(c, 2);
  uint32_t w = ld32(desc + 12), h = ld32(desc + 8);
  uint32_t caps = ld32(desc + 0x68);
  PixelFormat pf = read_pixel_format(desc + 0x48);
  if (!w) w = g_mode.w;
  if (!h) h = g_mode.h;
  if (!pf.bpp) pf = g_mode.pf;

  if ((caps & DDSCAPS_PRIMARYSURFACE) && (caps & DDSCAPS_FLIP)) {
    uint32_t back_count = ld32(desc + 20);
    if (!back_count) back_count = 1;
    auto* front = alloc_surface(d, w, h, pf, DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    auto* back = alloc_surface(d, w, h, pf, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    front->attached = back;
    back->attached = front;
    d->primary = front;
    g_visible_primary = front;
    st32(out, com_wrap(front, surface_iface()));
    return DD_OK;
  }

  uint32_t scaps = caps ? caps : (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
  auto* s = alloc_surface(d, w, h, pf, scaps);
  if (caps & DDSCAPS_PRIMARYSURFACE) {
    d->primary = s;
    g_visible_primary = s;
    s->caps |= DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER;
  }
  st32(out, com_wrap(s, surface_iface()));
  return DD_OK;
}

uint32_t dd_GetCaps(Cpu* c, ComObject*) {
  uint32_t drv = arg(c, 1), hel = arg(c, 2);
  for (uint32_t p : {drv, hel}) {
    if (!p) continue;
    uint32_t sz = ld32(p);
    if (sz < 4) continue;
    memset(host(p + 4), 0, sz > 4 ? sz - 4 : 0);
    st32(p + 4, 0x00020000);  // CKEYSRC / CKEYDEST
    st32(p + 8, 0x00000001);  // blt
  }
  return DD_OK;
}

uint32_t dd_EnumDisplayModes(Cpu* c, ComObject*) {
  uint32_t flags = arg(c, 1), ctx = arg(c, 3), cb = arg(c, 4);
  (void)arg(c, 2);
  if (!cb) return DDERR_INVALIDPARAMS;
  uint32_t mode_desc = heap_alloc(process_heap(), 108, true);
  st32(mode_desc, 108);
  fill_display_mode_desc(mode_desc, g_mode);
  guest_call(c, cb, {mode_desc, ctx});
  heap_free(process_heap(), mode_desc);
  if (flags & DDSDM_DEFAULT) return DD_OK;
  return DD_OK;
}

#define DD_COMMON                                                                                        \
  {"QueryInterface", 2, dd_QueryInterface}, {"AddRef", 0, com_AddRef}, {"Release", 0, com_Release},    \
      {"Compact", 0, ok}, {"CreateClipper", 3, dd_CreateClipper}, {"CreatePalette", 4, dd_CreatePalette}, \
      {"CreateSurface", 3, dd_CreateSurface}, {"DuplicateSurface", 2, nullptr},                          \
      {"EnumDisplayModes", 4, dd_EnumDisplayModes}, {"EnumSurfaces", 4, nullptr},                        \
      {"FlipToGDISurface", 0, nullptr}, {"GetCaps", 2, dd_GetCaps}, {"GetDisplayMode", 1, dd_GetDisplayMode}, \
      {"GetFourCCCodes", 2, nullptr}, {"GetGDISurface", 1, nullptr}, {"GetMonitorFrequency", 1, nullptr}, \
      {"GetScanLine", 1, nullptr}, {"GetVerticalBlankStatus", 1, dd_GetVerticalBlankStatus},             \
      {"Initialize", 1, ok}, {"RestoreDisplayMode", 0, dd_RestoreDisplayMode},                          \
      {"SetCooperativeLevel", 2, dd_SetCooperativeLevel}, {"WaitForVerticalBlank", 2, dd_WaitForVerticalBlank}

ComInterface& dd1_iface() {
  static ComInterface i("IDirectDraw",
                        {DD_COMMON, {"SetDisplayMode", 3, dd_SetDisplayMode1}}, &g_dd_mutex);
  return i;
}

ComInterface& dd2_iface() {
  static ComInterface i("IDirectDraw2",
                        {DD_COMMON,
                         {"SetDisplayMode", 5, dd_SetDisplayMode2},
                         {"GetAvailableVidMem", 3, ok}},
                        &g_dd_mutex);
  return i;
}

ComInterface& dd4_iface() {
  static ComInterface i("IDirectDraw4",
                        {DD_COMMON,
                         {"SetDisplayMode", 5, dd_SetDisplayMode2},
                         {"GetAvailableVidMem", 3, ok},
                         {"GetSurfaceFromDC", 2, nullptr},
                         {"RestoreAllSurfaces", 0, ok},
                         {"TestCooperativeLevel", 0, ok},
                         {"GetDeviceIdentifier", 2, nullptr}},
                        &g_dd_mutex);
  return i;
}

ComInterface& dd7_iface() {
  static ComInterface i("IDirectDraw7",
                        {DD_COMMON,
                         {"SetDisplayMode", 5, dd_SetDisplayMode2},
                         {"GetAvailableVidMem", 3, ok},
                         {"GetSurfaceFromDC", 2, nullptr},
                         {"RestoreAllSurfaces", 0, ok},
                         {"TestCooperativeLevel", 0, ok},
                         {"GetDeviceIdentifier", 2, nullptr},
                         {"StartModeTest", 3, nullptr},
                         {"EvaluateMode", 2, nullptr}},
                        &g_dd_mutex);
  return i;
}

}  // namespace

// ---- public API ------------------------------------------------------------------------------

namespace ddraw {

std::atomic<uint64_t> g_compose_time_us{0};
std::atomic<uint64_t> g_last_compose_us{0};

uint64_t frame_counter() { return g_frame_counter.load(); }

uint64_t compose_time_us() { return g_compose_time_us.load(); }

uint64_t last_compose_us() { return g_last_compose_us.load(); }

namespace {

std::vector<uint8_t> g_rgba_scratch;
uint32_t g_pal_lut[256];

inline uint32_t rgba_from_palette_entry(const uint8_t* e) {
  return 0xFF000000u | (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16);
}

void build_palette_lut(const uint8_t* pal256x4) {
  if (!pal256x4) {
    for (int i = 0; i < 256; i++) g_pal_lut[i] = 0xFF000000u | (uint32_t)i;
    return;
  }
  for (int i = 0; i < 256; i++) g_pal_lut[i] = rgba_from_palette_entry(pal256x4 + i * 4);
}

// Movie/clue cinematics blit via GDI on top of the DirectDraw primary (Mattel player uses SetDIBitsToDevice).
void overlay_gdi_on_rgba(uint32_t* dst, int w, int h) {
  if (g_gdi_rgba.empty() || w <= 0 || h <= 0) return;
  const int gw = g_gdi_w;
  const int gh = g_gdi_h;
  const int cw = std::min(w, gw);
  const int ch = std::min(h, gh);
  for (int y = 0; y < ch; y++) {
    uint32_t* row = dst + (size_t)y * w;
    const uint32_t* src = g_gdi_rgba.data() + (size_t)y * gw;
    for (int x = 0; x < cw; x++) {
      uint32_t c = src[x];
      if ((c & 0xFF000000u) != 0) row[x] = c;
    }
  }
}

void compose_surface_rgba8(DrawSurface* src, uint8_t* dst, int w, int h) {
  const uint8_t* pal = src->palette ? src->palette->entries : nullptr;
  if (src->pf.bpp == 8 && pal) {
    build_palette_lut(pal);
    uint32_t* out = (uint32_t*)dst;
    for (int y = 0; y < h; y++) {
      uint32_t off = src->mem + y * src->pitch;
      uint32_t* row = out + (size_t)y * w;
      for (int x = 0; x < w; x++) row[x] = g_pal_lut[ld8(off + x)];
    }
    return;
  }
  if (src->pf.bpp == 32) {
    uint32_t* out = (uint32_t*)dst;
    for (int y = 0; y < h; y++) {
      uint32_t off = src->mem + y * src->pitch;
      for (int x = 0; x < w; x++) {
        uint32_t p = off + x * 4;
        out[(size_t)y * w + x] = 0xFF000000u | (uint32_t)ld8(p + 2) | ((uint32_t)ld8(p + 1) << 8) | ((uint32_t)ld8(p) << 16);
      }
    }
    return;
  }
  uint32_t* out = (uint32_t*)dst;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint32_t bgra = read_pixel(src, x, y, pal);
      out[(size_t)y * w + x] = (bgra & 0xFF00FF00u) | ((bgra & 0xFFu) << 16) | ((bgra >> 16) & 0xFFu);
    }
  }
}

}  // namespace

namespace {
std::mutex g_snap_mutex;      // guards the published snapshot only (held for one memcpy)
std::vector<uint8_t> g_snap, g_snap_work;
int g_snap_w = 0, g_snap_h = 0;
std::chrono::steady_clock::time_point g_snap_time;
std::atomic<bool> g_snap_valid{false};
}  // namespace

void frame_boundary() {
  if (!t_drew_primary) return;
  g_stats[StatDdThreadSleeps]++;
  publish_frame();
}

void publish_frame() {
  const auto now = std::chrono::steady_clock::now();
  if (g_snap_valid && now - g_snap_time < std::chrono::milliseconds(8)) return;  // >120 fps is wasted work
  int w = 0, h = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
    DrawSurface* src = g_visible_primary;
    if (!src || !src->mem) return;  // GDI-only screens use the regular compose path
    w = (int)src->w;
    h = (int)src->h;
    g_snap_work.resize((size_t)w * h * 4);
    compose_surface_rgba8(src, g_snap_work.data(), w, h);
    overlay_gdi_on_rgba((uint32_t*)g_snap_work.data(), w, h);
  }
  std::lock_guard<std::mutex> lock(g_snap_mutex);
  g_snap.swap(g_snap_work);
  g_snap_w = w;
  g_snap_h = h;
  g_snap_time = now;
  g_snap_valid = true;
  g_stats[StatSnapshots]++;
}

// Latest whole frame from frame_boundary, if one was published recently (otherwise the caller
// composes the live surface: movie and GDI screens have no frame-cap Sleep on the game thread).
static bool take_snapshot(uint8_t* dst, int dst_bytes, int& w, int& h) {
  if (!g_snap_valid) return false;
  std::lock_guard<std::mutex> lock(g_snap_mutex);
  if (std::chrono::steady_clock::now() - g_snap_time > std::chrono::milliseconds(100)) return false;
  if ((size_t)dst_bytes < g_snap.size()) return false;
  w = g_snap_w;
  h = g_snap_h;
  memcpy(dst, g_snap.data(), g_snap.size());
  return true;
}

bool compose(std::vector<uint32_t>& rgba, int& w, int& h) {
  std::unique_lock<std::recursive_mutex> lock(g_dd_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  DrawSurface* src = g_visible_primary;
  if (!src || !src->mem) {
    if (g_gdi_rgba.empty()) return false;
    w = g_gdi_w;
    h = g_gdi_h;
    rgba = g_gdi_rgba;
    return true;
  }
  w = (int)src->w;
  h = (int)src->h;
  rgba.resize((size_t)w * h);
  const uint8_t* pal = src->palette ? src->palette->entries : nullptr;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) rgba[(size_t)y * w + x] = read_pixel(src, x, y, pal);
  overlay_gdi_on_rgba(rgba.data(), w, h);
  return true;
}

bool compose_rgba8(uint8_t* dst, int dst_bytes, int& w, int& h, uint8_t** out_buf) {
  const auto t0 = std::chrono::steady_clock::now();
  if (dst && take_snapshot(dst, dst_bytes, w, h)) return true;
  std::unique_lock<std::recursive_mutex> lock(g_dd_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  DrawSurface* src = g_visible_primary;
  if (!src || !src->mem) {
    if (g_gdi_rgba.empty()) return false;
    w = g_gdi_w;
    h = g_gdi_h;
    const size_t need = (size_t)w * h * 4;
    if (!dst || dst_bytes < (int)need) {
      if (g_rgba_scratch.size() < need) g_rgba_scratch.resize(need);
      dst = g_rgba_scratch.data();
      if (out_buf) *out_buf = dst;
    }
    const uint32_t* src32 = g_gdi_rgba.data();
    uint32_t* out = (uint32_t*)dst;
    for (size_t i = 0; i < (size_t)w * h; i++) {
      uint32_t bgra = src32[i];
      out[i] = (bgra & 0xFF00FF00u) | ((bgra & 0xFFu) << 16) | ((bgra >> 16) & 0xFFu);
    }
  } else {
    w = (int)src->w;
    h = (int)src->h;
    const size_t need = (size_t)w * h * 4;
    if (!dst || dst_bytes < (int)need) {
      if (g_rgba_scratch.size() < need) g_rgba_scratch.resize(need);
      dst = g_rgba_scratch.data();
      if (out_buf) *out_buf = dst;
    }
    compose_surface_rgba8(src, dst, w, h);
    overlay_gdi_on_rgba((uint32_t*)dst, w, h);
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  g_last_compose_us.store((uint64_t)us);
  g_compose_time_us.fetch_add((uint64_t)us);
  return true;
}

void gdi_blit_dib(int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, uint32_t bits, uint32_t bmi,
                  bool bottom_up) {
  g_stats[StatGdiBlits]++;
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  if (!bmi) return;
  int bpp = (int)ld16(bmi + 14);
  if (!bpp) bpp = 8;
  int src_w = (int)ld32(bmi + 4);
  int src_h = (int)std::abs((int32_t)ld32(bmi + 8));
  if (sw <= 0) sw = src_w - sx;
  if (sh <= 0) sh = src_h - sy;
  if (dw <= 0) dw = sw;
  if (dh <= 0) dh = sh;
  if (g_gdi_w < dx + dw || g_gdi_h < dy + dh) {
    int nw = std::max(g_gdi_w, dx + dw);
    int nh = std::max(g_gdi_h, dy + dh);
    if (nw < 640) nw = 640;
    if (nh < 480) nh = 480;
    g_gdi_rgba.assign((size_t)nw * nh, 0xFF000000);
    g_gdi_w = nw;
    g_gdi_h = nh;
  }
  uint32_t pal_off = bmi + 40;
  const uint32_t row_pitch = (bpp == 8) ? (uint32_t)((src_w + 3) / 4) * 4 : (uint32_t)src_w * (bpp / 8);
  for (int y = 0; y < dh; y++) {
    int sy_row = bottom_up ? (sy + (sh - 1 - y * sh / dh)) : (sy + y * sh / dh);
    for (int x = 0; x < dw; x++) {
      int sx_col = sx + x * sw / dw;
      uint32_t c = 0xFF000000;
      if (bpp == 8) {
        uint8_t idx = ld8(bits + sy_row * row_pitch + sx_col);
        uint8_t* e = host(pal_off + idx * 4);  // RGBQUAD: B,G,R,X
        c = bgra_from_rgb(e[2], e[1], e[0]);
      } else if (bpp == 16) {
        uint32_t pitch = ((src_w * 2 + 3) / 4) * 4;
        uint16_t v = (uint16_t)ld16(bits + sy_row * pitch + sx_col * 2);
        c = bgra_from_rgb((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3);
      } else if (bpp == 24) {
        uint32_t p = bits + sy_row * src_w * 3 + sx_col * 3;
        c = bgra_from_rgb(ld8(p + 2), ld8(p + 1), ld8(p));
      } else {
        uint32_t p = bits + sy_row * src_w * 4 + sx_col * 4;
        c = bgra_from_rgb(ld8(p + 2), ld8(p + 1), ld8(p));
      }
      int ox = dx + x, oy = dy + y;
      if (ox >= 0 && oy >= 0 && ox < g_gdi_w && oy < g_gdi_h) g_gdi_rgba[(size_t)oy * g_gdi_w + ox] = c;
    }
  }
  g_frame_counter++;
}

void gdi_bind_surface_hdc(uint32_t hdc, void* surface_opaque) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  gdi_dc(hdc).surface = static_cast<DrawSurface*>(surface_opaque);
}

uint32_t gdi_create_font(int height_px, int weight) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  if (height_px < 0) height_px = -height_px;
  if (height_px == 0) height_px = 16;
  if (weight <= 0) weight = 400;
  uint32_t h = g_next_font_handle++;
  g_gdi_fonts[h] = GdiFont{height_px, weight};
  return h;
}

uint32_t gdi_select_font(uint32_t hdc, uint32_t font_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  auto& dc = gdi_dc(hdc);
  uint32_t prev = (uint32_t)dc.font_handle;
  if (g_gdi_fonts.count(font_handle)) dc.font_handle = (int)font_handle;
  return prev ? prev : font_handle;
}

void gdi_set_bk_mode(uint32_t hdc, int mode) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  gdi_dc(hdc).bk_mode = mode;
}

uint32_t gdi_set_text_color(uint32_t hdc, uint32_t colorref) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  auto& dc = gdi_dc(hdc);
  uint32_t prev = dc.text_color;
  dc.text_color = colorref;
  return prev;
}

bool gdi_get_char_widths(uint32_t hdc, uint32_t first, uint32_t last, uint32_t guest_widths) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  const GdiFont& font = gdi_font_for_dc(gdi_dc(hdc));
  if (!guest_widths) return false;
  for (uint32_t ch = first; ch <= last; ch++)
    st32(guest_widths + (ch - first) * 4, gdi_glyph_advance_px((int)ch, font.height));
  return true;
}

bool gdi_text_out(uint32_t hdc, int x, int y, uint32_t guest_str, int len) {
  std::lock_guard<std::recursive_mutex> lock(g_dd_mutex);
  if (!guest_str || len <= 0) return false;
  auto& dc = gdi_dc(hdc);
  const GdiFont& font = gdi_font_for_dc(dc);
  uint32_t fg = colorref_to_bgra(dc.text_color);
  uint32_t bg = colorref_to_bgra(dc.bk_color);
  DrawSurface* surf = dc.surface;
  if (hdc == ddraw::kGdiWindowHdc) surf = nullptr;
  int cx = x;
  for (int i = 0; i < len; i++) {
    uint8_t ch = ld8(guest_str + i);
    if (!ch) break;
    if (surf)
      gdi_draw_glyph_surface(surf, cx, y, (int)ch, font.height, dc.text_color, dc.bk_color, dc.bk_mode);
    else
      gdi_draw_glyph_overlay(cx, y, (int)ch, font.height, fg, bg, dc.bk_mode);
    cx += gdi_glyph_advance_px((int)ch, font.height);
  }
  if (!surf) g_frame_counter++;
  return true;
}

}  // namespace ddraw

HLE_STDCALL("ddraw.dll", DirectDrawCreate, 3) {
  g_saved_mode = g_mode;
  g_mode.pf = mode_pixel_format(g_mode);
  auto* d = new DirectDraw();
  st32(arg(c, 1), com_wrap(d, dd1_iface()));
  log(Log::Info, "DirectDrawCreate -> software surfaces");
  return DD_OK;
}

void hle_link_ddraw() {}
