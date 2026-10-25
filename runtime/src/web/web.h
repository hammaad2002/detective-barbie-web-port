// Web build: pacing between the game pthread and the page's animation-frame presenter.
#pragma once

#include <cstdint>
#include <vector>

namespace web {

// Legacy GLES command queue (D3D recorder); unused once the 2D presenter owns the display.
bool gl_ready();
void submit(std::vector<uint32_t>&& words, bool present);
void submit_and_wait(std::vector<uint32_t>&& words);

// Blocks the game thread until the main thread has presented through ddraw::frame_counter().
void present_wait();

// After Flip: wait for main-thread present, then sleep to the next 60 Hz slot.
void flip_paced();

}  // namespace web
