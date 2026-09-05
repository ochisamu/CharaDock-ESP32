# CharaDock ESP32 device protocols

This document describes the implemented ATOM Echo transport, its compatibility boundary, and the device-neutral protocol-v2 foundation shared by StackChan and RLCD 4.2.

## Protocol v1: ATOM Echo / Voice

## Discovery and session setup

1. A provisioned device broadcasts `CHARADOCK_ATOM_DISCOVER_V1 <device-id>` to UDP port 41721.
2. CharaDock replies to UDP port 41723 with `CHARADOCK_ATOM_HOST_V1 <host-address> <audio-port>`.
3. The device opens TCP port 41722 on the selected host and sends `DeviceHello`.
4. CharaDock sends a random `AuthChallenge`.
5. The device returns `DeviceAuth`, an HMAC-SHA256 calculated with the provisioned 256-bit pairing secret.
6. Only the expected device ID and a valid challenge response are accepted.

The discovery strings retain `ATOM` for protocol-v1 compatibility. A future capability handshake may introduce a device-neutral discovery version; do not infer UI capabilities from the discovery string.

## Binary frame

All multibyte integer fields are little-endian.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | ASCII magic `CD` |
| 2 | 1 | Protocol version (`1`) |
| 3 | 1 | Frame type |
| 4 | 2 | Sequence number |
| 6 | 2 | Payload length, maximum 2048 bytes |
| 8 | 4 | CRC-32 over version through payload length, then payload |
| 12 | N | Payload |

Malformed magic, versions, lengths, CRCs, or authentication data must be rejected without interpreting the payload.

## Frame types

| Value | Name | Direction | Purpose |
| ---: | --- | --- | --- |
| `0x01` | DeviceHello | Device → host | Board, firmware, ID, rate, transport |
| `0x02` | HostHello | Host → device | Host acceptance, session metadata, or an empty idle heartbeat |
| `0x03` | AuthChallenge | Host → device | Random authentication challenge |
| `0x04` | DeviceAuth | Device → host | HMAC-SHA256 response |
| `0x10` | PttStart | Device → host | Start one microphone utterance |
| `0x11` | PcmChunk | Both | PCM16 mono samples |
| `0x12` | PttEnd | Device → host | End one microphone utterance |
| `0x13` | Interrupt | Device → host | Stop the active turn or playback |
| `0x20` | State | Host → device | Idle/listening/thinking/speaking/error |
| `0x21` | AudioBegin | Host → device | Begin speaker stream |
| `0x22` | AudioChunk | Host → device | Speaker PCM chunk |
| `0x23` | AudioEnd | Host → device | Drain and finish speaker stream |
| `0x24` | AudioStop | Host → device | Stop speaker immediately |
| `0x30` | WifiConfig | Host → device | USB-only provisioning payload |
| `0x31` | WifiStatus | Device → host | Bounded connection metadata |
| `0x32` | CaptureConfig | Host → device | Button/hands-free mode and VAD threshold |
| `0x33` | CaptureStatus | Device → host | RMS, noise floor, thresholds, VAD state |
| `0x7e` | Ack | Both | Bounded flow-control acknowledgement |
| `0x7f` | Error | Both | Bounded diagnostic message |

During Wi-Fi playback, CharaDock may keep up to six `AudioChunk` frames awaiting acknowledgement. The bounded window hides normal LAN round-trip jitter without allowing an unbounded sender queue. `AudioEnd` is sent only after every outstanding audio acknowledgement has settled.

## Audio contract

- Microphone: signed PCM16 little-endian, mono, 16 kHz.
- Speaker: signed PCM16 little-endian, mono, 16 kHz on the wire.
- The ATOM Echo implementation duplicates mono to both I2S slots locally.
- A microphone utterance is capped at 30 seconds.
- Audio remains half-duplex: a new input turn interrupts speaker playback.
- CharaDock owns resampling, voice DSP, TTS, Realtime, conversation state, and cloud credentials.

## Provisioning and stored data

`WifiConfig` is accepted only from the physical USB transport. The firmware stores SSID, password, pairing token, and derived device identity in NVS. Wi-Fi status can report the SSID but never the password or pairing token.

## Future devices

A device-neutral protocol revision is being introduced by the StackChan target. Protocol v2 retains the authenticated session and audio concepts while adding explicit capability and presentation frames. CharaDock settings must render from these capabilities instead of assuming ATOM Echo hardware. Protocol-v1 values remain stable for existing firmware.

## Protocol v2 foundation

Version 2 uses the same `CD` header shape and CRC-32 algorithm, sets the protocol byte to `2`, and allows a maximum control/asset chunk payload of 4096 bytes. StackChan and RLCD share the codec and frame allocation; each board advertises its actual display, audio, input, sensor, motion, storage, and network support through `Capabilities`.

| Value | Name | Direction | Purpose |
| ---: | --- | --- | --- |
| `0x05` | Capabilities | Device → host | Model and available display, audio, motion, touch, LED, and storage features |
| `0x34` | TimeSync | Host → device | Unix time and UTC offset for a device RTC |
| `0x40` | CharacterChanged | Host → device | Atomic character/profile revision change |
| `0x41` | PresentationConfig | Host → device | Theme, motion profile, artwork policy, and presentation settings |
| `0x42` | Expression | Host → device | Neutral/listening/thinking/happy/surprised/soft expression |
| `0x43` | MouthLevel | Host → device | Closed/half/open mouth level derived from played PCM |
| `0x44` | Motion | Host → device | Bounded nod, tilt, yaw, pitch, or home command |
| `0x50` | AssetMeta | Host → device | Bounded asset dimensions, format, frame name, byte count, and revision |
| `0x51` | AssetChunk | Host → device | One bounded current-character asset chunk |
| `0x52` | AssetEnd | Host → device | Complete and verify the staged asset |
| `0x53` | AssetInvalidate | Host → device | Drop a stale cached revision |
| `0x54` | DisplayMode | Host → device | Hybrid, Character Art, or Native Face |
| `0x55` | DisplayScene | Host → device | Stage one versioned device-neutral scene snapshot |
| `0x56` | DisplayText | Host → device | Stage one bounded UTF-8 scene text field |
| `0x57` | SensorReport | Device → host | RTC, environment, battery, and hardware-presence report |
| `0x58` | InputEvent | Device → host | Bounded physical KEY/BOOT event |
| `0x59` | DisplayCommit | Host → device | Atomically reveal the staged scene revision |

`State`, `Expression`, `MouthLevel`, and `DisplayMode` each use one byte whose value follows the corresponding firmware enum. Out-of-range values and trailing bytes are rejected.

`PresentationConfig` is a 13-byte atomic settings snapshot:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Settings schema version (`1`) |
| 1 | 1 | Flags; bit 0 allows character artwork, all other bits must be zero |
| 2 | 1 | Motion profile: energetic (`0`), calm (`1`), curious (`2`), reserved (`3`), custom (`4`) |
| 3 | 1 | Motion intensity, 0–100 |
| 4 | 3 | Primary color, RGB888 |
| 7 | 3 | Secondary color, RGB888 |
| 10 | 3 | Accent color, RGB888 |

Applying a snapshot updates theme and motion together. Disabling artwork invalidates the active portrait and forces Character Art back to Native Face, preventing a partially updated character presentation.

### Protocol-v2 speaker audio

The first StackChan audio route is available over physical USB serial. `AudioBegin` carries an 11-byte fixed settings snapshot:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Audio settings schema version (`1`) |
| 1 | 4 | Sample rate (`16000`) |
| 5 | 1 | Channels (`1`) |
| 6 | 1 | Bits per sample (`16`) |
| 7 | 1 | M5 speaker volume, 0–255 |
| 8 | 2 | Prebuffer duration, 80–500 ms; default 200 ms |
| 10 | 1 | Flags; bit 0 signed samples, bit 1 little-endian; both required |

`AudioChunk` contains only non-empty, even-length PCM16 little-endian bytes and is limited to the protocol's 4096-byte payload. `AudioEnd` and `AudioStop` have empty payloads. End drains all accepted samples; Stop discards them and interrupts the hardware queue immediately.

The CoreS3 firmware uses a 16,384-sample circular buffer (about 1.024 seconds at 16 kHz), starts after the requested prebuffer, and feeds M5Unified with three rotating 20 ms blocks. A chunk is removed from the ring only after the speaker queue accepts it. If playback catches the producer, it returns to buffering instead of manufacturing silence and increments the underrun diagnostic.

Audio frames are idempotent for stop-and-wait retry. The firmware remembers the eight most recent accepted chunk sequences and fingerprints. Repeating the same sequence and bytes returns an acknowledged duplicate without appending audio twice; reusing a sequence with different bytes is rejected. A full ring returns `buffer-full`, allowing the sender to wait and retry the unchanged frame. This eight-entry history also covers the planned maximum six-frame authenticated LAN window.

### Protocol-v2 portrait payloads

`AssetMeta` uses a compact fixed-prefix payload. All multibyte values are little-endian.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Format (`0` = RGB565 little-endian) |
| 1 | 2 | Width (`320`) |
| 3 | 2 | Height (`240`) |
| 5 | 4 | Total uncompressed byte count (`153600`) |
| 9 | 4 | CRC-32 of the complete raw asset |
| 13 | 1 | Revision length, 1–64 bytes |
| 14 | 1 | Frame-name length, 0–32 bytes |
| 15 | R | Revision identifier |
| 15+R | F | Frame name, initially `portrait` |

Revision identifiers permit ASCII letters, digits, `.`, `_`, `-`, and `:`. Frame names omit `:`. `AssetChunk` begins with a four-byte absolute offset followed by up to 4092 asset bytes. Chunks must be sequential with no gaps or overlap. `AssetEnd` has an empty payload and commits only a complete asset whose CRC matches. `AssetInvalidate` carries either no payload to clear the current portrait or one revision identifier to invalidate only a matching cache entry.

When the current presentation policy does not allow artwork, `AssetMeta` and a Character Art `DisplayMode` request are rejected; Hybrid and Native Face continue without showing or accepting character art.

StackChan reserves two 153,600-byte PSRAM slots. One remains the verified active portrait while the other receives a transfer. Invalid metadata, offset errors, incomplete transfer, or CRC failure cannot replace the active image. This is the Static Portrait baseline; additional mouth/blink frames can reuse the same metadata and frame-name contract later.

During bring-up, protocol-v2 control, portrait, and speaker-audio frames are accepted from the physical USB serial connection. Each applied frame receives `Ack`; invalid or unsupported input receives `Error`. Both responses reuse the request sequence and carry four bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Request frame type |
| 1 | 1 | Apply-result code |
| 2 | 1 | Portrait-cache result code |
| 3 | 1 | Audio-buffer result code |

The PC portrait helper also accepts the original three-byte portrait-only acknowledgement for firmware compatibility. Network use must not expose this unauthenticated USB convenience; LAN transport still requires the mutually HMAC-authenticated session.

Apply-result codes are applied (`0`), portrait completed (`1`), portrait cache hit (`2`), ignored (`3`), invalid payload (`4`), portrait rejected (`5`), and audio rejected (`6`). Portrait-cache result codes are OK (`0`), storage unavailable (`1`), invalid metadata (`2`), no active transfer (`3`), unexpected offset (`4`), too large (`5`), incomplete (`6`), and checksum mismatch (`7`). Audio-buffer result codes are OK (`0`), storage unavailable (`1`), invalid format (`2`), no active session (`3`), invalid chunk (`4`), buffer full (`5`), sequence conflict (`6`), duplicate (`7`), and already ended (`8`). These values are diagnostic; `Ack` versus `Error` remains the primary success signal. Duplicate audio is deliberately returned as `Ack` with apply-result `0` and audio result `7`.

`CharacterChanged` may carry the expected portrait revision. A matching current revision returns an acknowledged cache-hit result and immediately reuses the verified portrait. A missing or different revision asks the host to send fresh `AssetMeta` and chunks, but does not discard the verified active portrait. The inactive slot receives and verifies the replacement first; only `AssetEnd` may switch it into view. `AssetInvalidate` remains the explicit cache-clearing operation.

The first StackChan build reports, but does not overclaim, these capabilities: 320×240 RGB565 display, half-duplex microphone/speaker, yaw/pitch motion, head touch, RGB LEDs, optional microSD, and PSRAM. Its protocol-v2 codec, validated portrait staging, Hybrid/Static Character Art rendering, presentation dispatcher, USB PCM playback, and USB portrait/audio diagnostic senders are implemented. Wi-Fi discovery, HMAC authentication, microphone transport, and CharaDock PC integration remain gated on physical bring-up.

For lip sync, `MouthLevel` is closed (`0`), half (`1`), or open (`2`). The device-side envelope helper is rate-limited to 10 Hz with attack/release hysteresis and consumes PCM blocks only after the speaker path accepts them. This prevents serial receive timing from driving the face far ahead of audible voice. USB PCM currently uses these locally derived levels. A future host transport must choose either this path or host levels derived from the exact post-Beatrice playback stream, never two competing sources.

## RLCD 4.2 display/control profile

The Waveshare ESP32-S3-RLCD-4.2 target is a Protocol-v2 USB/Wi-Fi profile. Its capabilities report a 400×300, 1-bit, landscape display; Shinonome 12/16 px fonts; `raw1-msb` bitmaps; KEY and BOOT-long input; RTC, temperature, humidity, and battery sensors; 16 kHz PCM16 mono capture/playback; half duplex; USB-only provisioning; Wi-Fi; and mutual HMAC-SHA256 authentication. A host must use the capability response rather than infer support from the board name.

The current CharaDock desktop source implements the profile end to end: USB discovery, board/capability verification, time synchronization, manga-oriented 1-bit character conversion, revision-aware portrait caching, atomic scene commits, input and sensor reports, microphone routing into Chat/Work/Live, PCM WAV decoding, mono conversion, 16 kHz resampling, and standard TTS/GPT-Live/Beatrice speaker transfer. The PC remains the only conversation and voice runtime; the firmware contains no local speech-recognition, sanoTTS, Open JTalk, TTS model, or cloud credentials.

### RLCD Wi-Fi discovery and mutual authentication

1. After USB provisioning, the device broadcasts `CHARADOCK_DEVICE_DISCOVER_V2 <device-id> waveshare-esp32-s3-rlcd-4.2` to UDP port 41721 from local port 41723.
2. CharaDock replies to the packet's source with `CHARADOCK_DEVICE_HOST_V2 <host-address> <tcp-port>`; the normal TCP port is 41722.
3. The device opens TCP to that host and sends `DeviceHello` with its stable `cd-rlcd-…` ID.
4. CharaDock checks the expected ID and board, sends a random 32-byte `AuthChallenge`, and verifies `DeviceAuth = HMAC-SHA256(secret, challenge)` using the provisioned 256-bit secret.
5. CharaDock sends the initial `HostHello` payload as `HMAC-SHA256(secret, "CHARADOCK_HOST_V2:" || challenge)`.
6. Until that host proof succeeds, the device accepts no status, control, display, microphone, or speaker request. An unauthenticated TCP destination is abandoned after seven seconds and discovery resumes.
6. The device verifies that host proof before acknowledging the session or accepting control, microphone, display, or speaker traffic.

SSID, password, and pairing secret can be written only by a CRC-verified `WifiConfig` over physical USB. Password and secret are never included in `WifiStatus`. Wi-Fi disables station power saving for predictable PCM timing. An authenticated Wi-Fi route is preferred when available; USB remains the setup and failover route.

### Atomic scene transaction

The host stages one `DisplayScene`, zero or more `DisplayText` frames with the same non-zero revision, then sends `DisplayCommit`. Nothing staged becomes visible until a matching commit succeeds. Older revisions, mismatched commits, malformed UTF-8, unsupported controls, unknown flags, and trailing bytes are rejected.

`DisplayScene` schema 1 uses this prefix followed by UTF-8 character and mode-label bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Schema (`1`) |
| 1 | 1 | Scene: Home (`0`), Conversation (`1`), Work (`2`), Offline (`3`), Recovery (`4`) |
| 2 | 1 | State: idle (`0`), listening (`1`), thinking (`2`), speaking (`3`), error (`4`), connecting (`5`), working (`6`), completed (`7`), approval (`8`), offline (`9`) |
| 3 | 1 | Flags: connected, Live, Beatrice, approval in bits 0–3 |
| 4 | 4 | Non-zero scene revision |
| 8 | 4 | Elapsed seconds |
| 12 | 2 | Artifact count |
| 14 | 1 | Character-name byte length, 1–48 |
| 15 | 1 | Mode-label byte length, 0–24 |
| 16 | N | Character name, then mode label |

`DisplayText` schema 1 has a 10-byte prefix: schema, target (`0` caption, `1` activity, `2` next action, `3` footer), font size (`12` or `16`), reserved zero, 32-bit revision, and 16-bit UTF-8 byte length. Payload limits are 1024, 384, 256, and 160 bytes respectively. Line feed is the only accepted control character. `DisplayCommit` is schema byte `1` plus the same 32-bit revision.

### RLCD monochrome portrait

The RLCD profile reuses `AssetMeta`, `AssetChunk`, `AssetEnd`, `AssetInvalidate`, and `CharacterChanged`. Its `AssetMeta` format value is `1` (`raw1-msb`), followed by little-endian width, height, byte count, CRC-32, revision length, and frame-name length in the same 15-byte prefix used by the StackChan profile. Width and height are bounded at 400×300; byte count must equal `ceil(width / 8) × height` and may not exceed 15000 bytes.

Each `AssetChunk` begins with a 32-bit absolute offset. Chunks must be non-empty and sequential. Two 15000-byte PSRAM slots ensure that a bad offset, incomplete transfer, or failed CRC cannot replace the active image. The new portrait becomes visible only after `AssetEnd` verifies it.

The default host conversion is `manga`: it normalizes contrast, removes one-pixel texture, recovers strong Sobel outlines, and expresses shading with two fixed screen densities. This keeps eyes, hair, and silhouettes legible on the reflective 1-bit panel. The original full-image illustration dither remains an explicit user-selectable alternative. Both outputs are deterministic and include the style in the portrait revision hash.

### Active-route liveness and snapshot recovery

While an RLCD USB or authenticated Wi-Fi session is idle, the host sends an empty `HostHello` every eight seconds and expects the normal four-byte `Ack`. On Wi-Fi, an empty `HostHello` is accepted only after mutual authentication on that TCP stream. The device treats every accepted active-route frame as liveness activity. If no host frame arrives for 24 seconds, it switches locally to Offline while preserving the last verified portrait, character name, RTC, and sensors. A heartbeat failure makes CharaDock reconnect with bounded exponential backoff. Every successful handshake is a new session, so the host resends the complete current portrait and scene Snapshot rather than assuming incremental state survived the interruption.

### Time, sensors, and input

`TimeSync` schema 1 is 11 bytes: schema, unsigned 64-bit Unix seconds, and signed 16-bit UTC offset minutes from −720 through +840. The device writes UTC to PCF85063 and retains the offset for local display.

`SensorReport` schema 1 is 18 bytes. Byte 1 has availability bits for SHTC3, RTC, battery, ES8311, and ES7210. It then carries signed temperature ×100, humidity ×100, battery millivolts, battery percent, and RTC year/month/day/hour/minute/second. The final two bytes are reserved zero. Reports are sent at boot and every 60 seconds. A host may send an empty `SensorReport` request; the device responds immediately with the same sequence, which avoids waiting for the periodic report after opening USB.

`InputEvent` schema 1 is seven bytes: schema, button (`0` KEY, `1` BOOT), event code, and 32-bit duration milliseconds. KEY is debounced at 25 ms; its long press begins at 420 ms. BOOT's three-second long press is reserved for a local diagnostic scene. Physical press feedback renders locally without waiting for a host round trip.

### RLCD microphone capture

`CaptureConfig` is three bytes. Byte 0 is push-to-talk (`0`), hands-free (`1`), or disabled (`2`); bytes 1–2 are the little-endian minimum start threshold, clamped to 80–800 RMS. The selected active transport receives `PttStart`, non-empty even-length `PcmChunk` frames, then `PttEnd`. PCM is signed 16-bit little-endian mono at 16 kHz and one utterance is capped at 30 seconds.

The device configures the onboard ES7210 MIC1/MIC3 pair only while listening, samples both slots, and selects the channel with greater recent energy for the mono stream. In push-to-talk mode, microphone monitoring starts on KEY press, the most recent 12288 bytes (384 ms) remain local, holding KEY for at least 420 ms commits that preroll to the utterance, and release submits it. A short press cancels monitoring without sending microphone PCM. In hands-free mode, the first quiet blocks calibrate a local noise floor; two qualifying chunks begin an utterance, the same bounded local preroll is emitted once, and sustained silence ends it. Monitoring alone neither opens GPT-Live nor sends continuous microphone PCM to the PC.

RLCD portrait animation uses the same verified `raw1-msb` asset contract with the frame names `portrait`, `portrait-blink`, `portrait-mouth-half`, and `portrait-mouth-open`. Frames are transferred before playback and retained in PSRAM. During playback the device selects the mouth frame from the measured PCM peak at no more than 4 fps; local blink timing varies and may occasionally double-blink. Every character also receives a sparse one-pixel idle breath/sway at a randomized 9–18 second interval without relying on character-specific coordinates. A display shadow limits each panel write to the bounding tile rectangle that actually changed. No bitmap traffic competes with PCM streaming: capture, playback, or an audio underrun cancels large-area idle motion, and the neutral portrait remains the fallback.

When an ESP32 device owns GPT-Live, CharaDock arms a five-minute idle timer after connection and after each completed input/output turn. Microphone capture and response playback suspend it. Expiry closes only the GPT-Live/WebRTC session; Device Protocol v2 over USB or authenticated Wi-Fi, the last portrait, sensors, and local controls remain active. The next long KEY press opens a fresh Live session automatically.

`CaptureStatus` is 12 bytes: flags (`monitoring` and `recording`), current RMS, noise floor, effective start threshold, effective continue threshold, qualifying-speech chunk count, silence chunk count, and remaining calibration chunks. CharaDock uses this report for the live meter and threshold guidance. Capture and playback share I2S0, so either direction stops the other before reconfiguration; echo cancellation and full duplex are intentionally not claimed.

### RLCD speaker audio

RLCD reuses the Protocol-v2 `AudioBegin`, `AudioChunk`, `AudioEnd`, and `AudioStop` frame types, but its fixed playback contract has a compact eight-byte `AudioBegin` payload:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Sample rate; currently `16000` |
| 4 | 4 | Total mono samples, or `0xffffffff` for a rolling stream |

`AudioChunk` contains non-empty, even-length PCM16 little-endian mono bytes and is limited to 4096 bytes. `AudioEnd` and `AudioStop` have empty payloads. A known utterance may contain at most 262,144 samples (512 KiB); CharaDock advertises a known count only when it fits that limit.

The device stores a known utterance completely in a 512 KiB PSRAM ring and starts playback only after `AudioEnd`, preventing TTS transfer timing from producing gaps. An unknown-length stream starts after an 8192-byte (256 ms) prebuffer and continues as a rolling buffer. The USB CDC receive queue is larger than two maximum Protocol-v2 frames so a 4096-byte audio chunk is not lost while the reflective LCD refreshes.

Speaker setup is fail-closed: GPIO 46 (PA_EN) is driven low before initialization, I2S MCLK starts before ES8311 setup, mute and volume registers are read back, and a zero DMA block is queued before PA_EN may rise. PCM is duplicated into both I2S slots. Completion drains accepted samples; stop, timeout, codec failure, or shutdown forces PA_EN low. CharaDock uses a dedicated RLCD output profile rather than ATOM Echo's tiny-speaker attenuation and compression.
