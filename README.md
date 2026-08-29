# CharaDock ESP32

Voice and character-device firmware for [CharaDock](https://github.com/ochisamu/CharaDock). The PC remains the conversation runtime; an ESP32 device provides the microphone, speaker, controls, and physical feedback.

[日本語](./README.ja.md)

> This repository is expanding beyond one board. Device-specific code lives under `firmware/<device>` so StackChan, RLCD 4.2, and other ESP32 character devices can share the same host protocol without being presented as ATOM Echo settings in CharaDock.

## Device status

| Device | Input | Output / display | Status |
| --- | --- | --- | --- |
| M5Stack ATOM Voice (formerly ATOM Echo, C008-C) | Button or hands-free VAD | Built-in speaker + RGB LED | Supported |
| StackChan | Planned | Planned | Hardware evaluation pending |
| M5Stack RLCD 4.2 | Planned | Planned | Hardware evaluation pending |

The current release targets the original **M5Stack ATOM Voice (formerly ATOM Echo, product code C008-C) with ESP32-PICO-D4**. It is tested with CharaDock v0.5.1.

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

For standard TTS, choose a PCM-capable character voice such as Irodori TTS; Windows system speech cannot be forwarded as PCM. GPT-Live requires the Codex app-server connection. CharaDock can optionally close only the ATOM Echo Live session five minutes after the last conversation; this option is off by default.

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

The environment and library versions are pinned in [`firmware/atom-echo/platformio.ini`](./firmware/atom-echo/platformio.ini).

```powershell
pio run --project-dir firmware/atom-echo
pio run --project-dir firmware/atom-echo --target upload --upload-port COM3
pio device monitor --port COM3 --baud 500000
```

To create the same merged binary used for releases:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

Outputs are written to `dist/` with a SHA-256 manifest. Generated firmware and build folders are intentionally excluded from Git.

## Audio design

The device sends 16 kHz PCM16 mono. CharaDock applies a speech-focused high-pass filter, peak protection, gain, and resampling on the PC. Firmware disables periodic Wi-Fi power-save latency, prebuffers about 200 ms for wireless playback, duplicates each mono sample into both I2S slots, uses a larger DMA queue, and drains the queue before stopping I2S. This avoids network underrun clicks and the original ESP32 mono-I2S level/slot issue without pretending the 0.5–0.8 W micro-speaker can reproduce bass.

## Network and protocol

| Purpose | Transport / port |
| --- | --- |
| Device discovery | UDP 41721 |
| Host selection | UDP 41723 |
| Authenticated control + PCM | TCP 41722 |
| USB setup / fallback | Serial 500000 baud |

Wireless sessions authenticate the provisioned device ID using an HMAC-SHA256 challenge. Frames include a fixed header, sequence, bounded payload, and CRC-32; speaker chunks use acknowledgements for flow control. See [docs/protocol.md](./docs/protocol.md) for the wire format.

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
docs/
  protocol.md      Shared CharaDock ESP32 host protocol
scripts/
  build-release.ps1
```

New devices should get their own `firmware/<device>` project and reuse the documented host concepts. Device capabilities must be reported explicitly; the PC UI should reveal only controls that device actually supports.

## Security and privacy

No Wi-Fi password, pairing token, API key, user path, or local log is committed to this repository or embedded in release images. Provisioned credentials remain in the device's NVS until erased or replaced. Conversation services and costs are determined by the CharaDock PC settings; this firmware does not contain cloud credentials and never connects directly to OpenAI.

## License

Source code in this repository is licensed under [Apache License 2.0](./LICENSE). Third-party libraries retain their own licenses and are resolved by PlatformIO during the build.
