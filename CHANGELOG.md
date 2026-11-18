# Changelog

## 0.9.0 - 2026-11-18

- Add StackChan K151 camera-assisted hands-free mode with on-device face detection and a display indicator for searching and detected states.
- Add head movement by voice for directions, a nod, a bow, a side-to-side sweep and a tilt. Preserve the configured safe movement range and motion consent.
- Improve speech playback with staged phrases, bounded buffering, speaker-start confirmation, and recovery on interrupted USB or Wi-Fi sessions.
- Add adjustable idle posture, optional face tracking, screen dimming, and quick privacy controls. Turn off the voice LED after recording or playback.
- Build separate images for StackChan K151, Waveshare ESP32-S3-RLCD-4.2, and M5Stack ATOM Echo. Use CharaDock v0.9.0 for the new StackChan features.

## 0.6.0 - 2026-09-06

- Add Waveshare ESP32-S3-RLCD-4.2 Protocol-v2 firmware: Japanese text, monochrome character frames, blink/mouth animation, clock/environment dashboard, microphone and speaker.
- Prefer an active USB host over authenticated Wi-Fi; stop in-flight audio on transport changes. Keep standby diagnostics without applying standby microphone configuration.
- Bound USB transmit waits and avoid restarting microphone calibration on heartbeats.
- Keep the clock visible for hands-free noise candidates until PC speech admission.
- Include the independent StackChan K151 target and shared Protocol-v2 source. StackChan remains experimental; RLCD validation does not imply StackChan hardware validation.
- Retain ATOM Echo/Voice Protocol-v1 support and PC-generated speech. No onboard TTS model or dictionary is bundled.

Use CharaDock v0.6.0 with the matching device firmware. Combined factory images
erase settings in their written range; RLCD users should prefer the source
repository's discrete-segment update procedure to preserve NVS Wi-Fi/pairing.
