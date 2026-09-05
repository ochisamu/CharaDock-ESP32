# CharaDock StackChan firmware

Early hardware bring-up firmware for the M5Stack StackChan K151 (CoreS3). CharaDock remains the source of truth for character, conversation, voice, expression, and device presentation.

## Current vertical slice

- Independent PlatformIO project for `m5stack-cores3`
- M5Stack's StackChan BSP pinned to a reviewed commit
- 320×240 double-buffered Native Face renderer
- connecting, idle, listening, thinking, speaking, and error states
- neutral, listening, thinking, happy, surprised, and soft expressions
- 3-level mouth state and randomized 4–7 second blink
- head-touch short action and push-to-talk long press preview
- state-matched RGB feedback
- explicit speaker tone and microphone RMS bring-up checks
- conservative, explicit-only servo test; servo power is off at boot
- device-neutral protocol v2 frame codec and capability schema
- Hybrid / Character Art / Native Face presentation model with artwork-policy fallback
- atomic, CRC-verified RGB565 portrait staging in two PSRAM slots
- 1.5 second Hybrid portrait presentation and persistent Static Character Art
- played-PCM mouth envelope with hysteresis, release hold, and a 10 Hz update cap
- USB PCM16 speaker stream with 80–500 ms configurable prebuffer
- 1.024 second circular jitter buffer and three rotating 20 ms speaker blocks
- idempotent audio retry, bounded backpressure, immediate touch/`AudioStop` interruption
- strict WAV diagnostic sender with real-time pacing and click-reducing edge fades

The portrait and speaker receivers can be exercised over USB before CharaDock's authenticated protocol-v2 host is connected. Wi-Fi/authenticated transport, microphone PCM upload, and CharaDock PC integration are intentionally not enabled before the first physical-device validation.

Portrait storage uses two 153,600-byte PSRAM slots. A new transfer is displayed only after every sequential chunk arrives and the full RGB565 payload passes CRC-32. A failed or interrupted transfer leaves the previous verified portrait active; missing PSRAM falls back to Native Face.

## Build

```powershell
pio run --project-dir firmware/stackchan
pio run --project-dir firmware/stackchan --target upload --upload-port COM3
pio device monitor --port COM3 --baud 500000
```

The board normally appears as a USB CDC serial device. Hold the reset button for at least three seconds if it must be forced into download mode.

To create one merged image that can be flashed at address `0x0`, including bootloader and partition data, run:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-stackchan-release.ps1 `
  -Version 0.1.0-preview
```

The ignored `dist\stackchan` directory receives the merged bin and `SHA256SUMS.txt`. The script performs a clean build, embeds the requested version, and rejects local build paths in the distributable image.

After preserving the factory-firmware details and completing the safety check below, the merged preview can be written with:

```powershell
py -m esptool --chip esp32s3 --port COM3 --baud 921600 `
  write_flash 0x0 `
  .\dist\stackchan\CharaDock-StackChan-K151-v0.1.0-preview.bin
```

This image is build-verified but remains a preview until it passes the first physical-device checklist.

## First-device safety check

1. Photograph or note the factory firmware version before flashing.
2. Power the CoreS3 directly over USB-C for the first flash.
3. Keep the robot clear of objects and do not rotate powered servos by hand.
4. Verify the display, head touch, PSRAM, LEDs, battery telemetry, speaker, and microphones before enabling motion.
5. Confirm the physical neutral position before sending `motion on`.
6. Run `motion test` only with the robot on a stable surface. It moves about 8° left/right and returns home.
7. Keep microSD removed for the first run; the MVP must not depend on it.

## Bring-up console

The serial console accepts:

```text
capabilities
status
state connecting|idle|listening|thinking|speaking|error
expression neutral|listening|thinking|happy|surprised|soft
mouth 0|1|2
mode hybrid|character-art|native-face
theme FFF5DF 171513 D99A24
artwork on|off
portrait test|show|invalidate
motion on|off|test
audio speaker-test|mic-test|envelope-preview|stream-stop
```

`portrait test` generates a local RGB565 test face, sends it through the same staged cache path, and shows it using the current display mode. In Hybrid mode, `portrait show` or a short head touch displays it for 1.5 seconds before returning to Native Face. Character Art keeps it visible. `audio envelope-preview` exercises the three mouth levels without a network connection.

The current firmware treats a short head touch as a portrait/demo request and a 500 ms hold as a PTT preview. Touch-down gives immediate visual feedback. These events become protocol messages when the authenticated v2 transport lands.

Touching the head while PCM is playing interrupts the speaker immediately. A short release remains idle and does not also trigger the portrait action; continuing to hold transitions into the PTT preview. This keeps interruption and long-press input on one predictable gesture path.

## Send a real portrait over USB

The serial port accepts protocol-v2 frames at the start of a line as well as the text console. This USB-only helper crops and composites a local image on the PC, converts it to RGB565, transfers 4092 bytes at a time, waits for every acknowledgement, and commits it only after the device verifies the whole-image CRC:

```powershell
py -m pip install pillow pyserial
py .\firmware\stackchan\tools\send_portrait.py `
  --port COM3 `
  --mode hybrid `
  C:\path\to\portrait.png
```

Use `--mode character-art` to keep the portrait on screen. The helper derives the revision from SHA-256 unless `--revision` is supplied. It does not upload anything to a cloud service or add the source image to this repository. Only transfer artwork whose device-display use is permitted.

## Send test audio over USB

The audio helper accepts only the firmware wire format: uncompressed 16 kHz mono PCM16 WAV. It validates the file before opening the serial port, applies a 5 ms edge fade by default, fills the requested prebuffer, then paces subsequent chunks against audio time. Missing acknowledgements and temporary ring-buffer pressure reuse the exact same sequence, so firmware retry protection cannot append a chunk twice.

```powershell
py -m pip install pyserial
py .\firmware\stackchan\tools\send_audio.py `
  --port COM3 `
  --volume 72 `
  C:\path\to\stackchan-16k-mono.wav
```

For a first check with no audio file, play the built-in bounded speech-band chime:

```powershell
py .\firmware\stackchan\tools\send_audio.py --port COM3 --test-tone
```

If conversion is needed, one possible FFmpeg command is:

```powershell
ffmpeg -i .\input.wav -ac 1 -ar 16000 -c:a pcm_s16le .\stackchan-16k-mono.wav
```

Press Ctrl+C or touch the head to interrupt. `status` reports `audio`, ring occupancy, and the most recent stream's underrun count; `audio stream-stop` is the console fallback. `AudioEnd` drains already accepted samples instead of cutting off the final syllable.

## Host-side logic test

The protocol and presentation state machine are hardware-independent:

```bash
g++ -std=c++17 -Ifirmware/stackchan/include \
  firmware/stackchan/src/protocol_v2.cpp \
  firmware/stackchan/src/presentation.cpp \
  firmware/stackchan/src/portrait_cache.cpp \
  firmware/stackchan/src/mouth_envelope.cpp \
  firmware/stackchan/src/audio_playback.cpp \
  firmware/stackchan/src/frame_dispatcher.cpp \
  firmware/stackchan/test/protocol_test.cpp \
  -o /tmp/charadock-stackchan-test
/tmp/charadock-stackchan-test

PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/stackchan/test/portrait_tool_test.py

PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/stackchan/test/audio_tool_test.py
```

## Third-party software

The firmware links M5Stack StackChan-BSP, M5Unified, M5GFX, and ArduinoJson. The BSP also declares IR and NFC libraries that this MVP explicitly excludes from linking. Exact versions or commits are pinned in `platformio.ini`; see the repository `THIRD_PARTY_NOTICES.md` before redistribution.
