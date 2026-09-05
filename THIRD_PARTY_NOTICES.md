# Third-party notices

CharaDock ESP32 firmware depends on separately distributed third-party libraries. Their source and license terms remain with their respective projects.

## StackChan target

| Dependency | Pinned version | License | Purpose |
| --- | --- | --- | --- |
| M5Stack StackChan-BSP | commit `8d4d6fc3b7a6be379c6317c45a02a30bff8c492e` | MIT | K151 body, touch, servos, RGB, battery |
| M5Unified | 0.2.21 | MIT | CoreS3 display, audio, touch, power |
| M5GFX | 0.2.28 | MIT | Display rendering |
| ArduinoJson | 7.4.3 | MIT | Bounded capability/configuration JSON |

StackChan-BSP declares optional IRremoteESP8266 and M5Unit-NFC dependencies. CharaDock's MVP does not use or link those features; they are explicitly ignored in `platformio.ini`.

The StackChan target does not vendor third-party source or character artwork. PlatformIO resolves its linked dependencies during a source build.

## Waveshare ESP32-S3-RLCD-4.2 target

| Dependency | Pinned version | License | Purpose |
| --- | --- | --- | --- |
| Waveshare ESP32-S3-RLCD-4.2 example | commit `eb1f63427d735a22b9c30e22fa63ebddae1834d3` | Apache-2.0 | ST7305 initialization and U8g2 display callback reference |
| U8g2 | 2.36.18 | BSD-2-Clause | 1-bit framebuffer and graphics primitives; no bundled U8g2 font is used |
| pioarduino platform-espressif32 | 55.03.311 | Apache-2.0 | Reproducible PlatformIO platform definition |
| Arduino-ESP32 | 3.3.11 | LGPL-2.1-or-later | ESP32-S3 Arduino framework |
| Shinonome Gothic fonts | 0.9.11 | Public Domain | Embedded 12/16 px JIS X 0201/0208 Japanese glyphs |
| Espressif esp_codec_dev | 1.5.4 | Apache-2.0 | ES8311 output and ES7210 microphone register/clock-sequencing reference |

The four original Shinonome BDF files, upstream authorship, source checksum, and license statement are retained under `third_party/shinonome/`. `scripts/build-shinonome-font.py` verifies those source hashes and deterministically converts them to the bounded Unicode-indexed binary files embedded by the RLCD firmware.

The ST7305 source file carries a prominent adaptation notice and upstream commit. PlatformIO resolves U8g2, pioarduino, Arduino-ESP32, ESP-IDF libraries, and toolchain packages during a source build; only the generated application image is distributed.

`audio_output.cpp` and `audio_input.cpp` adapt the ES8311/ES7210 reset,
sample-clock, mute, gain, power, and suspend register sequencing from
Espressif's Apache-2.0 `esp_codec_dev` 1.5.4 driver. The dependency itself is
not linked or vendored; the board-specific implementation uses Arduino Wire
and ESP-IDF I2S directly. Source:
https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.5.4
