# CharaDock ESP32 protocol v1

This document describes the implemented ATOM Echo transport and the compatibility boundary future CharaDock ESP32 devices can reuse.

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
| `0x02` | HostHello | Host → device | Host acceptance and session metadata |
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

A future device-neutral protocol revision should add an explicit capability object such as microphone, speaker, display, touch, buttons, battery, and animation support. CharaDock settings must render from those capabilities instead of assuming ATOM Echo hardware. Protocol-v1 values must remain stable for existing firmware.
