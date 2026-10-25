// Software mixer: DirectSound buffers and decoded Bink audio are mixed into interleaved stereo
// float output at kRate. The platform pulls samples with render().
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace audio {

constexpr int kRate = 44100;

class Source {
 public:
  virtual ~Source() = default;
  // Add this source's contribution to `out` (frames x 2 interleaved floats).
  virtual void mix(float* out, int frames) = 0;
};

void init();
void add(const std::shared_ptr<Source>& s);
void remove(Source* s);
void render(float* out, int frames);
// When no host audio device is open, advance the mixer to match ddraw::frame_counter() (see mixer.cpp).
void sync_to_video_frames();
// Mix without opening the platform audio device (tests call this instead of waiting on render()).
inline void render_offline(float* out, int frames) { render(out, frames); }
// Monotonic count of output frames rendered so far (the audio clock).
uint64_t frames_rendered();

// Gains from DirectSound units: volume in hundredths of dB (0 .. -10000), pan -10000 .. 10000.
float db100_to_gain(int32_t v);

}  // namespace audio
