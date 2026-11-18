# StackChan conversation integration

This guide covers the official M5Stack StackChan K151 with CoreS3 and its
CharaDock desktop integration.

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
  An explicit switch to Wi-Fi releases the USB lease before the port is
  closed. CDC disconnection or a 24-second heartbeat expiry also releases USB.
- Two static 20 ms microphone buffers remain alive during I2S capture. Manual
  turns have a 29.5-second firmware limit and automatic turns an 18-second limit.
  The microphone pauses during playback.
- GPT-Live uses streaming playback and normal TTS preloads complete segments.
  Compatible Wi-Fi connections use independently decodable IMA ADPCM chunks
  and bounded batches that fit in the advertised device buffer. USB playback
  keeps PCM16. Complete TTS segments may stage one following phrase in a
  separate buffer while the speaker plays. A rejected upload can fall back
  to normal playback, and an interruption clears both buffers.
- The animated native face handles normal conversations. The optional
  diagnostic portrait path validates and stages 320×240 RGB565 assets by CRC.
- While recording, the screen holds a steady gaze, responds to sustained voice,
  and shows one small nod at a pause. This feedback stays on the screen during
  mic capture so the servo cannot contribute noise to that recording.
- The speaker queue drives mouth aperture and brief accents. Reply cues select
  a nod, thoughtful tilt, celebration or reassurance. They share the same pose
  across the screen and the optional servos, and can supersede an automatic
  accent. Touch cancels the current gesture. Startup and transport handover cut
  servo power. The desktop restores explicit motion consent on the new link.

### Camera, capture and native expressions

Normal conversation uses the animated native face. The desktop no longer
uploads illustrations to StackChan automatically. The diagnostic portrait
commands remain available.

Hands-free is opt-in. While idle, StackChan detects sustained input and keeps
240 ms of pre-roll. The device reopens the microphone only after the speaker
has released the shared I2S peripheral. Compatible GPT-Live sessions use
desktop Silero admission and request device capture completion about 900 ms
after the end of speech.
If Silero detects no voice within 1.5 seconds, the candidate closes without
starting a Live turn. An updated ambient estimate allows the microphone to
rearm in steady background noise. Touch-and-hold remains available.

In the separate camera-assisted mode, two successive face detections on the
ESP32-S3 are required along with a locally voiced candidate before audio can
leave the device. While idle, the display shows `顔を探し中` and switches to a
green `顔を検知` indicator when a face is found. Face evidence expires after
one second. The device wakes the face when a nearby person first appears and eases the
screen gaze toward the largest nearby face. A face alone does not start audio
capture. The device does not identify people, save images, or send frames to CharaDock. Head touch still
works as manual input. The same half-duplex playback pause applies.

The optional Jev expression director can supply a native face and a semantic
gesture for TTS and Live. Cues belong to the current turn and apply when the
speaker reaches voiced audio. Later TTS clauses can carry another cue at their
playback onset. Later Live cues wait for a pause and expire when the audio
never reaches a suitable boundary. Rule-based replies also provide simple cues.

The local GC0308 preview is explicitly requested from CharaDock while idle,
shows on the StackChan display for up to 15 seconds, and closes early on head
touch or conversation. Camera frames stay on the device. See the
[M5Stack camera example](https://docs.m5stack.com/en/arduino/stackchan/camera).

### Display idle behavior

The backlight starts dimming after 30 seconds of inactivity and turns off after
five minutes. The screen wakes on the initial head touch or a conversation
event, including audio buffering. A touch still goes through the usual
tap/hold behavior. USB, Wi-Fi, touch and audio stay active. Repeated scene
captions and heartbeat traffic do not count as activity.

[Apple Watch Always On](https://support.apple.com/en-la/105074) informed the
dimmed face and immediate wake. [StackChan MCP's display control](https://github.com/kisaragi-mochi/stackchan-mcp)
uses a five-minute idle-off default. The [M5Stack Codex Companion](https://github.com/SimonBear03/m5stack-codex-companion)
keeps the connection reachable during screen sleep.

### Related implementations

- [Reachy Mini conversation app](https://github.com/pollen-robotics/reachy_mini_conversation_app) and its [SDK](https://github.com/pollen-robotics/reachy_mini) put audio-driven accents around expressive movements. StackChan uses only the PCM block already at its own speaker and gives reply cues priority.
- [Furhat reactive gestures](https://docs.furhat.io/skill-development/reference/gestures.html) show acknowledgement at user speech onset and eyebrow motion on speech prominence. [Intent-aware backchanneling](https://doi.org/10.1145/3820253.3831400) also uses a local feedback branch during the user's turn. StackChan uses a restrained face reaction and one nod at a short pause without delaying capture or the reply.
- [M5Stack official firmware](https://github.com/m5stack/StackChan) and [AIAvatarStackChan](https://github.com/uezo/AIAvatarStackChan) expose face, motion and audio separately. CharaDock uses one gesture target for the face and optionally the servo so the same response is visible with the servo off.
- [Xiaozhi's audio pipeline](https://github.com/78/xiaozhi-esp32/blob/main/docs/notify.md) uses bounded compressed transport and separate PCM playback. CharaDock already uses bounded IMA ADPCM Wi-Fi transport and schedules expression from native PCM. [Rhubarb Lip Sync](https://github.com/DanielSWolf/rhubarb-lip-sync) illustrates phoneme-based lipsync for prerecorded files. This device's live and TTS mouth opening share their local sound-envelope implementation.

## Protocol additions

| Type | Payload |
| --- | --- |
| CaptureConfig 0x32 | Mode byte (0 touch, 1 hands-free, 2 disabled, 3 camera-assisted) and little-endian RMS threshold. |
| Motion 0x44 | Exactly one byte, 0 disable or 1 enable. Current host only. |
| Gesture 0x45 | One conversation gesture byte. The current host owns motion. |
| Caption 0x60 | UTF-8 JSON `{ "name": "…", "caption": "…" }`, at most 1024 bytes. PC caps name at 32 and caption at 180 UTF-16 units. |
| Playback diagnostics 0x65 | Empty request. Response reports buffer underruns, speaker gaps and maximum service interval in milliseconds for the latest playback. |
| Voice motion 0x66 | One byte selecting right, left, center, side-to-side sweep, or nod. |
| Motion diagnostics 0x68 | Empty request. Response contains motion enabled, last command, signed little-endian yaw and pitch in tenths of a degree, rendered poses, and speech accent count. |
| Capture control 0x69 | An empty payload cancels capture. A one-byte payload of `01` stops capture and submits the recorded turn. Repeat requests with the same sequence acknowledge the original action. |
| USB release 0x6A | Empty request on the USB owner. A Wi-Fi-configured device acknowledges and drops the USB lease so Wi-Fi can take over while the cable supplies power. |

On compatible firmware, Live playback preparation sets the `AudioStop`
payload to `01` so a voice-command head movement can finish. An ordinary stop
uses an empty payload and cancels both playback and the current gesture.

The native face shows the current caption in the bottom area. Long captions
can exceed the visible area.

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

## Release verification on K151

Use the same firmware and CharaDock revision that will ship. Record the active
transport, firmware version, speech mode and result for each run.

1. Exercise manual, hands-free and camera-assisted capture. Confirm that steady
   noise closes within the local admission window, that speech immediately after
   the rejection starts a fresh turn, and that stop from the head touch cancels
   both pending speech and the current motor gesture. During an active reply,
   the head pad must register a firm touch for 600 ms. Check the reported touch
   strength when investigating unexpected stops.
2. Run `--verify-stackchan-live=<speech.wav>` against each transport. Include a
   long reply and natural pauses. Check playback completion and read diagnostic
   frame `0x65` for zero buffer underruns and speaker gaps.
   Run at least three turns in one Live session. After each reply, confirm
   fresh microphone-monitoring telemetry and ask a follow-up that depends
   on a fact mentioned earlier in that session.
3. Run `--verify-rlcd42-audio --verify-stackchan-tts-staging` with a normal TTS
   profile. On Wi-Fi also run `--verify-rlcd42-audio --verify-stackchan-tts-recovery`
   to exercise an intentionally lost acknowledgement on the second phrase.
4. Check Live phrases for turning right and looking side to side. Use
   diagnostic frame `0x68` to corroborate visible movement. During a normal
   reply, confirm that the mouth follows playback and that small head gestures
   stay timed to voiced audio. Record valid pitch samples during playback,
   report motion-read timeouts separately and compare the pitch range with the
   stationary baseline.
5. Switch USB → Wi-Fi → USB, with an answer active on at least one transition.
   Confirm that the next turn works and the saved camera, home pitch and motion
   choices apply on the new owner. Verify the hardware privacy shortcut and
   camera-assisted start against a real face.
6. Listen at the normal working distance and compare to the PC over the same
   conversation and TTS voice. Include low-level noise, clipped peaks, and
   whether every phrase is intelligible. Verify display dimming and touch wake
   on the device.

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
