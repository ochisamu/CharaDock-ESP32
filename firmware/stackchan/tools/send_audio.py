#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stream a strict PCM WAV file to StackChan over protocol-v2 USB serial."""

from __future__ import annotations

import argparse
import math
import struct
import sys
import time
import wave
from dataclasses import dataclass
from pathlib import Path

from protocol_serial import (
    FRAME_ERROR,
    exchange_frame,
    require_success,
)

FRAME_AUDIO_BEGIN = 0x21
FRAME_AUDIO_CHUNK = 0x22
FRAME_AUDIO_END = 0x23
FRAME_AUDIO_STOP = 0x24

SAMPLE_RATE = 16_000
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2
BYTES_PER_SECOND = SAMPLE_RATE * CHANNELS * SAMPLE_WIDTH_BYTES
AUDIO_RESULT_BUFFER_FULL = 5


@dataclass(frozen=True)
class AudioClip:
    pcm: bytes
    frame_count: int

    @property
    def duration_seconds(self) -> float:
        return self.frame_count / SAMPLE_RATE


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Stream a 16 kHz, mono, signed PCM16 little-endian WAV file to "
            "StackChan over USB serial."
        )
    )
    parser.add_argument("wav", nargs="?", type=Path, help="PCM WAV file to play")
    parser.add_argument("--port", required=True, help="Serial port, for example COM3")
    parser.add_argument("--baud", type=int, default=500_000)
    parser.add_argument(
        "--test-tone",
        action="store_true",
        help="Play a built-in, speech-band diagnostic chime instead of a WAV",
    )
    parser.add_argument(
        "--volume", type=int, default=72, help="M5 speaker volume, 0-255 (default: 72)"
    )
    parser.add_argument(
        "--prebuffer-ms",
        type=int,
        default=200,
        help="Audio buffered before playback, 80-500 ms (default: 200)",
    )
    parser.add_argument(
        "--chunk-ms",
        type=int,
        default=40,
        help="PCM payload duration, 20-120 ms (default: 40)",
    )
    parser.add_argument(
        "--fade-ms",
        type=int,
        default=5,
        help="Click-reducing fade at both file edges, 0-20 ms (default: 5)",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=3,
        help="Retries after a missing acknowledgement (default: 3)",
    )
    parser.add_argument(
        "--buffer-wait-timeout",
        type=float,
        default=5.0,
        help="Seconds to wait when the device ring buffer is full (default: 5)",
    )
    parser.add_argument(
        "--startup-wait",
        type=float,
        default=1.5,
        help="Seconds to wait after opening the port (default: 1.5)",
    )
    return parser.parse_args()


def validate_options(args: argparse.Namespace) -> None:
    if (args.wav is None) == (not args.test_tone):
        raise ValueError("provide exactly one WAV path or --test-tone")
    if not 0 <= args.volume <= 255:
        raise ValueError("volume must be between 0 and 255")
    if not 80 <= args.prebuffer_ms <= 500:
        raise ValueError("prebuffer-ms must be between 80 and 500")
    if not 20 <= args.chunk_ms <= 120:
        raise ValueError("chunk-ms must be between 20 and 120")
    if not 0 <= args.fade_ms <= 20:
        raise ValueError("fade-ms must be between 0 and 20")
    if not 0 <= args.retries <= 20:
        raise ValueError("retries must be between 0 and 20")
    if not 0.0 <= args.buffer_wait_timeout <= 60.0:
        raise ValueError("buffer-wait-timeout must be between 0 and 60 seconds")
    if not 0.0 <= args.startup_wait <= 30.0:
        raise ValueError("startup-wait must be between 0 and 30 seconds")
    chunk_bytes = args.chunk_ms * BYTES_PER_SECOND // 1000
    if chunk_bytes <= 0 or chunk_bytes > 4096 or chunk_bytes % 2:
        raise ValueError("chunk-ms produces an invalid protocol payload size")


def load_pcm_wave(path: Path) -> AudioClip:
    try:
        with wave.open(str(path), "rb") as source:
            format_description = (
                source.getframerate(),
                source.getnchannels(),
                source.getsampwidth(),
                source.getcomptype(),
            )
            required = (SAMPLE_RATE, CHANNELS, SAMPLE_WIDTH_BYTES, "NONE")
            if format_description != required:
                raise ValueError(
                    "WAV must be uncompressed 16 kHz mono PCM16; resample it "
                    "on the PC before sending"
                )
            frame_count = source.getnframes()
            pcm = source.readframes(frame_count)
    except (EOFError, wave.Error) as error:
        raise ValueError(f"invalid WAV file: {error}") from error
    if frame_count == 0:
        raise ValueError("WAV contains no audio samples")
    if len(pcm) != frame_count * SAMPLE_WIDTH_BYTES:
        raise ValueError("WAV ended before all declared samples were read")
    return AudioClip(pcm=pcm, frame_count=frame_count)


def diagnostic_tone_clip() -> AudioClip:
    samples: list[int] = []
    for frequency, duration_ms in (
        (0, 120),
        (700, 240),
        (0, 100),
        (1000, 240),
        (0, 100),
        (1400, 240),
        (0, 120),
    ):
        count = duration_ms * SAMPLE_RATE // 1000
        if frequency == 0:
            samples.extend([0] * count)
            continue
        ramp = min(80, count // 2)
        for index in range(count):
            edge_gain = min(1.0, index / ramp, (count - index - 1) / ramp)
            sample = round(
                5000
                * edge_gain
                * math.sin(2.0 * math.pi * frequency * index / SAMPLE_RATE)
            )
            samples.append(sample)
    pcm = struct.pack(f"<{len(samples)}h", *samples)
    return AudioClip(pcm=pcm, frame_count=len(samples))


def apply_edge_fades(pcm: bytes, fade_ms: int) -> bytes:
    if fade_ms <= 0 or not pcm:
        return pcm
    output = bytearray(pcm)
    sample_count = len(output) // SAMPLE_WIDTH_BYTES
    fade_samples = min(fade_ms * SAMPLE_RATE // 1000, sample_count // 2)
    if fade_samples == 0:
        return bytes(output)
    for index in range(fade_samples):
        fade_in_offset = index * 2
        fade_out_offset = (sample_count - fade_samples + index) * 2
        fade_in = index / fade_samples
        fade_out = (fade_samples - index - 1) / fade_samples
        first = struct.unpack_from("<h", output, fade_in_offset)[0]
        last = struct.unpack_from("<h", output, fade_out_offset)[0]
        struct.pack_into("<h", output, fade_in_offset, round(first * fade_in))
        struct.pack_into("<h", output, fade_out_offset, round(last * fade_out))
    return bytes(output)


def audio_config_payload(volume: int, prebuffer_ms: int) -> bytes:
    flags = 0x01 | 0x02  # signed samples and little-endian
    return struct.pack(
        "<BIBBBHB", 1, SAMPLE_RATE, CHANNELS, 16, volume, prebuffer_ms, flags
    )


def next_sequence(sequence: int) -> int:
    return 1 if sequence == 0xFFFF else sequence + 1


def send_audio_frame(
    serial_port,
    frame_type: int,
    sequence: int,
    payload: bytes = b"",
    *,
    retries: int = 3,
    buffer_wait_timeout: float = 5.0,
    retry_delay: float = 0.02,
    response_timeout: float = 3.0,
) -> None:
    missing_acknowledgements = 0
    buffer_deadline = time.monotonic() + buffer_wait_timeout
    while True:
        try:
            acknowledgement = exchange_frame(
                serial_port,
                frame_type,
                sequence,
                payload,
                timeout_seconds=response_timeout,
            )
        except TimeoutError:
            if missing_acknowledgements >= retries:
                raise
            missing_acknowledgements += 1
            time.sleep(retry_delay)
            continue
        if acknowledgement.accepted:
            return
        if (
            frame_type == FRAME_AUDIO_CHUNK
            and acknowledgement.response_type == FRAME_ERROR
            and acknowledgement.audio_result == AUDIO_RESULT_BUFFER_FULL
            and time.monotonic() < buffer_deadline
        ):
            time.sleep(retry_delay)
            continue
        require_success(acknowledgement)


def best_effort_stop(serial_port, sequence: int) -> None:
    try:
        send_audio_frame(
            serial_port,
            FRAME_AUDIO_STOP,
            sequence,
            retries=0,
            buffer_wait_timeout=0,
            response_timeout=0.5,
        )
    except (OSError, RuntimeError, TimeoutError):
        pass


def play(args: argparse.Namespace) -> None:
    validate_options(args)
    clip = diagnostic_tone_clip() if args.test_tone else load_pcm_wave(args.wav)
    pcm = apply_edge_fades(clip.pcm, args.fade_ms)
    try:
        import serial
    except ImportError as error:
        raise RuntimeError("pyserial is required: py -m pip install pyserial") from error

    with serial.Serial(
        args.port, args.baud, timeout=0.1, write_timeout=3.0
    ) as port:
        time.sleep(args.startup_wait)
        port.reset_input_buffer()
        sequence = 1
        stream_started = False
        try:
            send_audio_frame(
                port,
                FRAME_AUDIO_BEGIN,
                sequence,
                audio_config_payload(args.volume, args.prebuffer_ms),
                retries=args.retries,
                buffer_wait_timeout=args.buffer_wait_timeout,
            )
            stream_started = True
            sequence = next_sequence(sequence)
            chunk_bytes = args.chunk_ms * BYTES_PER_SECOND // 1000
            queued_bytes = 0
            pacing_started_at = time.monotonic()
            for offset in range(0, len(pcm), chunk_bytes):
                chunk = pcm[offset : offset + chunk_bytes]
                send_audio_frame(
                    port,
                    FRAME_AUDIO_CHUNK,
                    sequence,
                    chunk,
                    retries=args.retries,
                    buffer_wait_timeout=args.buffer_wait_timeout,
                )
                sequence = next_sequence(sequence)
                queued_bytes += len(chunk)
                queued_seconds = queued_bytes / BYTES_PER_SECOND
                print(
                    f"\rQueued {queued_seconds:6.2f}/{clip.duration_seconds:.2f} s",
                    end="",
                    flush=True,
                )
                target_elapsed = max(
                    0.0, queued_seconds - args.prebuffer_ms / 1000.0
                )
                remaining = target_elapsed - (time.monotonic() - pacing_started_at)
                if remaining > 0:
                    time.sleep(remaining)
            print()
            send_audio_frame(
                port,
                FRAME_AUDIO_END,
                sequence,
                retries=args.retries,
                buffer_wait_timeout=args.buffer_wait_timeout,
            )
            stream_started = False
        except (Exception, KeyboardInterrupt):
            if stream_started:
                best_effort_stop(port, next_sequence(sequence))
            raise
    print(
        f"Queued {clip.duration_seconds:.2f} seconds of audio; "
        "StackChan is draining its speaker queue."
    )


def main() -> int:
    args = parse_args()
    try:
        play(args)
    except KeyboardInterrupt:
        print("\nPlayback interrupted; AudioStop was sent.", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
