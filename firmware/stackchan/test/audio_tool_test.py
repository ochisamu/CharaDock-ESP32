#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import io
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import ANY, call, patch


TOOLS_DIR = Path(__file__).parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import protocol_serial  # noqa: E402
import send_audio as tool  # noqa: E402


class BufferFullThenAckSerial:
    def __init__(self):
        self.writes: list[bytes] = []
        self.response = bytearray()

    def write(self, request: bytes) -> int:
        request = bytes(request)
        self.writes.append(request)
        frame_type = request[3]
        sequence = struct.unpack_from("<H", request, 4)[0]
        if len(self.writes) == 1:
            response_type = protocol_serial.FRAME_ERROR
            response_payload = bytes([frame_type, 6, 0, 5])
        else:
            response_type = protocol_serial.FRAME_ACK
            response_payload = bytes([frame_type, 0, 0, 0])
        self.response.extend(
            protocol_serial.encode_frame(
                response_type, sequence, response_payload
            )
        )
        return len(request)

    def flush(self) -> None:
        pass

    def read(self, count: int) -> bytes:
        block = bytes(self.response[:count])
        del self.response[:count]
        return block


def wav_bytes(*, rate: int = tool.SAMPLE_RATE, frames: int = 320) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(rate)
        destination.writeframes(struct.pack(f"<{frames}h", *([1000] * frames)))
    return output.getvalue()


class AudioToolTests(unittest.TestCase):
    def test_audio_config_matches_firmware_layout(self) -> None:
        self.assertEqual(
            tool.audio_config_payload(72, 200),
            struct.pack("<BIBBBHB", 1, 16000, 1, 16, 72, 200, 3),
        )

    def test_edge_fade_reaches_silence_without_changing_the_middle(self) -> None:
        source = struct.pack("<320h", *([1000] * 320))
        faded = tool.apply_edge_fades(source, 5)
        samples = struct.unpack("<320h", faded)
        self.assertEqual(samples[0], 0)
        self.assertEqual(samples[-1], 0)
        self.assertEqual(samples[160], 1000)
        self.assertGreater(samples[79], 900)
        self.assertGreater(samples[-80], 900)

    def test_wave_loader_accepts_only_the_wire_format(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            valid_path = Path(directory) / "valid.wav"
            invalid_path = Path(directory) / "invalid.wav"
            valid_path.write_bytes(wav_bytes())
            invalid_path.write_bytes(wav_bytes(rate=24000))
            clip = tool.load_pcm_wave(valid_path)
            self.assertEqual(clip.frame_count, 320)
            self.assertEqual(len(clip.pcm), 640)
            with self.assertRaisesRegex(ValueError, "16 kHz mono PCM16"):
                tool.load_pcm_wave(invalid_path)

    def test_diagnostic_tone_is_bounded_and_has_silent_edges(self) -> None:
        clip = tool.diagnostic_tone_clip()
        samples = struct.unpack(f"<{clip.frame_count}h", clip.pcm)
        self.assertAlmostEqual(clip.duration_seconds, 1.16, places=2)
        self.assertEqual(samples[0], 0)
        self.assertEqual(samples[-1], 0)
        self.assertLessEqual(max(abs(sample) for sample in samples), 5000)

    def test_buffer_full_retries_the_exact_same_frame(self) -> None:
        serial_port = BufferFullThenAckSerial()
        tool.send_audio_frame(
            serial_port,
            tool.FRAME_AUDIO_CHUNK,
            42,
            b"\x01\x00\x02\x00",
            retry_delay=0,
            response_timeout=0.1,
        )
        self.assertEqual(len(serial_port.writes), 2)
        self.assertEqual(serial_port.writes[0], serial_port.writes[1])

    def test_missing_ack_retries_the_same_sequence_and_payload(self) -> None:
        acknowledgement = protocol_serial.Acknowledgement(
            response_type=protocol_serial.FRAME_ACK,
            sequence=17,
            request_type=tool.FRAME_AUDIO_CHUNK,
            apply_result=0,
            portrait_result=0,
            audio_result=0,
        )
        with patch.object(
            tool,
            "exchange_frame",
            side_effect=[TimeoutError("lost acknowledgement"), acknowledgement],
        ) as exchange:
            tool.send_audio_frame(
                object(),
                tool.FRAME_AUDIO_CHUNK,
                17,
                b"\x01\x00",
                retries=1,
                retry_delay=0,
            )
        self.assertEqual(
            exchange.call_args_list,
            [
                call(
                    ANY,
                    tool.FRAME_AUDIO_CHUNK,
                    17,
                    b"\x01\x00",
                    timeout_seconds=3.0,
                ),
                call(
                    ANY,
                    tool.FRAME_AUDIO_CHUNK,
                    17,
                    b"\x01\x00",
                    timeout_seconds=3.0,
                ),
            ],
        )

    def test_sequence_wrap_skips_zero(self) -> None:
        self.assertEqual(tool.next_sequence(10), 11)
        self.assertEqual(tool.next_sequence(0xFFFF), 1)


if __name__ == "__main__":
    unittest.main()
