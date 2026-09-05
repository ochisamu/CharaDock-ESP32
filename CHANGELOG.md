# Changelog

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
