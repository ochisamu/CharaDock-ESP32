# Waveshare ESP32-S3-RLCD-4.2 firmware

This independent PlatformIO target implements the display, microphone, speaker, controls, sensors, and USB/Wi-Fi transport used by CharaDock on the Waveshare ESP32-S3-RLCD-4.2. It does not modify the ATOM Voice protocol-v1 firmware or the StackChan target. CharaDock on the PC remains authoritative: this firmware does not contain sanoTTS, Open JTalk, a language model, or any other local speech synthesizer.

Hardware references: [Waveshare documentation](https://docs.waveshare.com/ESP32-S3-RLCD-4.2) and [official example repository](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2). The build platform is pinned to [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32) 55.03.311.

## Implemented now

Automatic transport selection prefers a live USB host, then authenticated Wi-Fi.
Standby links retain diagnostic connectivity without controlling capture. Link
changes stop the current recording/playback; USB transmit waits are bounded and
idle heartbeats do not restart microphone calibration. USB charging is unchanged.

Hands-free energy detection is only a speech candidate. It keeps the ambient
clock visible until the PC speech gate sends the Listening state. Rejected
noise returns to idle without opening the portrait. Explicit long-press PTT
still opens the portrait locally; speech admission and display are separate.

- Official 400 × 300 ST7305 landscape panel timing and pin map
- Full-frame 1-bit canvas with Home, Conversation, Work, Offline, and Recovery scenes
- Embedded Shinonome 12/16 px Japanese fonts generated reproducibly from the vendored BDF source
- Device Protocol v2 over USB CDC at 500000 baud and authenticated TCP on a private Wi-Fi LAN
- Atomic scene/text commits and atomic CRC-checked 400 × 300 monochrome portrait transfer
- USB heartbeat, local Offline fallback, exponential reconnect, and full Snapshot recovery
- Debounced KEY and BOOT input events with immediate local feedback
- PCF85063 RTC, SHTC3 temperature/humidity, battery voltage, and codec-presence diagnostics
- 16 kHz PCM16 mono speaker playback through ES8311, I2S, and GPIO 46 PA enable
- 16 kHz PCM16 mono capture from the onboard ES7210 microphones
- KEY push-to-talk and optional local hands-free VAD with configurable threshold and status telemetry
- Press-time microphone preroll, so speech begun before the 420 ms long-press decision is retained
- Optional PSRAM-resident blink and three-stage mouth frames with variable/double blink timing, plus sparse 1–2 px horizontal/diagonal idle motion and a static fallback on capture or underrun
- Five-minute ambient dashboard with a large unscaled character crop on the right, a white-paper hour/minute and environment panel on the left, battery, and immediate KEY toggle
- USB-only Wi-Fi provisioning, UDP discovery, TCP transport, and mutual HMAC-SHA256 authentication
- 512 KiB PSRAM audio ring, whole-utterance buffering for normal TTS, and 256 ms rolling prebuffer for longer streams
- Safe audio sequencing: MCLK before codec setup, verified mute/volume registers, zero DMA prefill, and fail-closed PA disable

The firmware reports `audio.capture=true`, `audio.playback=true`, `audio.duplex=half`, and a fixed `pcm-s16le-mono` / 16 kHz contract. ES8311 playback has been checked on physical hardware against the Waveshare factory audio path. CharaDock generates standard TTS, GPT-Live, and optional Beatrice audio on the PC, then sends only PCM to the device. The reverse path sends microphone PCM to the PC for the selected Chat/Work/Live route. The desktop renders controls from the advertised capabilities rather than the board name.

## Build and flash

Install Python 3.10 or later and PlatformIO Core 6.1.19 or later, then run from the repository root. The first pioarduino build downloads and expands a large ESP32-S3 framework/toolchain bundle and can take several minutes on Windows; if that initial package bootstrap is interrupted, rerun the same command. Its Arduino-ESP32 3.x package directory is intentionally isolated from the ATOM/StackChan Arduino-ESP32 2.x toolchain so the targets can be built in any order.

```powershell
pio run --project-dir firmware/waveshare-rlcd-4.2
powershell -ExecutionPolicy Bypass -File `
  .\scripts\flash-rlcd42.ps1 -Port COM3
pio device monitor --port COM3 --baud 500000
```

Replace `COM3` with the actual port. The update script writes discrete boot and application segments with esptool progress disabled for Japanese Windows consoles. It deliberately leaves the NVS range untouched so Wi-Fi credentials and the pairing secret survive firmware updates. The pinned board is the 16 MB Flash / 8 MB octal PSRAM ESP32-S3 configuration. The build also creates `.pio/build/waveshare-rlcd-42/firmware.factory.bin`, a combined image for address `0x0`; reserve that image for full recovery because writing it erases NVS and requires Wi-Fi provisioning again.

After flashing, open **Settings → ESP32 devices → RLCD 4.2** in the current CharaDock desktop source. Enable the device, select its USB port if auto-detection is ambiguous, then use **Sync selected character**. The default **Manga** conversion emphasizes clean outlines for the reflective 1-bit panel; the previous illustration dither remains selectable. Turn on the microphone and speaker, select push-to-talk or hands-free, choose the bounded speaker level, and use **Test character voice** after selecting a PCM-capable normal TTS voice. CharaDock verifies the board name and Protocol-v2 display/audio/network capabilities before transferring content.

To go wireless, keep USB connected for initial setup, enter the current 2.4 GHz Wi-Fi SSID and password in the RLCD card, and provision once. CharaDock creates a random 256-bit pairing secret. The password and secret stay in ESP32 NVS; Wi-Fi accepts only a device that proves the secret and the device also verifies the host's HMAC proof. Do not expose UDP 41721 or TCP 41722 to the public internet.

To produce a versioned preview image and checksum:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-rlcd42-release.ps1 `
  -Version 0.2.0-preview
```

See [`CHANGELOG.md`](./CHANGELOG.md) for preview-to-preview behavior changes.

## First physical check

1. Check the speaker connector and 18650 polarity, then flash over USB. PA_EN remains low throughout boot.
2. Confirm that the Recovery screen is upright and fills 400 × 300.
3. Hold BOOT for three seconds to show the local display/PSRAM/RTC/SHTC3/codec diagnostic.
4. Run the USB preflight. It auto-detects a single ESP32-S3 port, or accepts `--port COM3`, and verifies identity, microphone/playback/network capabilities, RTC sync, immediate sensors, and heartbeat without starting capture or playback:

   ```powershell
   py firmware/waveshare-rlcd-4.2/tools/diagnose_device.py --port COM3
   ```

5. Run the sample scene sender and verify that the whole scene changes in one refresh:

   ```powershell
   py firmware/waveshare-rlcd-4.2/tools/send_scene.py `
     --port COM3 --scene conversation --state speaking `
     --character "コハク" --live --beatrice
   ```

6. Optionally send a portrait. Pillow and pyserial are required for the host helpers:

   ```powershell
   py -m pip install Pillow pyserial
   py firmware/waveshare-rlcd-4.2/tools/send_portrait.py `
     --port COM3 --revision portrait-r1 .\portrait.png
   ```

7. In CharaDock, enable RLCD 4.2 and its speaker output, then use the test-voice button. Stop immediately with KEY or the CharaDock stop action if the speaker connector is loose, the battery warning LED appears, or the board becomes hot.
8. Select push-to-talk, press and hold KEY while speaking, and release it to submit. Microphone monitoring begins on press and the 420 ms long-press decision flushes a bounded local preroll, so the beginning of the sentence is retained. Confirm that CharaDock receives 16 kHz microphone PCM and returns the selected PC-generated character voice. Provision Wi-Fi only after this USB round trip succeeds.

KEY short press toggles the local clock/environment dashboard from any settled character scene, including an idle Chat snapshot. Its white-paper information pane sits on the left and the character remains large on the right without resampling; the clock omits seconds to avoid needless reflective-panel refreshes. During an active turn or playback KEY requests interruption. A long KEY press captures and streams microphone PCM, including the locally retained press-time preroll; release ends the utterance. In hands-free mode, the firmware keeps the same bounded local preroll and sends PCM only after the configured voice threshold is crossed. BOOT long press opens diagnostics. The same dashboard opens automatically after five idle minutes and closes as soon as capture, playback, or a host scene begins.

CharaDock may upload neutral, blink, half-mouth, and open-mouth 400x300 monochrome frames before playback. They remain in PSRAM and are selected locally: blink timing varies with an occasional double blink, while mouth state follows the measured PCM peak at up to 4 fps. During an otherwise idle Home scene, the portrait occasionally moves 1–2 pixels horizontally or diagonally, holds briefly, and returns to the exact origin; it never continuously scrolls. A shadow framebuffer sends only the bounding changed tile rectangle to the panel. This works for any character, while standard PuruPuru eye/mouth layers add expression and RLCD-specific line-art variants remain optional overrides. Audio remains authoritative: capture, playback, ambient mode, or increased underruns suppress motion and retain the neutral frame fallback.

CharaDock sends an idle heartbeat automatically on the active USB or authenticated Wi-Fi route. If the PC process or active session disappears for 24 seconds while the board remains powered, the screen keeps the last verified character and moves to Offline. Reconnection performs a complete character and scene sync. A portrait with invalid metadata, a missing chunk, or a failed CRC never replaces or clears the previous verified portrait.

## Font regeneration

Generated font binaries are committed so normal builds do not depend on host locale or network access. Rebuild and verify them with:

```powershell
py scripts/build-shinonome-font.py
py scripts/build-shinonome-font.py --check
```

See `third_party/shinonome/SOURCE.md` and `THIRD_PARTY_NOTICES.md` for provenance and licensing.
