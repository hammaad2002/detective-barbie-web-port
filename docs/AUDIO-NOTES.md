# Audio — Detective Barbie port

## Native capture (`DETECTIVE_AUDIO_DUMP`)

The software mixer writes post-mix **16-bit stereo PCM** at **44100 Hz** when `DETECTIVE_AUDIO_DUMP=<path.wav>` is set. Capture runs with or without a host audio device. With no SDL device (typical `SDL_VIDEODRIVER=dummy` runs), a **software clock** advances the mixer from `ddraw::frame_counter()` at **60 Hz** video (`DETECTIVE_AUDIO_VIDEO_HZ` overrides).

DirectSound HLE: guest buffer memory, 8-bit unsigned / 16-bit signed, pan & volume in **hundredths of dB**, frequency ratio for pitch, `DSBPLAY_LOOPING`, `GetCurrentPosition` write-ahead ~10 ms, `DSBSTATUS_PLAYING|LOOPING`.

## Test command

```bash
ID=x2-audio
node recomp/src/cli.ts gen-c work/ref/BDETECT.EXE --out work/gen-c-${ID}
cmake -S runtime -B work/build-native-game-${ID} -DGEN_DIR="$PWD/work/gen-c-${ID}"
cmake --build work/build-native-game-${ID} -j8

mkdir -p work
SDL_VIDEODRIVER=dummy \
DETECTIVE_AUDIO_DUMP=work/audio-x2.wav \
DETECTIVE_DUMP_FRAME=700:work/frame-x2.ppm \
DETECTIVE_INPUT="200:move(200,400),205:click(200,400),500:move(480,400),505:click(480,400),560:move(295,440),565:click(295,440)" \
timeout 120 work/build-native-game-${ID}/detective work/install

python3 tests/audio-check.py work/audio-x2.wav
```

Pitch check (optional): compare `work/install/OPENING.WAV` duration to the opening segment in the dump (same perceived tempo when video Hz matches the game).

## Web (`platform_web.cpp`)

Not modified in this worker. Feeds a ring buffer from a detached render thread (~256-frame chunks, ~90 ms target latency). Same mixer/dsound path as native; browser AudioWorklet consumes the ring.

## Unit test

`runtime/tests/dsound_test.cpp` — offline 8-bit mono buffer, play cursor vs `audio::render_offline` (build manually if not in CMake).
