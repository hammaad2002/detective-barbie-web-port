// DirectDraw (ddraw.dll) software surfaces: the interface between the HLE and the presenter.
#pragma once

#include <cstdint>
#include <vector>

namespace ddraw {

// Frame counter bumped by every Flip / primary-surface Blt / GDI DIB blit to the screen.
uint64_t frame_counter();

// Compose the current visible image (primary surface + palette, or GDI screen buffer) as
// 0xAARRGGBB (little-endian BGRA bytes) pixels, w*h entries. Returns false until the game has
// produced a first image. Thread-safe; called from the presenter, not the game thread.
bool compose(std::vector<uint32_t>& rgba, int& w, int& h);

// Web presenter: write RGBA8 (R,G,B,A byte order) into dst (w*h*4 bytes). Reuses an internal
// scratch buffer when dst is null (returns pointer via out_buf). Returns false until ready.
bool compose_rgba8(uint8_t* dst, int dst_bytes, int& w, int& h, uint8_t** out_buf = nullptr);

// The game draws straight onto the visible primary surface (no Flip) and its frame loops end in a
// frame-cap Sleep. Called from Sleep: on the thread that draws the frames, snapshot the finished
// frame so the presenter shows whole frames without contending for the DirectDraw lock.
void frame_boundary();

// Cumulative / last compose time in microseconds (web perf).
uint64_t compose_time_us();
uint64_t last_compose_us();

// GDI DIB blit onto the game window's screen buffer (called by SetDIBitsToDevice/StretchDIBits).
// bmi: guest BITMAPINFO (8/16/24/32 bpp, optional 256-entry palette), bits: guest pointer.
void gdi_blit_dib(int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, uint32_t bits, uint32_t bmi,
                  bool bottom_up_is_native);

// GDI text (TextOutA / CreateFontA / GetCharWidthA) — window DC → g_gdi_rgba overlay; DDraw surface DC → 8-bit
// indices on the locked primary/back-buffer surface.
constexpr uint32_t kGdiWindowHdc = 0x00010020u;

void gdi_bind_surface_hdc(uint32_t hdc, void* surface_opaque);
uint32_t gdi_create_font(int height_px, int weight);
uint32_t gdi_select_font(uint32_t hdc, uint32_t font_handle);
void gdi_set_bk_mode(uint32_t hdc, int mode);
uint32_t gdi_set_text_color(uint32_t hdc, uint32_t colorref);
bool gdi_get_char_widths(uint32_t hdc, uint32_t first, uint32_t last, uint32_t guest_widths);
bool gdi_text_out(uint32_t hdc, int x, int y, uint32_t guest_str, int len);

}  // namespace ddraw
