# StackChan conversation integration (hardware validation pending)

Scope: official M5Stack StackChan K151/CoreS3, not arbitrary M5Stick-based
StackChan kits. This change was built and tested without connecting or flashing
hardware. Existing RLCD and ATOM firmware is not replaced.

## Implemented path

PC CharaDock → USB or mutually authenticated Wi-Fi → StackChan speaker.
Head touch → ESP32 PCM16 microphone → PC recognition/conversation/TTS.
The PC retains its normal Chat/Work and PTT Live pipelines, voice settings,
speech gating and optional five-minute Live idle timeout. No on-device TTS,
model, dictionary, or cloud credential is stored by this firmware.

- USB provisioned SSID/password/256-bit token in NVS; UDP 41721, TCP 41722.
- Board identity `m5stack-stackchan-k151`, individual ID `stackchan-<12 hex>`.
- HMAC-SHA256 challenge and domain-separated host proof, seven-second auth
  deadline. Diagnostic queries remain available on authenticated standby Wi-Fi.
- Connected USB host has priority; standby configuration cannot steal capture.
  CDC disconnection or a 24-second heartbeat lease expiry releases USB.
- Static 20 ms mic buffer remains alive until asynchronous capture completes;
  capture is limited to 30 seconds, and disabled during playback.
- Eleven-byte StackChan AudioBegin remains compatible with existing diagnostic
  tools; PC retries only explicit ring-full rejections, not ambiguous ACK loss.
- 320×240 RGB565 assets use atomic staging and CRC. PC uses original colour
  layers, not RLCD manga derivatives. Hybrid shows the portrait briefly and
  returns to the animated native face; mouth animation belongs to that face.
- Japanese name/caption, blinking, subtle two-axis gaze and state RGB LEDs.
- Session-only servo opt-in: head tilt when thinking, gentle nod when speaking.
  Startup/disconnect/transport handover cuts servo power; never auto-enable it.

## Protocol additions

| Type | Payload |
| --- | --- |
| CaptureConfig 0x32 | 3 bytes: mode (0 PTT, 2 disabled), threshold u16 reserved. Mode 1 is rejected. |
| Motion 0x44 | Exactly one byte, 0 disable or 1 enable. Current host only. |
| Caption 0x60 | UTF-8 JSON `{ "name": "…", "caption": "…" }`, at most 1024 bytes. PC caps name at 32 and caption at 180 UTF-16 units. |

Caption rendering is clipped to the bottom area; long captions can exceed the
visible area. This is not the RLCD multi-page clock/caption compositor.

## Build and tests (no upload)

```sh
pio run --project-dir firmware/stackchan
g++ -std=c++17 -Ifirmware/stackchan/include \
  firmware/stackchan/src/{protocol_v2,presentation,portrait_cache,mouth_envelope,audio_playback,frame_dispatcher}.cpp \
  firmware/stackchan/test/protocol_test.cpp -o /tmp/stackchan-test
/tmp/stackchan-test
python3 firmware/stackchan/test/portrait_tool_test.py
python3 firmware/stackchan/test/audio_tool_test.py
```

The CharaDock PC repository adds StackChan serial/Wi-Fi tests with simulated
devices, RGB565 validation and buffer-full/cancellation tests.

## First physical validation (separate authorized session)

1. Verify K151 marking, factory recovery path, power and stable placement.
2. After an explicitly authorized flash, leave servos OFF. Confirm native face,
   touch input, mic sample levels, speaker output, and thermal behaviour.
3. PC Settings → ESP32 devices: turn the display device OFF, select StackChan,
   choose its USB port and enable it. A separate pairing/profile is retained
   for RLCD; only one of these display satellites is active at a time.
4. Test a short PTT Chat reply on USB, then a long reply and head-touch cancel.
5. Provision Wi-Fi over USB; test Wi-Fi-only, unplug/replug USB, host restart,
   unplug during recording/playback, and rejection of the wrong pairing key.
6. Test Work and PTT Live, speech recognition failures, next-turn recovery,
   microphone/speaker disable and Live idle expiry.
7. Only after confirming clearance/neutral position, explicitly enable motion;
   verify tilt/nod and power-off on loss of host or transport change.

Not claimed: hardware success, simultaneous microphone/speaker (AEC/full
duplex), hands-free VAD, RLCD temperature/humidity/clock screens, simultaneous
RLCD+StackChan control, or animation of the uploaded colour portrait itself.
