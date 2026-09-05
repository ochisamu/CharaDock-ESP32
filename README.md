# CharaDock ESP32

Voice and character-device firmware for [CharaDock](https://github.com/ochisamu/CharaDock). The PC remains the conversation runtime; an ESP32 device provides the microphone, speaker, controls, and physical feedback.

[日本語](./README.ja.md)

> This repository is expanding beyond one board. Device-specific code lives under `firmware/<device>` so StackChan, RLCD 4.2, and other ESP32 character devices can share the same host protocol without being presented as ATOM Echo settings in CharaDock.

## Device status

| Device | Input | Output / display | Status |
| --- | --- | --- | --- |
| M5Stack ATOM Voice (formerly ATOM Echo, C008-C) | Button or hands-free VAD | Built-in speaker + RGB LED | Supported |
| M5Stack StackChan K151 | Head touch / PTT vertical slice | Display / RGB / servo vertical slice | CoreS3 firmware implemented; physical validation pending |
| Waveshare ESP32-S3-RLCD-4.2 | KEY PTT / hands-free VAD / BOOT diagnostics | 400×300 monochrome display + onboard speaker | Protocol-v2 USB/Wi-Fi, microphone, speaker, and display preview |

The current stable release targets the original **M5Stack ATOM Voice (formerly ATOM Echo, product code C008-C) with ESP32-PICO-D4**. It is tested with CharaDock v0.5.1. The StackChan K151 and Waveshare RLCD 4.2 implementations are isolated in their own directories and do not change the existing ATOM protocol-v1 target. RLCD 4.2 adds the ST7305 display, five atomic UI scenes, Japanese fonts, manga-oriented portrait transfer, physical controls, RTC/environment/battery diagnostics, ES7210 microphone capture, ES8311 speaker playback, and Device Protocol v2 over USB or authenticated Wi-Fi. CharaDock on the PC owns Chat/Work, speech recognition, standard TTS, GPT-Live, and Beatrice; the RLCD firmware only exchanges PCM and presentation state and contains no local TTS engine or model.

> **Product-name note:** The [Switch Science product page](https://www.switch-science.com/products/6347) records that the product was renamed from “ATOM Echo” to “ATOM Voice” in April 2026. The product code and supported hardware are unchanged. CharaDock v0.5.1 retains “ATOM Echo” in its UI, protocol, and firmware filenames for compatibility.

## What works

- USB setup and fallback audio on a selected COM port
- Authenticated low-latency audio over the same private Wi-Fi as the PC
- Push-to-talk or hands-free voice activity detection
- Chat and Work using the interaction mode selected on the PC
- Standard character TTS, GPT-Live, and optional Beatrice 2 conversion
- Output gain and microphone threshold controls in CharaDock
- Physical interruption during thinking or playback
- Voice-oriented PC DSP and stereo-slot I2S output for the small built-in speaker

Audio is half-duplex. Wake words, acoustic echo cancellation, public internet relay, and over-the-air firmware updates are not included.

## Install the prebuilt firmware

Download `CharaDock-ATOM-Echo-v0.5.2.bin` and `SHA256SUMS.txt` from the [latest GitHub release](https://github.com/ochisamu/CharaDock-ESP32/releases). Verify the checksum, close CharaDock so it releases the COM port, then flash the merged image at address `0x0` with an ESP32 flashing tool.

Example with Python and esptool:

```powershell
py -m pip install --upgrade esptool
py -m esptool --chip esp32 --port COM3 erase_flash
py -m esptool --chip esp32 --port COM3 --baud 460800 write_flash 0x0 .\CharaDock-ATOM-Echo-v0.5.2.bin
```

Replace `COM3` with the port assigned on your PC. Erasing flash also removes previously stored Wi-Fi credentials and pairing data.

## Connect it to CharaDock

1. Install and start [CharaDock v0.5.1 or later](https://github.com/ochisamu/CharaDock/releases).
2. Open **Settings → ESP32 devices → ATOM Echo**.
3. Connect the device by USB, enable it, and select its COM port if automatic detection does not find it.
4. To use wireless audio, enter the PC's current Wi-Fi name and password while USB is connected, then run provisioning once.
5. Choose button or hands-free input, tune the microphone threshold, and adjust overall speaker gain.
6. Select the PC's normal TTS or GPT-Live input mode. ATOM Echo follows the same Chat/Work, character, voice, and Beatrice settings.

The Wi-Fi password and a random pairing secret are stored in ESP32 NVS. The password is not returned to the PC. Wireless mode is intended for a trusted private LAN; do not forward its ports to the public internet.

For standard TTS, choose a PCM-capable character voice such as Irodori TTS; Windows system speech cannot be forwarded as PCM. GPT-Live requires the Codex app-server connection. CharaDock closes an idle ESP32-device Live session five minutes after the last conversation by default; USB/Wi-Fi remains connected and the next long press reconnects Live automatically. Recording and response playback suspend the timer.

## Controls and LED states

| LED | Meaning |
| --- | --- |
| Green | Connected and ready |
| Blue | Listening |
| Amber | Recognized input is being processed |
| Purple | Playing the character's voice |
| Red | Actionable connection, recognition, or playback error |
| Pulsing / dim | Connecting or waiting for a host |

In push-to-talk mode, hold the ATOM button while speaking and release it to submit. Press during thinking or playback to interrupt. In hands-free mode, audio is monitored locally for voice activity; microphone PCM is sent to the paired PC only after the threshold is crossed. Merely enabling hands-free mode does not start a GPT-Live session or continuously incur Live charges.

## Build from source

Requirements:

- Windows, macOS, or Linux
- Python 3.10 or later
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)

The environment and library versions are pinned independently in each device's `platformio.ini`. The RLCD Arduino-ESP32 3.x package cache is isolated from the M5 targets' Arduino-ESP32 2.x cache, so building StackChan or ATOM cannot replace the RLCD toolchain.

```powershell
pio run --project-dir firmware/atom-echo
pio run --project-dir firmware/atom-echo --target upload --upload-port COM3
pio device monitor --port COM3 --baud 500000

# StackChan K151 / CoreS3 bring-up build
pio run --project-dir firmware/stackchan

# Waveshare ESP32-S3-RLCD-4.2 display / microphone / audio / Wi-Fi build
pio run --project-dir firmware/waveshare-rlcd-4.2
```

To create the same merged binary used for releases:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

StackChan preview images use a separate script so the supported ATOM release is not replaced:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-stackchan-release.ps1 `
  -Version 0.1.0-preview
```

Outputs are written to `dist/` with a SHA-256 manifest. Generated firmware and build folders are intentionally excluded from Git.

RLCD 4.2 uses the same preview-only separation:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\scripts\build-rlcd42-release.ps1 `
  -Version 0.2.0-preview
```

The RLCD profile includes active-route heartbeat detection, a last-character Offline screen, bounded reconnect backoff, complete Snapshot restoration after reconnect, 16 kHz microphone capture, and PC-generated speech playback buffered in 512 KiB of PSRAM. USB is used for first-time Wi-Fi provisioning and remains a fallback after mutual-HMAC pairing. See [`firmware/waveshare-rlcd-4.2/README.md`](./firmware/waveshare-rlcd-4.2/README.md) for flashing, diagnostics, sample scenes, manga portrait transfer, microphone, Wi-Fi, and speaker checks.

## Audio design

Devices send 16 kHz PCM16 mono to the PC. CharaDock performs recognition, synthesis, optional Live/Beatrice processing, protection, and resampling before returning PCM. The ATOM profile keeps its speech-focused small-speaker DSP. RLCD uses a separate near-unity profile with only a light 100 Hz high-pass and final limiter, avoiding ATOM-specific attenuation and compression on the ES8311 path. Firmware disables Wi-Fi power save, buffers playback in PSRAM, uses bounded flow control, and drains accepted audio before disabling I2S.

## Network and protocol

| Purpose | Transport / port |
| --- | --- |
| Device discovery | UDP 41721 |
| Host selection | UDP 41723 |
| Authenticated control + PCM | TCP 41722 |
| USB setup / fallback | Serial 500000 baud |

Wireless sessions use a 256-bit provisioned secret for mutual HMAC-SHA256 authentication: CharaDock verifies the RLCD response, and the RLCD verifies the host proof before accepting control or PCM. Frames include a fixed header, sequence, bounded payload, and CRC-32; speaker chunks use acknowledgements for flow control. See [docs/protocol.md](./docs/protocol.md) for the wire format.

## Troubleshooting

- **The device remains dim or connecting:** keep USB attached, confirm CharaDock is running, and reselect the COM port.
- **Wi-Fi never connects:** provision again after changing access points; both devices must be on a LAN that permits peer traffic.
- **Wi-Fi playback clicks or crackles:** update to v0.5.2 or later. It removes power-save latency, adds more prebuffering, and works with CharaDock's pipelined audio acknowledgements. If it persists, check distance from the access point and 2.4 GHz congestion.
- **Hands-free starts only at close range:** lower the threshold gradually while watching the live RMS/noise-floor display. Avoid setting it below normal room noise.
- **Amber never returns to green:** update both CharaDock and this firmware. Stop any PC/phone Live session that owns the shared Realtime route, then retry.
- **Standard TTS shows red:** select a PCM-capable non-system TTS provider and confirm its model is ready.
- **Audio is quiet:** raise overall gain in CharaDock. Very high gain will expose the physical speaker's distortion and cannot add bass extension.
- **Flashing cannot open the port:** quit CharaDock and close serial monitors before retrying.

## Repository layout

```text
firmware/
  atom-echo/       PlatformIO project for the supported ATOM Echo
  stackchan/       PlatformIO project for StackChan K151 bring-up
  waveshare-rlcd-4.2/
                   PlatformIO project for RLCD 4.2 bring-up
shared/
  protocol-v2/     Device-neutral frame codec shared by new targets
docs/
  protocol.md      Shared CharaDock ESP32 host protocol
scripts/
  build-release.ps1
  build-stackchan-release.ps1
  build-rlcd42-release.ps1
```

New devices should get their own `firmware/<device>` project and reuse the documented host concepts. Device capabilities must be reported explicitly; the PC UI should reveal only controls that device actually supports.

## Security and privacy

No Wi-Fi password, pairing token, API key, user path, or local log is committed to this repository or embedded in release images. Provisioned credentials remain in the device's NVS until erased or replaced. Conversation services and costs are determined by the CharaDock PC settings; this firmware contains no sanoTTS/Open JTalk model, no cloud credentials, and never connects directly to OpenAI.

## License

Source code in this repository is licensed under [Apache License 2.0](./LICENSE). Third-party libraries retain their own licenses and are resolved by PlatformIO during the build; see [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).
