# Now playing footer (RLCD 4.2)

The host may send a three-line Protocol-v2 footer: playback state, track title,
artist. On a connected Home scene the renderer displays a quiet 76px bottom
panel. The title uses the bundled 16px font and wraps to two lines; the state
and artist use 12px. Sensor information stays on the state row. No scrolling,
extra animation, speech, or media control originates from the display.

Normal single-line footers keep their existing layout. Conversation and
Recovery hide this footer; the host sends normal status during active work.
Disconnect handling already replaces the footer with offline text, removing
stale media. Hosts must normalize embedded newlines and bound the entire footer
to 160 UTF-8 bytes. Recommended title/artist bounds: 96/40 bytes.

No device has been flashed as part of this change. Build only:

```
pio run --project-dir firmware/waveshare-rlcd-4.2
```

Host renderer regression (Linux, from repository root):

```
g++ -std=c++17 -Ifirmware/waveshare-rlcd-4.2/test/render_stubs \
  -Ifirmware/waveshare-rlcd-4.2/include -Ishared/protocol-v2/include \
  firmware/waveshare-rlcd-4.2/test/now_playing_render_test.cpp \
  firmware/waveshare-rlcd-4.2/src/{scene_renderer,scene_model,shinonome_font,monochrome_asset,utf8}.cpp \
  shared/protocol-v2/src/protocol_v2.cpp -o /tmp/rlcd-now-playing-render-test
/tmp/rlcd-now-playing-render-test firmware/waveshare-rlcd-4.2 \
  /path/to/400x300-raw1-msb-portrait.raw1 /tmp/rlcd-now-playing
```

The test writes four PGM previews and asserts second-line text rendering,
artist rendering, no residual pixels after clearing, and no music footer in
conversation. It uses production rendering/font/asset code; only U8g2 and
Arduino hardware calls are replaced by a host framebuffer. It does not
simulate panel refresh, sensors, or the ambient clock font.
